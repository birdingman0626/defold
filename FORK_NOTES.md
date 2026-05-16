# Fork Notes — birdingman0626/defold

This fork diverges from upstream `defold/defold` to add **native
OpenHarmony / HarmonyOS Next (`arm64-ohos`) engine support**.

- **Base commit at last sync:** `17b9565` (`dev`, "New translations
  en.editor_localization (Chinese Simplified)" — most recent upstream
  commit before fork divergence).
- **Strategy:** small commits, each kept compile-clean for the existing
  control platform (arm64-android) so fork CI stays green between steps.
  See `.github/workflows/fork-build.yml`.
- **Companion forks:**
  - `birdingman0626/extender` — provides the `arm64-ohos` recipe in
    `share/extender/build_input.yml` (see that fork's `FORK_NOTES.md`
    §8).
  - `D:\Workspace\VNProj\defold_vn\ohos\` — the HarmonyOS HAP shell
    that hosts the eventual engine `.so`.

**Maintainership intent:** continuous merge from `upstream/dev` is expected.
This document is the rebase ledger.

---

## How to merge from upstream

```bash
cd D:\DevTools\defold-engine
git fetch upstream
git merge upstream/dev      # or: git rebase upstream/dev
# resolve conflicts using this document as a guide
git push origin dev         # auto-triggers fork-build CI
```

After pushing, watch:
- https://github.com/birdingman0626/defold/actions/workflows/fork-build.yml

---

## 1. Fork-only CI workflow (MUST KEEP)

### 1.1 `.github/workflows/fork-build.yml`
**Why:** Upstream's `.github/workflows/main-ci.yml` depends on
Defold-internal secrets that don't exist on this fork (`AWS_*`,
`SLACK_WEBHOOK`, `NOTARIZATION_*`). Every push from this fork to
`dev` failed CI before this workflow existed.

**What it does:**
- Triggers on push to `dev` or `ohos/**` branches, or on
  `workflow_dispatch` with a `platform` chooser
  (`arm64-android` / `armv7-android` / `arm64-ohos`).
- Runs `ci/ci.sh install --platform=...` then
  `ci/ci.sh ... --skip-tests --skip-builtins --skip-docs engine`.
- No `--archive`, no S3 upload, no Slack notifications, no
  notarization.

**Validated green builds:**
- `25953358754` arm64-android — initial workflow add (8m4s)
- `25953362435` arm64-android — manual dispatch sanity (7m19s)
- `25953551557` arm64-android — after OHOS platform constants (8m38s)

Conflict resolution: this is a pure addition, no upstream file to
conflict with. Safe to keep on any rebase.

---

## 2. OHOS platform registration

These four commits register `arm64-ohos` as a known target across
the proto / Java / Python layers. They do **not** add any toolchain
or platform-layer source code — that's stage 3 (still pending).

### 2.1 `engine/graphics/proto/graphics/graphics_ddf.proto`
- Added `OS_ID_OHOS = 11` to the `PlatformProfile.OS` enum.
- Upstream conflict risk: if upstream adds new `OS_ID_*` values,
  rebase to the next free integer.

### 2.2 `com.dynamo.cr/com.dynamo.cr.bob/src/com/dynamo/bob/PlatformArchitectures.java`
- Added `OHOS(new String[] {"arm64-ohos"}, ...)` enum value.

### 2.3 `com.dynamo.cr/com.dynamo.cr.bob/src/com/dynamo/bob/Platform.java`
- Added `public static final Platform Arm64Ohos` mirroring the
  `Arm64Android` definition (lib-prefix `lib`, lib-suffix `.so`).

### 2.4 `build_tools/build_constants.py`
- Added `OHOS: str = 'ohos'` to the `TargetOSContants` NamedTuple.

### 2.5 `build_tools/BuildUtility.py`
- Added `{'platform': 'arm64-ohos', 'os': TargetOS.OHOS, 'arch': 'arm64'}`
  to `_supported_platforms`.

### 2.6 `scripts/build.py`
- Added `'arm64-ohos'` to `BASE_PLATFORMS`.

### 2.7 `engine/extension/src/dmsdk/extension/extension.h`
- Added `DM_PLATFORM_OHOS` docstring placeholder. The actual
  `-DDM_PLATFORM_OHOS` injection in `build_tools/waf_dynamo.py`
  is in §2.8.

### 2.8 `build_tools/sdk.py` + `build_tools/waf_dynamo.py`
- `sdk.py`: three `arm64-ohos` branches added —
  `_get_defold_sdk_folders`, `check_local_sdk`, `_get_defold_sdk_info`,
  `_get_local_sdk_info`. All resolve OHOS toolchain from env vars
  `OHOS_NDK_PATH` / `OHOS_NDK_BIN_PATH` / `OHOS_NDK_SYSROOT` (no
  download support — set by caller). `check_local_sdk` raises a
  clear `SDKException` if `OHOS_NDK_PATH` is missing.
- `waf_dynamo.py`: `elif TargetOS.OHOS == target_os:` branch added.
  Injects `-target aarch64-linux-ohos`, `--sysroot=$OHOS_NDK_SYSROOT`,
  `-D__MUSL__`, `-DDM_PLATFORM_OHOS`, the Android-like flag set
  (`-fpic -ffunction-sections -fno-rtti -static-libstdc++`), and
  points `CC`/`CXX` at `$OHOS_NDK_BIN_PATH/clang*`. On Windows uses
  `clang++.exe` directly to bypass the POSIX shell-wrapper shim.

### 2.9 `com.dynamo.cr.bob/src/com/dynamo/bob/bundle/OhosBundler.java`
- Minimum-viable `IBundler` for `arm64-ohos` (auto-registered via
  `Project.doScan`'s classpath search). Copies the engine `.so`,
  shared libs, archive, and bundle resources into a flat output
  directory that the project's `ohos/` ArkTS shell consumes via
  `hvigorw assembleHap`.
- Does NOT yet produce a `.hap` directly — that requires templating
  module.json5 + invoking `hvigorw` + signing with `hap-sign-tool.jar`,
  which is deferred until an arm64-ohos engine `.so` actually exists
  to package (§3 engine port).

### 2.10 `share/extender/build_input.yml`
- Added `ohos:` base block (env, engineLibs, dynamicLibs, file
  patterns, allowedFlags, libCmd) and `arm64-ohos:` arch-specific
  block (NDK_CXX, defines, compileCmd, linkCmd). Mirrors arm64-android
  structure.
- Build output ships in every defoldsdk tarball as
  `defoldsdk/extender/build.yml`; the extender server picks it up
  automatically. This replaces the previously-uncommitted local
  prototype that lived in `D:\DevTools\extender\sdk\.../defoldsdk/
  extender/build.yml`.
- engineLibs list references `.a` files under
  `{{dynamo_home}}/lib/arm64-ohos` that don't yet exist; link will
  fail there with "library not found: libengine.a" until §3 produces
  them. That's the next break-point.

### 2.11 wscript dispatch wiring for arm64-ohos

Routes the existing `posix` / `linux` / `Android-equivalent` source
files for OHOS so `--platform=arm64-ohos build_engine` advances
past the per-subsystem dispatch and only fails when it hits the
missing platform-specific code in §3.

- `build_tools/cross.py` — add `'ohos'` to the fallback-tag list
  that maps to `posix`. Unblocks dlib's socket / sys / mutex /
  thread / time / condition_variable / file_descriptor.
- `engine/dlib/src/wscript` — add `'ohos': ['mbedtls']` to the
  sslsocket extra_tags dict (HTTPS works via the same mbedtls
  build that Android/Linux use).
- `engine/extension/src/wscript` — comment-only; no
  `extension_ohos.cpp` yet (no lifecycle-hook surface yet).
- `engine/sound/src/wscript` — route OHOS to
  `devices/device_opensl.cpp`. OpenSL ES exists on OHOS.
- `engine/sound/src/devices/device_opensl.cpp` — wrap
  `GetSampleRate()` JNI body in `#if !defined(DM_PLATFORM_OHOS)`
  with a hardcoded `44100` fallback for OHOS (OpenSL resamples
  internally if device rate differs).
- `engine/crash/src/wscript` — OHOS reuses libunwind +
  `load_addrs_proc_smap.cpp` + `file_posix.cpp` (matches Android).
- `engine/profiler/src/wscript` — OHOS reuses `profiler_linux.cpp`
  + `profiler_proc_utils.cpp` (musl exposes /proc the same way).
- `engine/engine/src/wscript` — OHOS added to UNWIND/CPP_RUNTIME
  list and to no-OPENAL list (it uses OpenSL ES via §2.11
  device_opensl).
- `build_tools/waf_dynamo.py apply_apk_test` — also convert
  `cprogram` → `cshlib` on OHOS so the engine ships as
  `libdmengine.so` for ArkTS to load via XComponent.

After this section, every Defold engine subsystem that has a
purely-POSIX or Linux-compatible variant is selected for OHOS. The
remaining gap is the truly platform-different code in §3.

### 2.12 platform_window_glfw_ohos.cpp + header placeholder
- `engine/platform/src/platform_window_ohos.h` — declares
  `dmPlatform::OhosVerifySurface/OhosBeginFrame/GetOhosEGLContext/
  GetOhosEGLSurface/GetSafeAreaOhos`. Mirrors the Android header but
  omits the JNI/ANativeWindow-specific functions; OHOS will need
  NAPI + OHNativeWindow getters when those are implemented.
- `engine/platform/src/platform_window_glfw_ohos.cpp` — every
  function returns `NULL` / `0` / `false`. Lets the `platform`
  static library link cleanly for arm64-ohos; engine produces no
  output at runtime because there's no surface to draw to. Real
  XComponent-backed implementation is the multi-day piece in §3.
- `engine/platform/src/wscript` routes arm64-ohos to the new .cpp
  and installs the new header.

### 2.13 external/glfw/lib/ohos/ohos_stub.c
- Single C placeholder symbol so `bld.stlib(target='dmglfw' ...)`
  builds a non-empty static archive for arm64-ohos. `dmengine_headless`
  doesn't link dmglfw at all (uses PLATFORM_NULL + GRAPHICS_NULL);
  full `dmengine` will link it and fail with missing GLFW symbols
  until §3 lands the real backend.

### 2.14 OHOS-direct dmPlatform implementation (~400 LOC)
- `engine/platform/src/platform_window_ohos.cpp` — full
  `dmPlatform::*` implementation that bypasses GLFW. Singleton
  Window struct, real EGL setup (`eglGetDisplay` → `eglInitialize` →
  `eglChooseConfig` → `eglCreateContext`, surface bound from
  XComponent's OHNativeWindow), trivial stubs for keyboard/mouse/
  joystick, touch buffer drained per-frame via `GetTouchData`.
- `engine/platform/src/wscript` arm64-ohos branch now builds the
  `platform` lib from `window.cpp + platform_window_ohos.cpp` only —
  skips `platform_window_glfw.cpp` entirely.
- The old `platform_window_glfw_ohos.cpp` stub is deleted.

### 2.15 NAPI module entry — XComponent lifecycle bridge
- `engine/engine/src/ohos/napi_init.cpp` — exports
  `attachXComponent / engineStart / engineStop` to ArkTS, registers
  `OH_NativeXComponent_RegisterCallback` handlers that forward into
  `OhosPlatform_*` C hooks (defined in platform_window_ohos.cpp).
  Spawns `engine_main()` on a pthread when ArkTS calls
  `engineStart()`.
- `engine/engine/src/wscript` includes ohos/napi_init.cpp in the
  `engine` static lib on arm64-ohos.
- `build_tools/waf_dynamo.py` + `share/extender/build_input.yml`
  add `ace_napi.z`, `ace_ndk.z`, `hilog_ndk.z`, `native_window` to
  the OHOS link line.

### 2.16 ArkTS host shell switched to XComponent
*Lives in `defold_vn`, not in this fork. Committed at
`b99ab79` (defold_vn main branch).*
- `ohos/entry/src/main/ets/pages/Index.ets` — now creates an
  `XComponent` with id "defold" and calls
  `dmengine.attachXComponent(context)` + `engineStart()` on load.
  The previous WebView wrapper is preserved at
  `Index.webview.ets.bak`.
- `EntryAbility.ets` drops the `customizeSchemes(dvn://)` block
  (no Web component → no CORS workaround needed).
- `scripts/build_ohos.ps1` switched to the native pipeline:
  bob.jar arm64-ohos bundle → copy `.so` to libs/arm64-v8a → hvigor →
  sign.

### 2.17 Diagnostic milestone
On the local Windows host with the OHOS NDK at
`D:\DevTools\command-line-tools\sdk\default\openharmony\native`,
`python scripts/build.py --platform=arm64-ohos check_sdk` now
advances past SDK lookup successfully (only failing on missing
`protoc`, which install_ext would provide). Confirms the
toolchain wiring + sdk.py + build_constants routing all work
end-to-end for the OHOS path.

---

## 3. Pending — Engine platform-layer port (PARTIAL / IN PROGRESS)

The work in §2 routes everything that *can* fall back to posix/linux.
What's left is the genuinely platform-different code: ArkTS host shell,
XComponent surface acquisition, EGL+GLESv2 context tied to the
XComponent's `OHNativeWindow`, NAPI bindings for OS lookups, and the
GLFW backend (`external/glfw/lib/ohos/`) that mirrors the ~4,450-line
android one.

Estimated remaining work: 40+ engineering hours, much of which is
emulator-iteration cycles (build → hdc install → hilog → fix → repeat,
typically 10-30 cycles to converge).

Files still to add (mirroring the android variants — see
`engine/*/src/platform_*` naming convention):

  - **`engine/platform/src/platform_window_ohos.cpp`** — surface
    backed by OHOS XComponent (instead of GLFW or Android
    NativeActivity).
  - **`engine/graphics/src/opengl/graphics_opengl_ohos.cpp`** — EGL +
    GLESv2 init. ArkWeb already proved OHOS GLESv2 works; this just
    wires it to a native EGL context.
  - **`engine/hid/src/hid_ohos.cpp`** — touch + sensor input from
    ArkUI's input bridge.
  - **`engine/sound/src/sound_ohos.cpp`** — OpenSL ES is available;
    can mostly copy the Android backend.
  - **`engine/sys/src/sys_ohos.cpp`** — file paths, locale, system
    info via OHOS NAPI.

After the engine .so exists, extend `OhosBundler` (§2.9) to template
ArkTS entry + run `hvigorw assembleHap` + sign.

The extender-side recipe lives in §2.10 above. Any defoldsdk built
from this fork after commit `120c575` ships the OHOS recipe
automatically — no need to keep the local extender SDK cache in sync
by hand.

---

## 4. Engineering handoff — step-by-step path to "runs on emulator"

The work in §1–§2 leaves the build system fully OHOS-aware and the
engine .so should now link as `libdmengine.so` for arm64-ohos (with
all-NULL platform/graphics). The path from there to actually rendering
the VN sample on the emulator is roughly:

### Step 1 — Validate the empty .so builds (½ day)

Set OHOS_NDK_PATH + OHOS_NDK_BIN_PATH + OHOS_NDK_SYSROOT, then
`./scripts/build.py --platform=arm64-ohos shell` and inside that
shell `python scripts/build.py --platform=arm64-ohos install_ext
install_sdk build_engine`. Iterate on whatever compile/link errors
surface (will be many — unfixed symbols inside files that compile but
weren't speculative-tested). When `dmengine_headless` for arm64-ohos
links to a .so, that's milestone 1.

### Step 2 — Port the GLFW backend (~3-5 days)

Mirror `external/glfw/lib/android/` into `external/glfw/lib/ohos/`.
The mapping is well-defined:

| Android concept                | OHOS equivalent                                |
|--------------------------------|------------------------------------------------|
| `ANativeWindow*`               | `OHNativeWindow*` (from `<native_window/external_window.h>`) |
| `android_app*` / `native_app_glue` | OH_NativeXComponent_Callback (from `<ace/xcomponent/native_interface_xcomponent.h>`) |
| JNI `JNIEnv` / `jobject`       | NAPI `napi_env` / `napi_value`                  |
| `ALooper`                      | OHOS event loop (uv_async via NAPI bridge)     |
| `AInputEvent`                  | XComponent touch callbacks (OnTouchEvent_CB)   |
| `ANativeActivity_onCreate()`   | XComponent register-callback NAPI export       |

Files to write (count + Android-equivalent LOC for sizing):
- `ohos_init.c` (~1300 LOC) — EGL+GLES context init, XComponent surface
- `ohos_window.c` (~900 LOC) — surface lifecycle (create/resize/destroy)
- `ohos_native_app_glue.c` (~450 LOC) — XComponent lifecycle ↔ engine main loop bridge
- `ohos_jni.c` → `ohos_napi.c` (~300 LOC) — NAPI bindings for OS lookups
- `ohos_joystick.c`, `ohos_thread.c`, `ohos_time.c`, `ohos_util.{c,h}`, `ohos_enable.c`, `ohos_glext.c` — typically each <200 LOC, mostly direct ports
- `platform.h` — OHOS-specific platform struct (mirrors `android/platform.h`)

### Step 3 — ArkTS host shell (1 day)

In `defold_vn/ohos/entry/src/main/ets/`:
- Replace the existing WebView wrapper with an XComponent.
- ArkTS side registers `OH_NativeXComponent_RegisterCallback` for
  surface lifecycle + touch events.
- Native side (a new tiny `napi_init.cpp` in the engine) receives
  those callbacks, packages them into events the GLFW backend's
  event loop consumes.
- ArkTS calls `engineMain(width, height)` from `OnSurfaceCreated_CB`.

### Step 4 — Wire `OhosBundler.java` to produce a real `.hap` (½ day)

`OhosBundler.java` (§2.9) currently copies the engine .so to a flat
output dir. Extend it to:
- Template `ohos/entry/src/main/module.json5` with the project's
  bundleName + abilityName.
- Run `hvigorw assembleHap` from the templated project.
- Sign with `hap-sign-tool.jar` using project-provided keystore.

### Step 5 — Iterate on the emulator (multi-day)

Build → `hdc install` → `aa start` → `hdc shell hilog` → fix → repeat.
Common first-cycle failures (in expected order):
1. `libdmengine.so` fails to load — missing NAPI export or undefined
   symbol. Fix in `napi_init.cpp` / GLFW backend.
2. Engine launches but XComponent surface acquisition times out.
   Fix lifecycle ordering in ArkTS shell.
3. EGL context creates but `eglMakeCurrent` fails. Usually the
   EGLConfig isn't matching the XComponent's surface format.
4. Rendering proceeds but produces a black frame. GLES extension
   detection (same family of issue as the WebView wrapper had —
   compressed textures, GLSL version).
5. Rendering works but touch input doesn't route. Fix
   `OH_NativeXComponent_RegisterCallback` wiring.

A real engineer with an OHOS device should expect ~10-30 iteration
cycles to converge. The emulator's software GLES will be more painful
than a real device — the prior WebView attempt black-screened on
emulator GLES specifically (see `.agentdocs/build/ohos_webview.md`
in `defold_vn`). A real device is the better target.

### Step 6 — Run the VN sample

`scripts/build_ohos.ps1` already drives the flow for the WebView
wrapper. After Step 4, swap it to:
- `bob.jar --platform=arm64-ohos --architectures=arm64-ohos bundle`
  (uses §2.9 `OhosBundler`).
- Optionally `hdc install` the result.

If §2.9 bundles correctly and §1-3 produce a working engine .so, this
script should produce a `.hap` that boots into the VN title screen on
the emulator.

### Total realistic effort

~6-10 engineering days for a developer who hasn't done OHOS native
work before; ~3-4 days for someone with prior OH_NativeXComponent
experience. Most of the variance comes from emulator GLES bugs vs.
real-device validation.
