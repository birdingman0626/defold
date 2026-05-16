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

// OHOS-direct dmPlatform implementation.
//
// Bypasses GLFW entirely — the engine's platform API doesn't need
// GLFW middleware, that's just how Android happens to be wired. Here
// we talk to OH_NativeXComponent + EGL + OHNativeWindow directly.
//
// Lifecycle:
//   ArkTS XComponent ──register──> OH_NativeXComponent_RegisterCallback
//                                       │
//                                       v
//                      OnSurfaceCreated_CB  → OhosCreateEGLSurface(window)
//                      OnSurfaceChanged_CB  → OhosResize(w, h)
//                      OnSurfaceDestroyed_CB → OhosDestroyEGLSurface()
//                      DispatchTouchEvent_CB → push into g_TouchQueue
//
//   engine main loop calls dmPlatform::SwapBuffers → eglSwapBuffers
//
// Companion file: napi_init.cpp wires the NAPI module entry and
// receives the XComponent callbacks; it forwards them into this
// translation unit via the Ohos* statics below.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <dlib/log.h>
#include <dlib/math.h>

#include "window.h"
#include "window.hpp"
#include "platform_window_constants.h"
#include "platform_window_ohos.h"

// OHOS native types are forward-declared here so we don't pull
// system headers into every consumer of dmPlatform.
struct OH_NativeXComponent;
struct NativeWindow;

extern "C" {
    // Implemented by napi_init.cpp.
    void  OhosNapi_RegisterPlatform();
    void  OhosNapi_SetActiveSurface(NativeWindow* window, uint32_t width, uint32_t height);
    void  OhosNapi_ClearActiveSurface();
}

namespace dmPlatform
{
    static const uint32_t MAX_TOUCH_POINTS = 10;

    struct Window
    {
        EGLDisplay m_Display;
        EGLConfig  m_Config;
        EGLContext m_Context;
        EGLSurface m_Surface;
        NativeWindow* m_NativeWindow;

        uint32_t   m_Width;
        uint32_t   m_Height;
        uint32_t   m_OriginalWidth;
        uint32_t   m_OriginalHeight;

        float      m_DisplayScaleFactor;
        uint32_t   m_SwapInterval;

        // Touch state — populated by napi_init.cpp via the
        // DispatchTouchEvent_CB callback. Engine consumes via
        // GetTouchData() each frame.
        WindowTouchData m_TouchData[MAX_TOUCH_POINTS];
        uint32_t        m_TouchCount;

        // Keyboard/IME/gamepad callback slots (engine sets these but
        // we don't currently surface any events).
        FWindowAddKeyboardCharCallback   m_KeyboardCharCb;
        void*                            m_KeyboardCharUserData;
        FWindowSetMarkedTextCallback     m_MarkedTextCb;
        void*                            m_MarkedTextUserData;
        FWindowDeviceChangedCallback     m_DeviceChangedCb;
        void*                            m_DeviceChangedUserData;
        FWindowGamepadEventCallback      m_GamepadEventCb;
        void*                            m_GamepadEventUserData;

        // Captured from WindowCreateParams at OpenWindow time.
        FWindowResizeCallback            m_ResizeCb;
        void*                            m_ResizeUserData;
        FWindowCloseCallback             m_CloseCb;
        void*                            m_CloseUserData;
        FWindowFocusCallback             m_FocusCb;
        void*                            m_FocusUserData;
        FWindowIconifyCallback           m_IconifyCb;
        void*                            m_IconifyUserData;

        bool       m_Opened;
        bool       m_Iconified;
        bool       m_Focused;
    };

    // The singleton window. OHOS apps have exactly one engine surface
    // (one XComponent per UIAbility) so we don't bother with a pool.
    static Window* g_Window = NULL;

