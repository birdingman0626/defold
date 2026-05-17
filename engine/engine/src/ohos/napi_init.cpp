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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <unistd.h>

#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>

#include <dlib/log.h>

// Forward decls to platform_window_ohos.cpp.
extern "C" {
    void OhosPlatform_SetNativeSurface(void* native_window, uint32_t width, uint32_t height);
    void OhosPlatform_ClearNativeSurface();
    void OhosPlatform_PushTouchEvent(int32_t id, int32_t phase, float x, float y);
    uint32_t OhosPlatform_GetSurfaceHeight();

    // engine_main.cpp exposes a C-linkage `ohos_engine_main` wrapper
    // around the C++-linkage `engine_main`. We need the C name so
    // the dlopen-driven dynamic linker can find it without C++
    // mangling.
    int ohos_engine_main(int argc, char* argv[]);
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

// Touch phase values, mirrored from dmHID::Phase
// (see engine/hid/src/dmsdk/hid/hid.h): BEGAN=0, MOVED=1,
// STATIONARY=2, ENDED=3, CANCELLED=4. HID copies these straight
// from WindowTouchData::m_Phase, so the wire values must match.
enum {
    OHOS_PHASE_PRESSED   = 0,
    OHOS_PHASE_MOVED     = 1,
    OHOS_PHASE_RELEASED  = 3,
    OHOS_PHASE_CANCELLED = 4,
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
    if (res != 0)
    {
        dmLogWarning("OHOS: GetTouchEvent failed: %d", res);
        return;
    }

    int32_t engine_phase = OHOS_PHASE_MOVED;
    switch (touch.type)
    {
        case OH_NATIVEXCOMPONENT_DOWN:   engine_phase = OHOS_PHASE_PRESSED;   break;
        case OH_NATIVEXCOMPONENT_UP:     engine_phase = OHOS_PHASE_RELEASED;  break;
        case OH_NATIVEXCOMPONENT_MOVE:   engine_phase = OHOS_PHASE_MOVED;     break;
        case OH_NATIVEXCOMPONENT_CANCEL: engine_phase = OHOS_PHASE_CANCELLED; break;
        default: return;
    }
    // OHOS XComponent reports touch in surface coords with origin
    // top-left. That's exactly what engine.cpp's GOActionCallback
    // expects when it does `input_action.m_Y = m_Height - action_y *
    // height_ratio` to convert into Defold's bottom-up design coords —
    // so we hand the raw top-down y through and let the engine do the
    // flip (any extra flip here would double-invert and the GUI hit
    // tests would land below the button row instead of on it).
    dmLogInfo("OHOS: touch %d phase=%d x=%.0f y=%.0f",
              touch.id, engine_phase, touch.x, touch.y);
    OhosPlatform_PushTouchEvent(touch.id, engine_phase, touch.x, touch.y);
}

// ──────────────────────────────────────────────────────────────────
// Engine thread entry.

// Set from ArkTS via dmengine.engineStart(filesDir). ArkTS owns the
// rawfile -> filesDir extraction step (it has the
// context.resourceManager API), then hands us the absolute path
// where game.projectc / game.arci / game.arcd / game.dmanifest live.
static char g_FilesDir[1024] = "";

static void* EngineThreadMain(void* arg)
{
    (void)arg;

    char cwd[1024] = "";
    if (getcwd(cwd, sizeof(cwd))) dmLogInfo("OHOS: getcwd = %s", cwd);

    if (g_FilesDir[0] == 0)
    {
        dmLogError("OHOS: engineStart called without a filesDir — no game.projectc to load");
        g_engine_running = 0;
        return NULL;
    }

    // Hand argv[0] = "<filesDir>/dmengine". dmSysPosix's
    // GetResourcesPath() does dirname(argv[0]) so the engine ends up
    // looking for game.projectc under <filesDir>/. The "dmengine"
    // basename is a dummy — only the directory matters.
    static char arg0_path[1280];
    snprintf(arg0_path, sizeof(arg0_path), "%s/dmengine", g_FilesDir);
    dmLogInfo("OHOS: engine resources path = %s", g_FilesDir);

    char* argv[] = { arg0_path, NULL };
    int rc = ohos_engine_main(1, argv);
    dmLogInfo("OHOS: engine_main exited with code %d", rc);
    g_engine_running = 0;
    return NULL;
}

// ──────────────────────────────────────────────────────────────────
// NAPI exports.

static napi_value EngineStart(napi_env env, napi_callback_info info)
{
    if (g_engine_running) return NULL;

    // First arg is the absolute path to the app's writable files
    // directory (ArkTS already extracted game.projectc + .arc* into
    // it). Required — see EngineThreadMain.
    size_t argc = 1;
    napi_value args[1] = {0};
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (argc >= 1)
    {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], g_FilesDir, sizeof(g_FilesDir), &len);
    }

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
    int32_t rc = OH_NativeXComponent_RegisterCallback(xcomp, &g_xcomp_callback);
    dmLogInfo("OHOS: XComponent callbacks registered (attach path), xcomp=%p rc=%d", xcomp, rc);
    return NULL;
}

// ──────────────────────────────────────────────────────────────────
// Module init.

// Resolve the framework-injected OH_NativeXComponent off `exports`
// and register our surface lifecycle callbacks against it. This is
// the path taken when ArkTS declares XComponent({libraryname:
// 'dmengine', ...}) — OHOS plugs the XComponent into our module's
// exports before Init runs. Doing it here (instead of from
// ArkTS-side dmengine.attachXComponent) means our OnSurfaceCreated
// callback is in place by the time the OHOS XComponent fires its
// "surface created" event, which happens before ArkTS onLoad.
static void RegisterXComponentFromExports(napi_env env, napi_value exports)
{
    napi_value export_instance = NULL;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &export_instance) != napi_ok)
    {
        dmLogWarning("OHOS: no OH_NATIVE_XCOMPONENT_OBJ on exports — libraryname may not be 'dmengine'");
        return;
    }

    OH_NativeXComponent* xcomp = NULL;
    if (napi_unwrap(env, export_instance, (void**)&xcomp) != napi_ok || !xcomp)
    {
        dmLogWarning("OHOS: failed to unwrap OH_NativeXComponent from exports");
        return;
    }

    g_xcomp_callback.OnSurfaceCreated   = OnSurfaceCreatedCB;
    g_xcomp_callback.OnSurfaceChanged   = OnSurfaceChangedCB;
    g_xcomp_callback.OnSurfaceDestroyed = OnSurfaceDestroyedCB;
    g_xcomp_callback.DispatchTouchEvent = DispatchTouchEventCB;
    int32_t rc = OH_NativeXComponent_RegisterCallback(xcomp, &g_xcomp_callback);
    dmLogInfo("OHOS: XComponent callbacks registered (libraryname path), xcomp=%p rc=%d", xcomp, rc);
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        { "attachXComponent", NULL, AttachXComponent, NULL, NULL, NULL, napi_default, NULL },
        { "engineStart",      NULL, EngineStart,      NULL, NULL, NULL, napi_default, NULL },
        { "engineStop",       NULL, EngineStop,       NULL, NULL, NULL, napi_default, NULL },
    };
    napi_define_properties(env, exports, sizeof(desc)/sizeof(desc[0]), desc);
    RegisterXComponentFromExports(env, exports);
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
