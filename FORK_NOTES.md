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

---

## 3. Pending — Engine platform-layer port (NOT YET IN FORK)

Multi-week work, OUT OF AUTOMATED SESSION SCOPE. After §2.8 is in place,
`--platform=arm64-ohos` compiles dlib + similar leaf modules cleanly,
then fails at engine/platform link time on missing symbols. Files to
add (mirroring the android variants — see `engine/*/src/platform_*`
naming convention):

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
