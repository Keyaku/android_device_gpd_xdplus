# AOSP source patches for xdplus LOS 18.1

Edits to upstream AOSP/LineageOS repos this port needs (can't live in the device tree). Apply from the repo root before `mka bacon`. See `../../../../docs/PORTING_LOG.md` for the why of each.

| Patch | Repo | What |
| --- | --- | --- |
| 0001 | `system/core` | Null-guard `SubcontextChildReap` — init SIGSEGV on pre-P (API-27) vendor. |
| 0002 | `system/core` | logd cap_set_proc non-fatal — 3.18 kernel lacks ambient caps; restores logcat. |
| 0003 | `system/security` | keystore: only enumerate Keymaster3 if no Keymaster4 exists — avoids CHECK-abort on the unloadable vendor keymaster@3.0. |
| 0004 | `frameworks/base` | `KeyStore` null-`mBinder` guard — belt-and-suspenders against system_server NPE when keystore is unavailable. |
| 0005 | `frameworks/base` | hwui `GLUtils::dumpGLErrors` non-fatal — old PowerVR driver leaves benign `GL_INVALID_ENUM` after frames; `GL_CHECKPOINT(LOW)` otherwise aborted every app RenderThread. |
| 0006 | `system/core` | init: ignore `capabilities` rc lines on pre-ambient kernels — parser left an EMPTY capset behind, execing netd/wificond/logd with zero caps (netd socket() EACCES → no wifi/network). |
| 0007 | `frameworks/base` | WindowOrientationListener: propose `persist.sys.flat_rotation` while device rests flat — accel is in the clamshell base, so on-table (normal gaming pose) reads flat and stock keeps last rotation. Prop unset/-1 = stock behavior. |
| 0008 | `system/core` | gatekeeperd: accept the MT8173 TEE trustlet's 61-byte password handle (AOSP's `password_handle_t` is 58) — relaxes three strict `!= sizeof` length checks to `<`. Without it, enroll is rejected ("HAL returned password handle of invalid length 61"), PIN never persists, and HW keymaster auth-bound keys fail `BeginOperation:-26`. Needed by the TEE gatekeeper HAL (§31). |

Apply:

```
cd <ANDROID_ROOT>
git -C system/core apply device/gpd/xdplus/patches/0001-*.patch
git -C system/core apply device/gpd/xdplus/patches/0002-*.patch
git -C system/security apply device/gpd/xdplus/patches/0003-*.patch
git -C frameworks/base apply device/gpd/xdplus/patches/0004-*.patch
git -C frameworks/base apply device/gpd/xdplus/patches/0005-*.patch
git -C system/core apply device/gpd/xdplus/patches/0006-*.patch
git -C frameworks/base apply device/gpd/xdplus/patches/0007-*.patch
git -C system/core apply device/gpd/xdplus/patches/0008-*.patch
```
