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
  lands together with the toolchain block (Task §3).

---

## 3. Pending — OHOS toolchain + engine port (NOT YET IN FORK)

Tracked but not committed:

- **`build_tools/sdk.py`** — add `install_sdk` branch for `arm64-ohos`
  that downloads or locates the OpenHarmony native SDK
  (`OHOS_NDK_PATH`, `OHOS_NDK_BIN_PATH`, `OHOS_NDK_SYSROOT` env vars).
- **`build_tools/waf_dynamo.py`** — add `elif TargetOS.OHOS == target_os:`
  block injecting:
  - `-DDM_PLATFORM_OHOS`
  - `-target aarch64-linux-ohos`, `--sysroot=$OHOS_NDK_SYSROOT`,
    `-D__MUSL__`
  - clang++ from `$OHOS_NDK_BIN_PATH/clang++.exe`
- **Engine platform layer** (multi-week, OUT OF AUTOMATED SESSION SCOPE):
  - `engine/platform/src/platform_window_*.cpp` — XComponent surface
    (instead of GLFW or Android NativeActivity)
  - `engine/graphics/src/opengl/graphics_opengl.cpp` — EGL+GLESv2 init
  - `engine/hid/src/...` — input from ArkUI bridge
  - `engine/sound/src/...` — OpenSL ES is available, mirror Android
  - `engine/sys/src/...` — file paths, system info via OHOS NAPI
- **bob `.hap` bundle producer** (multi-day):
  - new `com.dynamo.cr.bob/src/com/dynamo/bob/bundle/OhosBundler.java`
    that templates ArkTS entry skeleton, copies engine `.so`, runs
    `hvigorw assembleHap`, signs with `hap-sign-tool.jar`.

See also the extender fork's §8 (the `arm64-ohos:` recipe is already
prototyped locally in `D:\DevTools\extender\sdk\.../defoldsdk/extender/build.yml`,
not yet committed).
