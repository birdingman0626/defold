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

// NAPI module entry for the OHOS engine .so.
//
// ArkTS side (entry/src/main/ets/pages/Index.ets) creates an
// XComponent with id "defold". On surface-created we register
// callbacks here that forward into the dmPlatform OHOS hooks.
//
// Exported NAPI functions:
//   * engineStart()       — kicks off the engine main loop on a thread
//   * engineStop()        — graceful shutdown

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>

#include <dlib/log.h>

// Forward decls to platform_window_ohos.cpp.
extern "C" {
    void OhosPlatform_SetNativeSurface(void* native_window, uint32_t width, uint32_t height);
    void OhosPlatform_ClearNativeSurface();
    void OhosPlatform_PushTouchEvent(int32_t id, int32_t phase, float x, float y);

    // common/main.cpp ultimately calls engine_main; we route through
    // it so we share lifecycle with the desktop main entry.
    int engine_main(int argc, char* argv[]);
    void OhosNapi_RegisterPlatform() {}      // dmPlatform calls into us; nothing to do for now
    void OhosNapi_SetActiveSurface(void* native_window, uint32_t width, uint32_t height)
    {
        OhosPlatform_SetNativeSurface(native_window, width, height);
    }
    void OhosNapi_ClearActiveSurface()
    {
        OhosPlatform_ClearNativeSurface();
    }
}

static OH_NativeXComponent_Callback g_xcomp_callback = {};
static pthread_t                    g_engine_thread  = 0;
static volatile int                 g_engine_running = 0;

// Touch phase values for the engine's HID layer.
// Defold's WindowTouchData::m_Phase uses 0=Pressed, 1=Moved,
// 2=Released, 3=Cancelled (see hid.h DM_INPUT_PHASE_*).
enum {
    OHOS_PHASE_PRESSED   = 0,
    OHOS_PHASE_MOVED     = 1,
    OHOS_PHASE_RELEASED  = 2,
    OHOS_PHASE_CANCELLED = 3,
};

// ──────────────────────────────────────────────────────────────────
// XComponent surface lifecycle callbacks.

static void OnSurfaceCreatedCB(OH_NativeXComponent* xcomp, void* window)
{
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(xcomp, window, &w, &h);
    dmLogInfo("OHOS: XComponent surface created %lux%lu", (unsigned long)w, (unsigned long)h);
    OhosPlatform_SetNativeSurface(window, (uint32_t)w, (uint32_t)h);
}

static void OnSurfaceChangedCB(OH_NativeXComponent* xcomp, void* window)
{
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(xcomp, window, &w, &h);
    dmLogInfo("OHOS: XComponent surface resized to %lux%lu", (unsigned long)w, (unsigned long)h);
    OhosPlatform_SetNativeSurface(window, (uint32_t)w, (uint32_t)h);
}

static void OnSurfaceDestroyedCB(OH_NativeXComponent* xcomp, void* window)
{
    (void)xcomp; (void)window;
    dmLogInfo("OHOS: XComponent surface destroyed");
    OhosPlatform_ClearNativeSurface();
}

static void DispatchTouchEventCB(OH_NativeXComponent* xcomp, void* window)
{
    (void)window;
    OH_NativeXComponent_TouchEvent touch = {};
    int32_t res = OH_NativeXComponent_GetTouchEvent(xcomp, window, &touch);
    if (res != 0) return;

    int32_t engine_phase = OHOS_PHASE_MOVED;
    switch (touch.type)
    {
        case OH_NATIVEXCOMPONENT_DOWN:   engine_phase = OHOS_PHASE_PRESSED;   break;
        case OH_NATIVEXCOMPONENT_UP:     engine_phase = OHOS_PHASE_RELEASED;  break;
        case OH_NATIVEXCOMPONENT_MOVE:   engine_phase = OHOS_PHASE_MOVED;     break;
        case OH_NATIVEXCOMPONENT_CANCEL: engine_phase = OHOS_PHASE_CANCELLED; break;
        default: return;
    }
    OhosPlatform_PushTouchEvent(touch.id, engine_phase, touch.x, touch.y);
}

// ──────────────────────────────────────────────────────────────────
// Engine thread entry.

static void* EngineThreadMain(void* arg)
{
    (void)arg;
    char arg0[] = "dmengine";
    char* argv[] = { arg0, NULL };
    int rc = engine_main(1, argv);
    dmLogInfo("OHOS: engine_main exited with code %d", rc);
    g_engine_running = 0;
    return NULL;
}

// ──────────────────────────────────────────────────────────────────
// NAPI exports.

static napi_value EngineStart(napi_env env, napi_callback_info info)
{
    if (g_engine_running) return NULL;
    g_engine_running = 1;
    pthread_create(&g_engine_thread, NULL, EngineThreadMain, NULL);
    return NULL;
}

static napi_value EngineStop(napi_env env, napi_callback_info info)
{
    if (!g_engine_running) return NULL;
    // Soft stop. The engine will see the flag during its next pump.
    g_engine_running = 0;
    // pthread_join would block the JS thread; leave it to exit
    // naturally as the engine drains.
    return NULL;
}

// ──────────────────────────────────────────────────────────────────
// XComponent registration. ArkTS resolves the XComponent by id
// "defold" and attaches our callbacks.

static napi_value AttachXComponent(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {0};
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    napi_value export_instance = NULL;
    napi_get_named_property(env, args[0], OH_NATIVE_XCOMPONENT_OBJ, &export_instance);

    OH_NativeXComponent* xcomp = NULL;
    napi_unwrap(env, export_instance, (void**)&xcomp);
    if (!xcomp)
    {
        dmLogError("OHOS: failed to resolve OH_NativeXComponent from ArkTS");
        return NULL;
    }

    g_xcomp_callback.OnSurfaceCreated   = OnSurfaceCreatedCB;
    g_xcomp_callback.OnSurfaceChanged   = OnSurfaceChangedCB;
    g_xcomp_callback.OnSurfaceDestroyed = OnSurfaceDestroyedCB;
    g_xcomp_callback.DispatchTouchEvent = DispatchTouchEventCB;
    OH_NativeXComponent_RegisterCallback(xcomp, &g_xcomp_callback);
    dmLogInfo("OHOS: XComponent callbacks registered");
    return NULL;
}

// ──────────────────────────────────────────────────────────────────
// Module init.

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        { "attachXComponent", NULL, AttachXComponent, NULL, NULL, NULL, napi_default, NULL },
        { "engineStart",      NULL, EngineStart,      NULL, NULL, NULL, napi_default, NULL },
        { "engineStop",       NULL, EngineStop,       NULL, NULL, NULL, napi_default, NULL },
    };
    napi_define_properties(env, exports, sizeof(desc)/sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module dmengineModule = {
    /*nm_version*/      1,
    /*nm_flags*/        0,
    /*nm_filename*/     NULL,
    /*nm_register_func*/Init,
    /*nm_modname*/      "dmengine",
    /*nm_priv*/         (void*)0,
    /*reserved*/        { 0 },
};

extern "C" __attribute__((constructor)) void RegisterDmengineModule()
{
    napi_module_register(&dmengineModule);
}
