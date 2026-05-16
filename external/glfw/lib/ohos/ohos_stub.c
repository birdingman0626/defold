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

// dmglfw OHOS placeholder. The real XComponent + EGL + NAPI backend
// (analog of external/glfw/lib/android/*) is the multi-day piece
// tracked in FORK_NOTES.md §3.
//
// This file exists so `bld.stlib(... target='dmglfw' ...)` for
// arm64-ohos compiles to a static archive with at least one symbol,
// avoiding the empty-archive warning some toolchains emit. The
// dmengine_headless target doesn't link dmglfw (it uses PLATFORM_NULL
// + GRAPHICS_NULL); the full dmengine target will, and it'll fail
// then with missing GLFW symbols — which is the next break-point.

int _dmglfw_ohos_placeholder(void)
{
    return 0;
}
