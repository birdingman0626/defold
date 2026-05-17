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
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <dlib/log.h>
#include <dlib/math.h>

#include "window.h"
#include "window.hpp"
#include "platform_window_constants.h"
#include "platform_window_ohos_input.h"
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
    // Backwards-compat alias for code that still names the local constant.
    static const uint32_t MAX_TOUCH_POINTS = OHOS_MAX_TOUCH_POINTS;

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

        // Touch / mouse-emulation / synthetic-ESC state. The state machine
        // (per-finger queue, primary-touch tracking, ESC pending) lives in
        // platform_window_ohos_input.h so it can be unit-tested in
        // engine/platform/src/test/test_platform_ohos.cpp without the EGL +
        // XComponent + NativeWindow dependencies the rest of this file
        // brings in.
        OhosInputState m_Input;

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

        // The XComponent surface arrives asynchronously from the UI
        // thread (napi_init.cpp's OnSurfaceCreatedCB). The engine
        // thread blocks in OpenWindow() until m_NativeWindow is
        // non-null. eglMakeCurrent must run on the engine thread that
        // will later issue draws, NOT on the UI thread, so we never
        // touch EGL from inside the napi callbacks.
        pthread_mutex_t m_SurfaceMutex;
        pthread_cond_t  m_SurfaceCv;
    };

    // The singleton window. OHOS apps have exactly one engine surface
    // (one XComponent per UIAbility) so we don't bother with a pool.
    static Window* g_Window = NULL;

    // The XComponent surface lifecycle (OnSurfaceCreated) typically
    // fires from the UI thread BEFORE the engine thread reaches
    // NewWindow(). Cache the latest surface + dims here so NewWindow
    // can adopt them on creation; otherwise OpenWindow's cv wait
    // would time out because the SetNativeSurface call landed before
    // g_Window even existed.
    static pthread_mutex_t g_PendingSurfaceMutex = PTHREAD_MUTEX_INITIALIZER;
    static NativeWindow*   g_PendingNativeWindow = NULL;
    static uint32_t        g_PendingWidth        = 0;
    static uint32_t        g_PendingHeight       = 0;

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
        OhosInputInit(&w->m_Input);
        pthread_mutex_init(&w->m_SurfaceMutex, NULL);
        pthread_cond_init(&w->m_SurfaceCv, NULL);
        g_Window = w;

        // Adopt any surface that arrived from the XComponent UI
        // callback before the engine got here. (Order is typical:
        // OnSurfaceCreated → cached into g_Pending* → NewWindow
        // → moved into w-> fields.) Without this OpenWindow's cv
        // wait would time out even though the surface is available.
        pthread_mutex_lock(&g_PendingSurfaceMutex);
        if (g_PendingNativeWindow)
        {
            pthread_mutex_lock(&w->m_SurfaceMutex);
            w->m_NativeWindow = g_PendingNativeWindow;
            w->m_Width        = g_PendingWidth;
            w->m_Height       = g_PendingHeight;
            pthread_mutex_unlock(&w->m_SurfaceMutex);
            dmLogInfo("OHOS NewWindow: adopted pending surface %ux%u",
                      g_PendingWidth, g_PendingHeight);
        }
        pthread_mutex_unlock(&g_PendingSurfaceMutex);

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

        // Project's logical resolution (e.g. 1920x1080 from
        // game.project's display.width/height). Render scripts use
        // this for their orthographic projection.
        w->m_OriginalWidth  = params.m_Width;
        w->m_OriginalHeight = params.m_Height;

        // Physical render target dims. If the XComponent already
        // delivered a surface size (typical — OnSurfaceCreated fires
        // before engine_main reaches OpenWindow), use that. The GL
        // viewport will get re-sized to this via the ResizeCallback
        // a few lines below. Otherwise fall back to project dims
        // and let the cv wait + ResizeCallback correct it.
        if (w->m_NativeWindow && w->m_Width > 0 && w->m_Height > 0)
        {
            dmLogInfo("OHOS OpenWindow: project=%ux%u surface=%ux%u",
                      params.m_Width, params.m_Height, w->m_Width, w->m_Height);
        }
        else
        {
            w->m_Width  = params.m_Width;
            w->m_Height = params.m_Height;
        }

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

        // Block here on the engine thread until the XComponent
        // surface is delivered by the UI thread
        // (OhosPlatform_SetNativeSurface signals the cv). Required
        // because eglMakeCurrent binds the context to the *calling*
        // thread, so the engine thread must be the one to bind —
        // otherwise the worker draws into an unbound context and
        // every gl* call is a no-op (the original black-screen bug).
        // Cap the wait at 5 s so we fail fast if the host never
        // attaches.
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;

        pthread_mutex_lock(&w->m_SurfaceMutex);
        while (!w->m_NativeWindow)
        {
            int rc = pthread_cond_timedwait(&w->m_SurfaceCv, &w->m_SurfaceMutex, &ts);
            if (rc == ETIMEDOUT) break;
        }
        pthread_mutex_unlock(&w->m_SurfaceMutex);

        if (!w->m_NativeWindow)
        {
            dmLogError("OHOS OpenWindow: timed out waiting for XComponent surface");
            return WINDOW_RESULT_WINDOW_OPEN_ERROR;
        }

        if (!BindSurface(w)) return WINDOW_RESULT_WINDOW_OPEN_ERROR;

        w->m_Opened = true;

        // Tell the engine about the actual XComponent surface size
        // (typically larger than game.project's display.width/height
        // on a tablet/2in1 emulator) so its viewport + render
        // targets match. The render script does the
        // logical->physical scaling.
        if (w->m_ResizeCb && (w->m_Width != params.m_Width || w->m_Height != params.m_Height))
        {
            w->m_ResizeCb(w->m_ResizeUserData, w->m_Width, w->m_Height);
        }
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

    int32_t GetKey(HWindow window, int32_t code)
    {
        Window* w = (Window*)window;
        if (!w) return 0;
        return OhosInputReadKey(&w->m_Input, code);
    }
    int32_t GetMouseButton(HWindow window, int32_t button)
    {
        Window* w = (Window*)window;
        if (!w) return 0;
        return OhosInputReadMouseButton(&w->m_Input, button);
    }
    int32_t GetMouseWheel(HWindow window)                     { (void)window; return 0; }
    void    GetMousePosition(HWindow window, int32_t* x, int32_t* y)
    {
        Window* w = (Window*)window;
        if (!w || !x || !y) { if (x) *x = 0; if (y) *y = 0; return; }
        *x = w->m_Input.m_MouseX;
        *y = w->m_Input.m_MouseY;
    }

    uint32_t GetTouchData(HWindow window, WindowTouchData* touch_data, uint32_t touch_data_count)
    {
        Window* w = (Window*)window;
        if (!w) return 0;
        return OhosInputReadTouches(&w->m_Input, touch_data, touch_data_count);
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

    // dmGraphics::OpenGL calls into this to recover the default
    // framebuffer id (0 on every desktop / mobile GLES platform —
    // the on-screen surface owns FBO 0). Mirrors the glfw3 / glfw
    // platform_window implementations.
    int32_t OpenGLGetDefaultFramebufferId()
    {
        return 0;
    }

    // Key / mouse / joystick constant tables. Values mirror
    // platform_window_null.cpp — the actual numeric values don't
    // matter for OHOS (the engine compares against these symbols),
    // they just need to exist so the dynamic linker can resolve them.
    const int PLATFORM_KEY_START           = 0;
    const int PLATFORM_JOYSTICK_LAST       = 0;
    const int PLATFORM_KEY_ESC             = 1;
    const int PLATFORM_KEY_F1              = 2;
    const int PLATFORM_KEY_F2              = 3;
    const int PLATFORM_KEY_F3              = 4;
    const int PLATFORM_KEY_F4              = 5;
    const int PLATFORM_KEY_F5              = 6;
    const int PLATFORM_KEY_F6              = 7;
    const int PLATFORM_KEY_F7              = 8;
    const int PLATFORM_KEY_F8              = 9;
    const int PLATFORM_KEY_F9              = 10;
    const int PLATFORM_KEY_F10             = 11;
    const int PLATFORM_KEY_F11             = 12;
    const int PLATFORM_KEY_F12             = 13;
    const int PLATFORM_KEY_UP              = 14;
    const int PLATFORM_KEY_DOWN            = 15;
    const int PLATFORM_KEY_LEFT            = 16;
    const int PLATFORM_KEY_RIGHT           = 17;
    const int PLATFORM_KEY_LSHIFT          = 18;
    const int PLATFORM_KEY_RSHIFT          = 19;
    const int PLATFORM_KEY_LCTRL           = 20;
    const int PLATFORM_KEY_RCTRL           = 21;
    const int PLATFORM_KEY_LALT            = 22;
    const int PLATFORM_KEY_RALT            = 23;
    const int PLATFORM_KEY_TAB             = 24;
    const int PLATFORM_KEY_ENTER           = 25;
    const int PLATFORM_KEY_BACKSPACE       = 26;
    const int PLATFORM_KEY_INSERT          = 27;
    const int PLATFORM_KEY_DEL             = 28;
    const int PLATFORM_KEY_PAGEUP          = 29;
    const int PLATFORM_KEY_PAGEDOWN        = 30;
    const int PLATFORM_KEY_HOME            = 31;
    const int PLATFORM_KEY_END             = 32;
    const int PLATFORM_KEY_KP_0            = 33;
    const int PLATFORM_KEY_KP_1            = 34;
    const int PLATFORM_KEY_KP_2            = 35;
    const int PLATFORM_KEY_KP_3            = 36;
    const int PLATFORM_KEY_KP_4            = 37;
    const int PLATFORM_KEY_KP_5            = 38;
    const int PLATFORM_KEY_KP_6            = 39;
    const int PLATFORM_KEY_KP_7            = 40;
    const int PLATFORM_KEY_KP_8            = 41;
    const int PLATFORM_KEY_KP_9            = 42;
    const int PLATFORM_KEY_KP_DIVIDE       = 43;
    const int PLATFORM_KEY_KP_MULTIPLY     = 44;
    const int PLATFORM_KEY_KP_SUBTRACT     = 45;
    const int PLATFORM_KEY_KP_ADD          = 46;
    const int PLATFORM_KEY_KP_DECIMAL      = 47;
    const int PLATFORM_KEY_KP_EQUAL        = 48;
    const int PLATFORM_KEY_KP_ENTER        = 49;
    const int PLATFORM_KEY_KP_NUM_LOCK     = 50;
    const int PLATFORM_KEY_CAPS_LOCK       = 51;
    const int PLATFORM_KEY_SCROLL_LOCK     = 52;
    const int PLATFORM_KEY_PAUSE           = 53;
    const int PLATFORM_KEY_LSUPER          = 54;
    const int PLATFORM_KEY_RSUPER          = 55;
    const int PLATFORM_KEY_MENU            = 56;
    const int PLATFORM_KEY_BACK            = 57;

    const int PLATFORM_MOUSE_BUTTON_LEFT   = 0;
    const int PLATFORM_MOUSE_BUTTON_MIDDLE = 1;
    const int PLATFORM_MOUSE_BUTTON_RIGHT  = 2;
    const int PLATFORM_MOUSE_BUTTON_1      = 3;
    const int PLATFORM_MOUSE_BUTTON_2      = 4;
    const int PLATFORM_MOUSE_BUTTON_3      = 5;
    const int PLATFORM_MOUSE_BUTTON_4      = 6;
    const int PLATFORM_MOUSE_BUTTON_5      = 7;
    const int PLATFORM_MOUSE_BUTTON_6      = 8;
    const int PLATFORM_MOUSE_BUTTON_7      = 9;
    const int PLATFORM_MOUSE_BUTTON_8      = 10;

    const int PLATFORM_JOYSTICK_1          = 0;
    const int PLATFORM_JOYSTICK_2          = 1;
    const int PLATFORM_JOYSTICK_3          = 2;
    const int PLATFORM_JOYSTICK_4          = 3;
    const int PLATFORM_JOYSTICK_5          = 4;
    const int PLATFORM_JOYSTICK_6          = 5;
    const int PLATFORM_JOYSTICK_7          = 6;
    const int PLATFORM_JOYSTICK_8          = 7;
    const int PLATFORM_JOYSTICK_9          = 8;
    const int PLATFORM_JOYSTICK_10         = 9;
    const int PLATFORM_JOYSTICK_11         = 10;
    const int PLATFORM_JOYSTICK_12         = 11;
    const int PLATFORM_JOYSTICK_13         = 12;
    const int PLATFORM_JOYSTICK_14         = 13;
    const int PLATFORM_JOYSTICK_15         = 14;
    const int PLATFORM_JOYSTICK_16         = 15;
}

