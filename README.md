GPD XD+ (Plus)
==============

The XD+ (Plus) (codenamed _"xdplus"_) is a Mediatek MT8176-based Android handheld console from GPD and the successor to the original Rockchip RK3288-based XD.

Basic   | Spec Sheet
-------:|:-------------------------
CPU     | 2.1GHz Quad-Core MT8176
GPU     | PowerVR GX6250
Memory  | 4GB RAM
Ships   | Android 7.0
Storage | 4GB
Battery | 6000 mAh
Display | 5" 1280 x 720 px

![GPD XD+](https://www.geeky-gadgets.com/wp-content/uploads/2018/01/GPD-XD-Handheld-Android-Game-Console.jpg "GPD XD+")

LineageOS 18.1 (Android 11) device tree for the GPD XD+ (`xdplus`).

Branch: `lineage-18.1` (the only supported branch — this port targets 18.1). The `mt8176` kernel is currently a prebuilt (`BoardConfig.mk TARGET_PREBUILT_KERNEL`); a from-source 3.18 kernel is in progress.

## Building

```
# In a LineageOS 18.1 source tree, add a local manifest pointing at this repo:
#   .repo/local_manifests/xdplus.xml
#   <project name="<youruser>/android_device_gpd_xdplus"
#            path="device/gpd/xdplus" remote="github" revision="lineage-18.1" />
repo sync
source build/envsetup.sh
breakfast xdplus     # resolves lineage.dependencies (kernel + vendor)
brunch xdplus        # → flashable zip
```

Out-of-tree AOSP/framework fixes this port needs live as numbered patches in `patches/` — apply them from the repo root before `brunch` (see `patches/README.md`).

Install is via TWRP (locked bootloader, no `fastboot flash`). Vendor blobs are partition-based; the vendor-writing distributable is produced by `inject_vendor.sh`.