    HWindow NewWindow()
    {
        if (g_Window)
        {
            dmLogWarning("OHOS NewWindow() called twice; reusing existing window.");
            return (HWindow)g_Window;
        }

        Window* w = new Window();
        memset(w, 0, sizeof(Window));
        w->m_Display = EGL_NO_DISPLAY;
        w->m_Context = EGL_NO_CONTEXT;
        w->m_Surface = EGL_NO_SURFACE;
        w->m_DisplayScaleFactor = 1.0f;
        w->m_SwapInterval = 1;
        w->m_Focused = true;
        g_Window = w;

        // Tell the NAPI module about us so it can route XComponent
        // callbacks back via OhosNapi_SetActiveSurface().
        OhosNapi_RegisterPlatform();

        return (HWindow)w;
    }

    void DeleteWindow(HWindow window)
    {
        Window* w = (Window*)window;
        if (!w) return;
        if (w == g_Window) g_Window = NULL;
        delete w;
    }

    static bool InitEGL(Window* w)
    {
        w->m_Display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (w->m_Display == EGL_NO_DISPLAY)
        {
            dmLogError("OHOS: eglGetDisplay failed: 0x%x", eglGetError());
            return false;
        }

        EGLint major = 0, minor = 0;
        if (!eglInitialize(w->m_Display, &major, &minor))
        {
            dmLogError("OHOS: eglInitialize failed: 0x%x", eglGetError());
            return false;
        }
        dmLogInfo("OHOS: EGL initialized %d.%d", major, minor);

        const EGLint config_attribs[] = {
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_ALPHA_SIZE,      8,
            EGL_DEPTH_SIZE,      24,
            EGL_STENCIL_SIZE,    8,
            EGL_NONE
        };

        EGLint num_configs = 0;
        if (!eglChooseConfig(w->m_Display, config_attribs, &w->m_Config, 1, &num_configs)
            || num_configs == 0)
        {
            dmLogError("OHOS: eglChooseConfig failed: 0x%x", eglGetError());
            return false;
        }

        const EGLint ctx_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        w->m_Context = eglCreateContext(w->m_Display, w->m_Config, EGL_NO_CONTEXT, ctx_attribs);
        if (w->m_Context == EGL_NO_CONTEXT)
        {
            dmLogError("OHOS: eglCreateContext failed: 0x%x", eglGetError());
            return false;
        }

        return true;
    }

    static bool BindSurface(Window* w)
    {
        if (!w->m_NativeWindow)
        {
            dmLogWarning("OHOS BindSurface: no native window yet, deferring.");
            return false;
        }
        w->m_Surface = eglCreateWindowSurface(w->m_Display, w->m_Config,
                                              (EGLNativeWindowType)w->m_NativeWindow, NULL);
        if (w->m_Surface == EGL_NO_SURFACE)
        {
            dmLogError("OHOS: eglCreateWindowSurface failed: 0x%x", eglGetError());
            return false;
        }

        if (!eglMakeCurrent(w->m_Display, w->m_Surface, w->m_Surface, w->m_Context))
        {
            dmLogError("OHOS: eglMakeCurrent failed: 0x%x", eglGetError());
            return false;
        }

        eglSwapInterval(w->m_Display, (EGLint)w->m_SwapInterval);
        return true;
    }

    WindowResult OpenWindow(HWindow window, const WindowCreateParams& params)
    {
        Window* w = (Window*)window;
        if (w->m_Opened) return WINDOW_RESULT_WINDOW_ALREADY_OPENED;

        w->m_Width  = params.m_Width;
        w->m_Height = params.m_Height;
        w->m_OriginalWidth  = params.m_Width;
        w->m_OriginalHeight = params.m_Height;

        w->m_ResizeCb        = params.m_ResizeCallback;
        w->m_ResizeUserData  = params.m_ResizeCallbackUserData;
        w->m_CloseCb         = params.m_CloseCallback;
        w->m_CloseUserData   = params.m_CloseCallbackUserData;
        w->m_FocusCb         = params.m_FocusCallback;
        w->m_FocusUserData   = params.m_FocusCallbackUserData;
        w->m_IconifyCb       = params.m_IconifyCallback;
        w->m_IconifyUserData = params.m_IconifyCallbackUserData;

        if (!InitEGL(w))
        {
            return WINDOW_RESULT_WINDOW_OPEN_ERROR;
        }

        // Surface may already be set if XComponent fired
        // OnSurfaceCreated_CB before OpenWindow ran. Otherwise
        // BindSurface() will be called again from
        // OhosNapi_SetActiveSurface when the XComponent surface
        // arrives.
        if (w->m_NativeWindow)
        {
            if (!BindSurface(w)) return WINDOW_RESULT_WINDOW_OPEN_ERROR;
        }

        w->m_Opened = true;
        return WINDOW_RESULT_OK;
    }