// ──────────────────────────────────────────────────────────────────
// Out-of-namespace hooks called by napi_init.cpp.

extern "C" {

void OhosPlatform_SetNativeSurface(NativeWindow* native_window, uint32_t width, uint32_t height)
{
    using namespace dmPlatform;

    // Always stash in g_Pending* so a surface arriving before
    // NewWindow() runs gets picked up on creation. This is the
    // common case because OnSurfaceCreated typically fires before
    // ArkTS even gets to engineStart().
    pthread_mutex_lock(&g_PendingSurfaceMutex);
    g_PendingNativeWindow = native_window;
    g_PendingWidth        = width;
    g_PendingHeight       = height;
    pthread_mutex_unlock(&g_PendingSurfaceMutex);

    if (!g_Window) return;

    // Engine-thread side: hand the window to the engine's Window
    // struct and signal OpenWindow's cv. EGL stays untouched here —
    // eglMakeCurrent must run on the engine thread, NOT here on the
    // UI thread, or the engine thread would have no current context.
    pthread_mutex_lock(&g_Window->m_SurfaceMutex);
    g_Window->m_NativeWindow = native_window;
    g_Window->m_Width        = width;
    g_Window->m_Height       = height;
    pthread_cond_signal(&g_Window->m_SurfaceCv);
    pthread_mutex_unlock(&g_Window->m_SurfaceMutex);

    if (g_Window->m_ResizeCb)
    {
        g_Window->m_ResizeCb(g_Window->m_ResizeUserData, width, height);
    }
}

void OhosPlatform_ClearNativeSurface()
{
    using namespace dmPlatform;
    pthread_mutex_lock(&g_PendingSurfaceMutex);
    g_PendingNativeWindow = NULL;
    pthread_mutex_unlock(&g_PendingSurfaceMutex);

    if (!g_Window) return;
    // Also UI thread. Don't tear down EGL here — only clear the
    // pointer. Engine SwapBuffers will no-op once the engine thread
    // notices m_NativeWindow gone; the teardown happens in
    // CloseWindow() on the engine thread.
    pthread_mutex_lock(&g_Window->m_SurfaceMutex);
    g_Window->m_NativeWindow = NULL;
    pthread_mutex_unlock(&g_Window->m_SurfaceMutex);
}

void OhosPlatform_PushTouchEvent(int32_t id, int32_t phase, float x, float y)
{
    // State machine + mouse-emulation logic lives in
    // platform_window_ohos_input.h so the same code is exercised by the
    // host unit test (test_platform_ohos.cpp). The header docstring
    // covers the BEGAN/ENDED pending-queue rationale (sub-frame taps on
    // the slow emulator) and the primary-finger mouse-button emulation.
    using namespace dmPlatform;
    if (!g_Window) return;
    OhosInputPushTouch(&g_Window->m_Input, id, phase, (int32_t)x, (int32_t)y);
}

void OhosPlatform_SetFocus(int32_t focused)
{
    // ArkTS UIAbility.onForeground/onBackground forwards here so the
    // engine can pause sound + skip its render path while we're not
    // the foreground app. dmHID.cpp's iconified path then takes over
    // (see engine.cpp ~line 1900: !engine->m_WasIconified branch).
    using namespace dmPlatform;
    if (!g_Window) return;
    bool f = focused != 0;
    if (g_Window->m_Focused == f) return;
    g_Window->m_Focused = f;
    if (g_Window->m_FocusCb)
    {
        g_Window->m_FocusCb(g_Window->m_FocusUserData, f ? 1 : 0);
    }
}

void OhosPlatform_PushKeyEvent(int32_t code, int32_t pressed)
{
    // Synthetic keyboard event from the ArkTS layer. Today only used
    // for the system back-gesture → KEY_ESC bridge so a back swipe in
    // the game/pause/settings screens fires vn.screens.on_menu_action.
    // The pending-queue logic for sub-frame press/release lives in
    // platform_window_ohos_input.h alongside the unit-tested mouse-button
    // and touch state machines.
    using namespace dmPlatform;
    if (!g_Window) return;
    OhosInputPushKey(&g_Window->m_Input, code, pressed);
}

} // extern "C"
