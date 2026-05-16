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

// OHOS GLFW glue placeholder.
//
// The real implementation needs to talk to an XComponent-based ArkTS
// host shell via OH_NativeXComponent_* + NAPI. See
// FORK_NOTES.md §3 for the concrete file list and the ~4,450-line GLFW
// android backend that this mirrors.
//
// Right now every function returns a safe default (NULL/0/false). That
// lets the `platform` static library link cleanly but the engine will
// not produce any output at runtime — it has no surface to draw to.

#include "platform_window_ohos.h"

namespace dmPlatform
{
    int32_t OhosVerifySurface(HWindow window)
    {
        // 0 = surface invalid. Once the real OH_NativeXComponent surface
        // is wired this should return 1 when the XComponent has an
        // attached OHNativeWindow.
        return 0;
    }

    void OhosBeginFrame(HWindow window)
    {
        // No-op. Real impl will eglMakeCurrent() against the OHOS
        // EGL context bound to the XComponent's OHNativeWindow.
    }

    EGLContext GetOhosEGLContext()
    {
        return EGL_NO_CONTEXT;
    }

    EGLSurface GetOhosEGLSurface()
    {
        return EGL_NO_SURFACE;
    }

    bool GetSafeAreaOhos(HWindow window, WindowSafeArea* out)
    {
        // No safe-area data available yet — caller falls back to full
        // window rect.
        return false;
    }
}
