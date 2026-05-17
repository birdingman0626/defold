// Copyright 2020-2026 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <dlib/array.h>
#include <dlib/log.h>
#include <dlib/math.h>
#include <dlib/mutex.h>

#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostream_base.h>

#include "sound.h"
#include "sound_private.h"

// OHOS audio device backend.
//
// Pulls from the same three-queue (Free → Ready → Playing) model the
// OpenSL backend uses on Android, but OH_AudioRenderer drives the
// pull from a write-data callback instead of an enqueue-driven one.
//
// Engine thread:  DeviceQueue() pushes filled buffers from Free →
//                 Ready (signals the renderer callback to consume).
// Audio thread:   WriteDataCallback() pops Ready → Playing, memcpys
//                 into the audioData buffer the OS asks for, then
//                 pushes Playing → Free for the engine to reuse.

namespace dmDeviceOhos
{
    struct Buffer
    {
        void*    m_Buffer;
        uint32_t m_FrameCount;
        uint32_t m_FrameCapacity;

        Buffer()
        {
            memset(this, 0, sizeof(*this));
        }

        Buffer(void* buffer, uint32_t frame_capacity)
        {
            m_Buffer        = buffer;
            m_FrameCount    = 0;
            m_FrameCapacity = frame_capacity;
        }
    };

    // Trivial ring-buffer over a dmArray<Buffer>. Identical to
    // device_opensl.cpp's Queue — kept inline because the type is
    // backend-private.
    struct Queue
    {
        dmArray<Buffer> m_Queue;
        uint32_t        m_Size;
        int             m_Start;
        int             m_End;

        Queue() : m_Size(0), m_Start(0), m_End(0) {}

        void SetCapacity(uint32_t capacity)
        {
            m_Queue.SetCapacity(capacity);
            m_Queue.SetSize(capacity);
            m_Size = 0;
            m_Start = 0;
            m_End = 0;
        }

        void Push(const Buffer& b)
        {
            m_Queue[m_End] = b;
            m_End = (m_End + 1) % m_Queue.Size();
            m_Size++;
        }

        Buffer Pop()
        {
            int i = m_Start;
            m_Start = (m_Start + 1) % m_Queue.Size();
            m_Size--;
            return m_Queue[i];
        }

        uint32_t Size() const { return m_Size; }
    };

    struct OhosDevice
    {
        uint32_t                m_MixRate;
        uint32_t                m_FrameCount;
        uint32_t                m_BufferCount;

        // Filled (by engine) buffers waiting for the renderer to drain.
        // Empty buffers ready for the engine to refill.
        // Buffer the renderer is currently consuming (1-slot).
        Queue                   m_Free;
        Queue                   m_Ready;
        Buffer                  m_Active;
        uint32_t                m_ActiveOffsetFrames;

        dmMutex::HMutex         m_Mutex;

        OH_AudioStreamBuilder*  m_Builder;
        OH_AudioRenderer*       m_Renderer;
        bool                    m_IsPlaying;
    };

    static OH_AudioData_Callback_Result WriteDataCallback(OH_AudioRenderer* renderer,
                                                          void*             userData,
                                                          void*             audioData,
                                                          int32_t           audioDataSize)
    {
        OhosDevice* dev = (OhosDevice*)userData;
        int16_t*    out = (int16_t*)audioData;
        // OH_Audio asks for raw bytes; we're stereo 16-bit, so 4 bytes/frame.
        int32_t     frames_wanted = audioDataSize / 4;
        int32_t     frames_written = 0;

        dmMutex::Lock(dev->m_Mutex);
        while (frames_written < frames_wanted)
        {
            if (dev->m_Active.m_Buffer == NULL || dev->m_ActiveOffsetFrames >= dev->m_Active.m_FrameCount)
            {
                if (dev->m_Active.m_Buffer != NULL)
                {
                    // Active drained — recycle to free pool.
                    Buffer done = dev->m_Active;
                    done.m_FrameCount = 0;
                    dev->m_Free.Push(done);
                    dev->m_Active.m_Buffer = NULL;
                    dev->m_ActiveOffsetFrames = 0;
                }
                if (dev->m_Ready.Size() == 0)
                {
                    break;
                }
                dev->m_Active = dev->m_Ready.Pop();
                dev->m_ActiveOffsetFrames = 0;
            }

            int32_t frames_remaining = dev->m_Active.m_FrameCount - dev->m_ActiveOffsetFrames;
            int32_t to_copy = dmMath::Min((int32_t)(frames_wanted - frames_written), frames_remaining);
            int16_t* src = (int16_t*)dev->m_Active.m_Buffer + (dev->m_ActiveOffsetFrames * 2);
            memcpy(out + frames_written * 2, src, to_copy * 4);
            frames_written           += to_copy;
            dev->m_ActiveOffsetFrames += to_copy;
        }
        dmMutex::Unlock(dev->m_Mutex);

        if (frames_written < frames_wanted)
        {
            // Underrun — pad with silence so OH doesn't replay stale data.
            memset(out + frames_written * 2, 0, (frames_wanted - frames_written) * 4);
        }
        return AUDIO_DATA_CALLBACK_RESULT_VALID;
    }

