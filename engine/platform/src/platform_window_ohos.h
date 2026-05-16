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

#ifndef DM_PLATFORM_WINDOW_OHOS_H
#define DM_PLATFORM_WINDOW_OHOS_H

#include "window.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
// OHOS native window comes from <native_window/external_window.h> via the
// XComponent NAPI bindings (OH_NativeXComponent_GetNativeWindow). The full
// surface acquisition flow is documented in FORK_NOTES.md §3.

namespace dmPlatform
{
    // Mirror of the Android namespace so engine code that already calls
    // dmPlatform::Get*Android() can be ported file-by-file with a
    // mechanical s/Android/Ohos/ substitution.
    int32_t      OhosVerifySurface(HWindow window);
    void         OhosBeginFrame(HWindow window);

    EGLContext   GetOhosEGLContext();
    EGLSurface   GetOhosEGLSurface();
    bool         GetSafeAreaOhos(HWindow window, WindowSafeArea* out);

    // SetOhosInputMethod / SetOhosFullscreenParameters are intentionally
    // omitted until ArkUI's input-method handle becomes available to
    // native code.
}

#endif // DM_PLATFORM_WINDOW_OHOS_H
