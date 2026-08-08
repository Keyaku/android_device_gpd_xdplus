# Device SELinux policy — GPD XD+

`BOARD_SEPOLICY_DIRS += device/gpd/xdplus/sepolicy` has pointed here since the board config was written; the directory itself only came into existence once SELinux could actually be made enforcing.

**This is a first cut covering this port's own services, not a finished policy.** It exists to give our nine `.xdplus` daemons real domains instead of leaving them running as `u:r:init:s0`, which is what they do today.

## Why our services had no domains

They are all installed to `/system/bin/hw`, `/system_ext/bin/hw` or `/system/bin` and were labelled plain `u:object_r:system_file:s0`, which carries no `domain_auto_trans` from init. Under permissive that is invisible. Under enforcing they still exec — `system_file` is executable — but run **as init**, which is both wrong and the largest single source of denials measured on this device (86 of 166 in the first enforcing minute).

⚠️ **The platform's `hal_*_default_exec` types cannot be reused here.** They are declared `vendor_file_type` (`system/sepolicy/vendor/hal_*_default.te`), and labelling a `/system` file with a vendor type violates a platform neverallow — it fails the build rather than the boot. Hence our own exec types, declared `system_file_type`, with domains that carry the same `hal_*` attributes via `hal_server_domain()`.

## What is here

| Domain | Service | Binary |
|---|---|---|
| `hal_health_xdplus` | `health-hal-system` | `/system/bin/hw/android.hardware.health@2.1-service.xdplus` |
| `hal_configstore_xdplus` | `configstore-hal-system` | `/system/bin/hw/android.hardware.configstore@1.1-service.xdplus` |
| `hal_dumpstate_xdplus` | `dumpstate-hal-system` | `/system/bin/hw/android.hardware.dumpstate@1.1-service.xdplus` |
| `hal_omx_xdplus` | `omx-hal-system` | `/system/bin/hw/android.hardware.media.omx@1.0-service.xdplus` |
| `hal_keymaster_xdplus` | `kmxdplus-3-0`, `kmxdplus-4-1` | `/system_ext/bin/hw/android.hardware.keymaster@{3.0,4.1}-service.xdplus` |
| `hal_gatekeeper_xdplus` | `gkxdplus-1-0`, `gkxdplus-tee-1-0` | `/system_ext/bin/hw/android.hardware.gatekeeper@1.0-service{,.software}.xdplus` |
| `xdplus_hdmid` | `xdplus-hdmid` | `/system/bin/xdplus_hdmid` |

## What is deliberately NOT here yet

- **Rules derived from a denial harvest.** Only the structural minimum is written: exec type, domain, `init_daemon_domain()`, the HAL attribute, and the accesses that are obvious from each service's own rc file and source. Everything else waits for `setenforce 1` with the full feature set exercised, so that each rule has a denial behind it.
- **`dontaudit` rules.** Nothing is silenced before it has been seen.
- **Anything for the vendor blobs.** Their policy ships in `/vendor/etc/selinux/nonplat_sepolicy.cil` and is loaded as-is; the vendor-side gap that mattered was labelling, fixed in the vendor tree.

## Working on this

Build-time validation is `mka selinux_policy`, which runs the neverallow checks — a policy error here fails the build, it does not brick a boot. Runtime testing is `setenforce 1` on a userdebug build with the clamp-free kernel; a reboot restores permissive, so nothing persists.