    void CloseWindow(HWindow window)
    {
        Window* w = (Window*)window;
        if (!w || !w->m_Opened) return;

        if (w->m_Display != EGL_NO_DISPLAY)
        {
            eglMakeCurrent(w->m_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (w->m_Surface != EGL_NO_SURFACE) eglDestroySurface(w->m_Display, w->m_Surface);
            if (w->m_Context != EGL_NO_CONTEXT) eglDestroyContext(w->m_Display, w->m_Context);
            eglTerminate(w->m_Display);
        }

        w->m_Surface = EGL_NO_SURFACE;
        w->m_Context = EGL_NO_CONTEXT;
        w->m_Display = EGL_NO_DISPLAY;
        w->m_Opened  = false;

        OhosNapi_ClearActiveSurface();
    }

    void SwapBuffers(HWindow window)
    {
        Window* w = (Window*)window;
        if (!w || w->m_Surface == EGL_NO_SURFACE) return;
        if (!eglSwapBuffers(w->m_Display, w->m_Surface))
        {
            dmLogWarning("OHOS: eglSwapBuffers failed: 0x%x", eglGetError());
        }
    }

    void PollEvents(HWindow window)
    {
        // Input events arrive asynchronously via XComponent callbacks
        // (see napi_init.cpp). Nothing to drain here.
        (void)window;
    }

    uint32_t GetWindowWidth(HWindow window)  { Window* w=(Window*)window; return w?w->m_Width:0; }
    uint32_t GetWindowHeight(HWindow window) { Window* w=(Window*)window; return w?w->m_Height:0; }

    void SetWindowSize(HWindow window, uint32_t width, uint32_t height)
    {
        Window* w = (Window*)window;
        if (!w) return;
        w->m_Width  = width;
        w->m_Height = height;
        if (w->m_ResizeCb) w->m_ResizeCb(w->m_ResizeUserData, width, height);
    }

    void SetWindowPosition(HWindow window, int32_t x, int32_t y) { (void)window; (void)x; (void)y; }
    void SetWindowTitle(HWindow window, const char* title)       { (void)window; (void)title; }

    void ShowWindow(HWindow window)    { (void)window; }
    void HideWindow(HWindow window)    { (void)window; }
    void IconifyWindow(HWindow window) { (void)window; }

    uint32_t GetWindowStateParam(HWindow window, WindowState state)
    {
        Window* w = (Window*)window;
        if (!w) return 0;
        switch (state)
        {
            case WINDOW_STATE_OPENED:       return w->m_Opened ? 1 : 0;
            case WINDOW_STATE_ACTIVE:       return w->m_Focused ? 1 : 0;
            case WINDOW_STATE_ICONIFIED:    return w->m_Iconified ? 1 : 0;
            case WINDOW_STATE_ACCELERATED:  return 1;
            case WINDOW_STATE_RED_BITS:     return 8;
            case WINDOW_STATE_GREEN_BITS:   return 8;
            case WINDOW_STATE_BLUE_BITS:    return 8;
            case WINDOW_STATE_ALPHA_BITS:   return 8;
            case WINDOW_STATE_DEPTH_BITS:   return 24;
            case WINDOW_STATE_STENCIL_BITS: return 8;
            case WINDOW_STATE_REFRESH_RATE: return 60;
            case WINDOW_STATE_SAMPLE_COUNT: return 1;
            case WINDOW_STATE_HIGH_DPI:     return 1;
            case WINDOW_STATE_AUX_CONTEXT:  return 0;
            default: return 0;
        }
    }

    float GetDisplayScaleFactor(HWindow window)
    {
        Window* w = (Window*)window;
        return w ? w->m_DisplayScaleFactor : 1.0f;
    }

    uintptr_t GetProcAddress(HWindow window, const char* proc_name)
    {
        (void)window;
        return (uintptr_t)eglGetProcAddress(proc_name);
    }

    void SetSwapInterval(HWindow window, uint32_t swap_interval)
    {
        Window* w = (Window*)window;
        if (!w) return;
        w->m_SwapInterval = swap_interval;
        if (w->m_Display != EGL_NO_DISPLAY)
        {
            eglSwapInterval(w->m_Display, (EGLint)swap_interval);
        }
    }

    int32_t GetKey(HWindow window, int32_t code)              { (void)window; (void)code; return 0; }
    int32_t GetMouseButton(HWindow window, int32_t button)    { (void)window; (void)button; return 0; }
    int32_t GetMouseWheel(HWindow window)                     { (void)window; return 0; }
    void    GetMousePosition(HWindow window, int32_t* x, int32_t* y) { (void)window; *x=0; *y=0; }

    uint32_t GetTouchData(HWindow window, WindowTouchData* touch_data, uint32_t touch_data_count)
    {
        Window* w = (Window*)window;
        if (!w || !touch_data) return 0;
        uint32_t n = dmMath::Min(w->m_TouchCount, touch_data_count);
        for (uint32_t i = 0; i < n; ++i)
        {
            touch_data[i] = w->m_TouchData[i];
        }
        w->m_TouchCount = 0;
        return n;
    }

    bool GetAcceleration(HWindow window, float* x, float* y, float* z)
    {
        (void)window;
        if (x) *x = 0; if (y) *y = 0; if (z) *z = 0;
        return false;
    }

    bool GetSafeArea(HWindow window, WindowSafeArea* out)
    {
        Window* w = (Window*)window;
        if (!w || !out) return false;
        memset(out, 0, sizeof(*out));
        out->m_Width  = w->m_Width;
        out->m_Height = w->m_Height;
        return true;
    }

    const char* GetJoystickDeviceName(HWindow w, uint32_t i) { (void)w; (void)i; return NULL; }
    const char* GetJoystickDeviceGuid(HWindow w, uint32_t i) { (void)w; (void)i; return NULL; }
    uint32_t GetJoystickAxes(HWindow w, uint32_t i, float* v, uint32_t c)    { (void)w; (void)i; (void)v; (void)c; return 0; }
    uint32_t GetJoystickHats(HWindow w, uint32_t i, uint8_t* v, uint32_t c)  { (void)w; (void)i; (void)v; (void)c; return 0; }
    uint32_t GetJoystickButtons(HWindow w, uint32_t i, uint8_t* v, uint32_t c){(void)w; (void)i; (void)v; (void)c; return 0; }

    void SetDeviceState(HWindow window, WindowDeviceState state, bool op1)             { (void)window; (void)state; (void)op1; }
    void SetDeviceState(HWindow window, WindowDeviceState state, bool op1, bool op2)   { (void)window; (void)state; (void)op1; (void)op2; }
    bool GetDeviceState(HWindow window, WindowDeviceState state)                       { (void)window; (void)state; return false; }
    bool GetDeviceState(HWindow window, WindowDeviceState state, int32_t op1)          { (void)window; (void)state; (void)op1; return false; }

    void SetKeyboardCharCallback(HWindow window, FWindowAddKeyboardCharCallback cb, void* user_data)
    {
        Window* w = (Window*)window;
        if (!w) return;
        w->m_KeyboardCharCb = cb;
        w->m_KeyboardCharUserData = user_data;
    }
    void SetKeyboardMarkedTextCallback(HWindow window, FWindowSetMarkedTextCallback cb, void* user_data)
    {
        Window* w = (Window*)window;
        if (!w) return;
        w->m_MarkedTextCb = cb;
        w->m_MarkedTextUserData = user_data;
    }
    void SetKeyboardDeviceChangedCallback(HWindow window, FWindowDeviceChangedCallback cb, void* user_data)
    {
        Window* w = (Window*)window;
        if (!w) return;
        w->m_DeviceChangedCb = cb;
        w->m_DeviceChangedUserData = user_data;
    }
    void SetGamepadEventCallback(HWindow window, FWindowGamepadEventCallback cb, void* user_data)
    {
        Window* w = (Window*)window;
        if (!w) return;
        w->m_GamepadEventCb = cb;
        w->m_GamepadEventUserData = user_data;
    }

    int32_t TriggerCloseCallback(HWindow window)
    {
        Window* w = (Window*)window;
        if (w && w->m_CloseCb) return w->m_CloseCb(w->m_CloseUserData);
        return 1;
    }

    void* AcquireAuxContext(HWindow window)            { (void)window; return NULL; }
    void  UnacquireAuxContext(HWindow window, void* c) { (void)window; (void)c; }

    // ──────────────────────────────────────────────────────────────────
    // OHOS-specific entry points (declared in platform_window_ohos.h)
    // Called from napi_init.cpp on XComponent surface-lifecycle events.

    int32_t OhosVerifySurface(HWindow window)
    {
        Window* w = (Window*)window;
        return (w && w->m_Surface != EGL_NO_SURFACE) ? 1 : 0;
    }

    void OhosBeginFrame(HWindow window)
    {
        Window* w = (Window*)window;
        if (!w || w->m_Surface == EGL_NO_SURFACE) return;
        eglMakeCurrent(w->m_Display, w->m_Surface, w->m_Surface, w->m_Context);
    }

    EGLContext GetOhosEGLContext()
    {
        return g_Window ? g_Window->m_Context : EGL_NO_CONTEXT;
    }

    EGLSurface GetOhosEGLSurface()
    {
        return g_Window ? g_Window->m_Surface : EGL_NO_SURFACE;
    }

    bool GetSafeAreaOhos(HWindow window, WindowSafeArea* out)
    {
        return GetSafeArea(window, out);
    }
}

// ──────────────────────────────────────────────────────────────────
// Out-of-namespace hooks called by napi_init.cpp.

extern "C" {

void OhosPlatform_SetNativeSurface(NativeWindow* native_window, uint32_t width, uint32_t height)
{
    using namespace dmPlatform;
    if (!g_Window) return;
    g_Window->m_NativeWindow = native_window;
    g_Window->m_Width  = width;
    g_Window->m_Height = height;
    if (g_Window->m_Opened && g_Window->m_Surface == EGL_NO_SURFACE)
    {
        BindSurface(g_Window);
    }
    if (g_Window->m_ResizeCb)
    {
        g_Window->m_ResizeCb(g_Window->m_ResizeUserData, width, height);
    }
}

void OhosPlatform_ClearNativeSurface()
{
    using namespace dmPlatform;
    if (!g_Window) return;
    if (g_Window->m_Display != EGL_NO_DISPLAY && g_Window->m_Surface != EGL_NO_SURFACE)
    {
        eglMakeCurrent(g_Window->m_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(g_Window->m_Display, g_Window->m_Surface);
        g_Window->m_Surface = EGL_NO_SURFACE;
    }
    g_Window->m_NativeWindow = NULL;
}

void OhosPlatform_PushTouchEvent(int32_t id, int32_t phase, float x, float y)
{
    using namespace dmPlatform;
    if (!g_Window) return;
    if (g_Window->m_TouchCount >= MAX_TOUCH_POINTS) return;
    WindowTouchData& t = g_Window->m_TouchData[g_Window->m_TouchCount++];
    t.m_Id    = id;
    t.m_Phase = phase;
    t.m_X     = (int32_t)x;
    t.m_Y     = (int32_t)y;
}

} // extern "C"