    dmSound::Result DeviceOhosOpen(const dmSound::OpenDeviceParams* params, dmSound::HDevice* device)
    {
        const uint32_t mix_rate     = 44100;
        const uint32_t channels     = 2;
        uint32_t       frame_count  = params->m_FrameCount;
        if (frame_count == 0)
        {
            frame_count = dmSound::GetDefaultFrameCount(mix_rate);
        }
        const uint32_t buffer_count = params->m_BufferCount > 0 ? params->m_BufferCount : 4;

        OH_AudioStreamBuilder* builder = NULL;
        OH_AudioStream_Result  res     = OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER);
        if (res != AUDIOSTREAM_SUCCESS)
        {
            dmLogError("OHOS audio: Builder_Create failed: %d", (int)res);
            return dmSound::RESULT_UNKNOWN_ERROR;
        }
        OH_AudioStreamBuilder_SetSamplingRate(builder, (int32_t)mix_rate);
        OH_AudioStreamBuilder_SetChannelCount(builder, (int32_t)channels);
        OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
        OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
        OH_AudioStreamBuilder_SetLatencyMode(builder,  AUDIOSTREAM_LATENCY_MODE_NORMAL);
        OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_GAME);

        OhosDevice* dev = new OhosDevice();
        memset(dev, 0, sizeof(*dev));
        dev->m_MixRate     = mix_rate;
        dev->m_FrameCount  = frame_count;
        dev->m_BufferCount = buffer_count;
        dev->m_Mutex       = dmMutex::New();
        dev->m_Free.SetCapacity(buffer_count);
        dev->m_Ready.SetCapacity(buffer_count);

        for (uint32_t i = 0; i < buffer_count; ++i)
        {
            uint32_t bytes = frame_count * channels * sizeof(int16_t);
            Buffer b(malloc(bytes), frame_count);
            dev->m_Free.Push(b);
        }

        res = OH_AudioStreamBuilder_SetRendererWriteDataCallback(builder, WriteDataCallback, dev);
        if (res != AUDIOSTREAM_SUCCESS)
        {
            dmLogError("OHOS audio: SetRendererWriteDataCallback failed: %d", (int)res);
        }

        res = OH_AudioStreamBuilder_GenerateRenderer(builder, &dev->m_Renderer);
        if (res != AUDIOSTREAM_SUCCESS)
        {
            dmLogError("OHOS audio: GenerateRenderer failed: %d", (int)res);
            OH_AudioStreamBuilder_Destroy(builder);
            dmMutex::Delete(dev->m_Mutex);
            delete dev;
            return dmSound::RESULT_UNKNOWN_ERROR;
        }
        dev->m_Builder = builder;
        *device = dev;
        return dmSound::RESULT_OK;
    }

    void DeviceOhosClose(dmSound::HDevice device)
    {
        OhosDevice* dev = (OhosDevice*)device;
        if (!dev) return;
        if (dev->m_Renderer)
        {
            OH_AudioRenderer_Stop(dev->m_Renderer);
            OH_AudioRenderer_Release(dev->m_Renderer);
        }
        if (dev->m_Builder)
        {
            OH_AudioStreamBuilder_Destroy(dev->m_Builder);
        }
        // Drain whichever queue still holds buffers.
        for (;;)
        {
            Buffer b;
            if (dev->m_Free.Size() > 0)        b = dev->m_Free.Pop();
            else if (dev->m_Ready.Size() > 0)  b = dev->m_Ready.Pop();
            else if (dev->m_Active.m_Buffer != NULL) { b = dev->m_Active; dev->m_Active.m_Buffer = NULL; }
            else break;
            free(b.m_Buffer);
        }
        dmMutex::Delete(dev->m_Mutex);
        delete dev;
    }

    dmSound::Result DeviceOhosQueue(dmSound::HDevice device, const void* samples, uint32_t sample_count)
    {
        OhosDevice* dev = (OhosDevice*)device;
        if (!dev || !dev->m_IsPlaying) return dmSound::RESULT_INIT_ERROR;

        DM_MUTEX_SCOPED_LOCK(dev->m_Mutex);
        if (dev->m_Free.Size() == 0) return dmSound::RESULT_OUT_OF_BUFFERS;

        Buffer b = dev->m_Free.Pop();
        uint32_t bytes = sample_count * 2 * sizeof(int16_t);
        memcpy(b.m_Buffer, samples, bytes);
        b.m_FrameCount = sample_count;
        dev->m_Ready.Push(b);
        return dmSound::RESULT_OK;
    }

    uint32_t DeviceOhosFreeBufferSlots(dmSound::HDevice device)
    {
        OhosDevice* dev = (OhosDevice*)device;
        if (!dev) return 0;
        DM_MUTEX_SCOPED_LOCK(dev->m_Mutex);
        return dev->m_Free.Size();
    }

    void DeviceOhosDeviceInfo(dmSound::HDevice device, dmSound::DeviceInfo* info)
    {
        OhosDevice* dev = (OhosDevice*)device;
        info->m_MixRate          = dev->m_MixRate;
        info->m_FrameCount       = dev->m_FrameCount;
        info->m_DSPImplementation = dmSound::DSPIMPL_TYPE_CPU;
    }

    void DeviceOhosStart(dmSound::HDevice device)
    {
        OhosDevice* dev = (OhosDevice*)device;
        if (!dev || dev->m_IsPlaying) return;
        OH_AudioStream_Result res = OH_AudioRenderer_Start(dev->m_Renderer);
        if (res != AUDIOSTREAM_SUCCESS)
        {
            dmLogError("OHOS audio: Renderer_Start failed: %d", (int)res);
            return;
        }
        dev->m_IsPlaying = true;
    }

    void DeviceOhosStop(dmSound::HDevice device)
    {
        OhosDevice* dev = (OhosDevice*)device;
        if (!dev || !dev->m_IsPlaying) return;
        OH_AudioRenderer_Stop(dev->m_Renderer);
        dev->m_IsPlaying = false;
    }

    DM_DECLARE_SOUND_DEVICE(DefaultSoundDevice, "default",
                            DeviceOhosOpen, DeviceOhosClose, DeviceOhosQueue,
                            DeviceOhosFreeBufferSlots, 0, DeviceOhosDeviceInfo,
                            DeviceOhosStart, DeviceOhosStop);
}
