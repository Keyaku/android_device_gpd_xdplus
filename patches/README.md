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
| 0009 | `system/core` | Null-guard `SubcontextTerminate` — same pre-P (API-27) vendor null `subcontext` as 0001, but on the reboot path. Without it init SIGSEGVs in every reboot's powerctl handler and its fatal-signal handler force-reboots to `bootloader`/fastboot instead of completing the reboot (the "reboots drop to fastboot" quirk). |
| 0010 | `frameworks/av` | `DeviceHalHidl::supportsAudioPatches` → `false` — the frozen-8.1 MT8173 vendor HAL implements `create_audio_patch` for output but returns `-ENOSYS` for the capture (device→mix) patch. AudioFlinger tolerates that on output (falls back) but treats it as fatal on input, so all recording hangs at 0:00 (`AudioRecord start -38`). Forcing no HAL-patch support makes AF use software patches (`setParameters "routing="` → `audio_route`) everywhere — the mode this legacy HAL natively shipped in. Fixes builtin + headset mic capture (§48). |
| 0011 | `frameworks/base` | `WiredAccessoryManager` accdet plumbing — MT8173 accdet driver never creates `/sys/class/switch/h2w`, so AOSP's `UEventObserver` jack path is dead (no routing switch, speaker never mutes, headset mic never offered). Adds `AccdetObserver`, a light poll of `/sys/bus/platform/drivers/Accdet_Driver/state` (0=out, 1=headset+mic, 2=headphone) that feeds the h2w headset bits through the normal `updateLocked()` path → audio routing, speaker mute, `IN_WIRED_HEADSET` mic, and the `ACTION_HEADSET_PLUG` status-bar icon (§48). |
| 0012 | `frameworks/opt/net/wifi` | `ClientModeImpl.setPowerSave`: pin power-save off when `persist.sys.wifi_ps_pin_off` is set — MTK gen2 driver mishandles 802.11 power-save and APs low-ack-kick the device under sustained transfer load (`DISASSOC_LOW_ACK`, §44/§49). Every framework re-enable path (post-DHCP restore, WifiLockManager) funnels through this method, so an init-rc `iw` write alone gets overridden seconds after each connect. Prop unset = stock behavior. |
| 0013 | `frameworks/opt/net/wifi` | `ClientModeImpl.isWifiBandSupported`: force `false` for `WIFI_BAND_6_GHZ`. Chip is MT6630 (2.4/5 GHz, no 6E radio) but the legacy MTK HAL returns phantom 6 GHz channels, and the `config_wifi6ghzSupport` overlay is true-only (can't force false), so `is6GHzBandSupported()` wrongly reports "Yes" (Athena). 5 GHz left on the normal capability path (2026-07-13 Wi-Fi/Athena triage). |
| 0014 | `frameworks/native` | `InputDispatcher`: kill the ~1 s input lag on the prebuilt 3.18.79 kernel. Two bugs, one symptom — (1) `pokeUserActivityLocked` runs a synchronous `mPolicy->pokeUserActivity()` that stalls the RT dispatcher thread ~1 s on the first event of a burst (queued events pile up, then drain at once); (2) `epoll_wait` with any non-zero timeout intermittently drops the eventfd wake. Fix: `dispatchOnce` uses `pollOnce(0)+usleep(1000)` (non-blocking, ~1 kHz, 2.5 % CPU) and `pokeUserActivityLocked` early-returns. NOT cpuidle/cpusets/schedtune (disproved live). Follow-ups (`docs/TRIAGE_2026-07-14.md`): stub drops the interactive screen-timeout reset (defer off-thread for the clean fix); verify the 1 kHz poll doesn't block suspend. |
| 0016 | `frameworks/native` | `InputDispatcher`: fix patch 0014's side-effect — the fully-stubbed `pokeUserActivityLocked` dropped the interactive user-activity screen-timeout reset, so the screen dimmed/slept during active gamepad play. Root cause of the original lag: `pokeUserActivity()` bottoms out in a synchronous power-HAL `sendPowerHint(INTERACTION)` binder call that stalls ~1 s on the 3.18.79+ kernel, blocking the RT dispatcher thread. This patch offloads the poke to a dedicated `InputUserActivity` thread (coalesced, condvar-driven): the dispatcher never blocks (lag stays fixed) AND user activity again resets the screen timeout. Verified: `mLastUserActivityTime` advances on input, screen stays awake, no dispatcher stalls under burst input. Depends on 0014. |
| 0015 | `frameworks/av` | `RecordThread`/`MmapThread::createAudioPatch_l`: fix AudioFlinger `startInput -22` (all recording dead — both AudioRecord and AAudio). Companion to 0010: with HAL patches forced off, input routing goes through the `setParameters()` branch, which seeds the param string from the source device's address. AOSP hardcodes the built-in mic address to `"bottom"` (`AudioPolicyManager.cpp:4547`), and `audio_device_address_to_parameter()` returns it as a bare token (no `key=`), so it lands as a valueless `bottom=` param the frozen-8.1 MT8173 vendor HAL rejects with `-EINVAL`. Fix: seed `AudioParameter` from the address only when it contains `'='` (real a2dp/usb/submix `key=value`); otherwise start empty → clean `routing=X;input_source=N`. Verified: stock recorder + phiola both record real audio, no -22. |

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
git -C system/core apply device/gpd/xdplus/patches/0009-*.patch
git -C frameworks/av apply device/gpd/xdplus/patches/0010-*.patch
git -C frameworks/base apply device/gpd/xdplus/patches/0011-*.patch
git -C frameworks/opt/net/wifi apply device/gpd/xdplus/patches/0012-*.patch
git -C frameworks/opt/net/wifi apply device/gpd/xdplus/patches/0013-*.patch
git -C frameworks/native apply device/gpd/xdplus/patches/0014-*.patch
git -C frameworks/av apply device/gpd/xdplus/patches/0015-*.patch
git -C frameworks/native apply device/gpd/xdplus/patches/0016-*.patch
```
