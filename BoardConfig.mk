LOCAL_PATH := device/gpd/xdplus

# Architecture
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_VARIANT := generic
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_ABI2 :=

TARGET_2ND_ARCH := arm
TARGET_2ND_ARCH_VARIANT := armv7-a-neon
TARGET_2ND_CPU_VARIANT := cortex-a15
TARGET_2ND_CPU_ABI := armeabi-v7a
TARGET_2ND_CPU_ABI2 := armeabi

TARGET_USES_64_BIT_BINDER := true
ARCH_ARM_HAVE_NEON := true

# Bootloader
TARGET_NO_BOOTLOADER := true
TARGET_BOOTLOADER_BOARD_NAME := mt8173
TARGET_IS_64_BIT := true

# Kernel
BOARD_KERNEL_BASE := 0x40000000
BOARD_KERNEL_OFFSET := 0x00080000
# LK truncates the combined cmdline at ~237 chars and prepends its own, leaving
# only ~99 for ours, so anything appended past that is silently dropped. Keep this
# short and put load-bearing parameters first.
#
# androidboot.selinux= and androidboot.hardware= are deliberately NOT set here:
# LK already supplies both, so setting them again only spends the budget twice.
# Verified on three boots with them absent -- getenforce reports Permissive and
# ro.boot.hardware reads mt8173, and first-stage init still finds /fstab.mt8173.
#
# log_buf_len is not a debugging luxury on this device. The default 512 KB ring
# (CONFIG_LOG_BUF_SHIFT=19) wraps in about 12 SECONDS under a Vulkan gameplay
# workload, which repeatedly destroyed the evidence for a kernel bug before it
# could be read. 8 MB covers a whole boot; it costs 8 MB of RAM out of 4 GB.
BOARD_KERNEL_CMDLINE := log_buf_len=8M loglevel=7 bootopt=64S3,32N2,64N2
BOARD_KERNEL_IMAGE_NAME := Image.gz-dtb
BOARD_KERNEL_PAGESIZE := 2048
# Offsets from the shipped boot.img header (ramdisk @0x55000000, tags @0x54000000,
# base 0x40000000) — MTK LK misboots with generic AOSP offsets.
BOARD_KERNEL_TAGS_OFFSET := 0x14000000
BOARD_RAMDISK_OFFSET := 0x15000000
TARGET_KERNEL_ARCH := arm64
# The 3.18.79 kernel is built in-tree by vendor/lineage/build/tasks/kernel.mk.
# ⚠️ TARGET_KERNEL_ADDITIONAL_CONFIG is MANDATORY, not an optimisation: without
# the fragment CONFIG_MTK_GPU_VERSION is unset, the DDK drops to 1.7 against the
# 1.9 vendor blobs, and SurfaceFlinger loops on "PVRSRVConnectKM: Incompatible
# driver". kernel.mk only $(warning)s on a missing fragment and then builds from
# /dev/null, so a typo here fails silently — after any change, diff
# out/target/product/xdplus/obj/KERNEL_OBJ/.config against a known-good .config.
TARGET_KERNEL_SOURCE := kernel/gpd/mt8176
TARGET_KERNEL_CONFIG := mt8176_defconfig
TARGET_KERNEL_ADDITIONAL_CONFIG := xdplus_kernel.frag
TARGET_KERNEL_CLANG_COMPILE := false
# 3.18's host tools predate -fno-common (clang 11 default) and C99 — dtc fails
# on duplicate `yylloc` without -fcommon. HOSTCFLAGS is assigned with `=` in the
# 3.18 top Makefile, so it must be overridden on the make command line; this
# assignment lands after BoardConfigKernel.mk's own HOSTCFLAGS and wins.
TARGET_KERNEL_ADDITIONAL_FLAGS := HOSTCFLAGS="-Wall -O2 -fomit-frame-pointer -std=gnu89 -fcommon -fuse-ld=lld"
BOARD_MKBOOTIMG_ARGS := --kernel_offset $(BOARD_KERNEL_OFFSET) --ramdisk_offset $(BOARD_RAMDISK_OFFSET) --tags_offset $(BOARD_KERNEL_TAGS_OFFSET)

# Shim CallStack (moved out of libutils in R) into the vendor graphics blobs
TARGET_LD_SHIM_LIBS := \
    /vendor/lib/libMtkOmxVdecEx.so|libshim_ui_codec.so \
    /vendor/lib/libMtkOmxVenc.so|libshim_ui_codec.so \
    /vendor/lib/libion_ulit.so|libshim_callstack.so \
    /vendor/lib64/libion_ulit.so|libshim_callstack.so \
    /vendor/lib64/hw/gralloc.mt8173.so|libshim_callstack.so \
    /vendor/lib64/hw/hwcomposer.mt8173.so|libshim_callstack.so \
    /vendor/lib/hw/gralloc.mt8173.so|libshim_callstack.so \
    /vendor/lib/hw/hwcomposer.mt8173.so|libshim_callstack.so \
    /vendor/lib/libmtkcam_stdutils.so|libshim_callstack.so \
    /vendor/lib64/libmtkcam_stdutils.so|libshim_callstack.so \
    /vendor/lib/libaudiocomponentengine_vendor.so|libshim_callstack.so \
    /vendor/lib64/libaudiocomponentengine_vendor.so|libshim_callstack.so \
    /vendor/lib64/libhwminijail.so|libshim_logbase.so \
    /vendor/lib/libhwminijail.so|libshim_logbase.so \
    /vendor/lib64/libnvram.so|libshim_logbase.so \
    /vendor/lib/libnvram.so|libshim_logbase.so \
    /vendor/lib64/hw/android.hardware.sensors@1.0-impl-mediatek.so|libshim_logbase.so \
    /vendor/bin/hw/android.hardware.wifi@1.0-service|libshim_logbase.so \
    /vendor/bin/hw/android.hardware.wifi@1.0-service|libshim_wifi.so \
    /vendor/bin/hw/wpa_supplicant|libshim_wifi.so \
    /vendor/bin/hw/android.hardware.drm@1.0-service.widevine|libshim_logbase.so \
    /vendor/lib/vendor.mediatek.hardware.audio@2.1_vendor.so|libshim_audio.so \
    /vendor/lib64/vendor.mediatek.hardware.audio@2.1_vendor.so|libshim_audio.so \
    /vendor/lib/hw/android.hardware.audio@2.0-impl-mediatek.so|libshim_audio.so \
    /vendor/lib64/hw/android.hardware.audio@2.0-impl-mediatek.so|libshim_audio.so \
    /vendor/lib/hw/android.hardware.audio.effect@2.0-impl.so|libshim_audio.so \
    /vendor/lib64/hw/android.hardware.audio.effect@2.0-impl.so|libshim_audio.so \
    /vendor/bin/hw/android.hardware.broadcastradio@1.1-service|libshim_logbase.so \
    /vendor/bin/hw/android.hardware.broadcastradio@1.1-service|libshim_broadcastradio.so

# Platform
TARGET_BOARD_PLATFORM := mt8173

# Bluetooth
BOARD_BLUETOOTH_BDROID_BUILDCFG_INCLUDE_DIR := $(LOCAL_PATH)/bluetooth
BOARD_HAVE_BLUETOOTH := true
BOARD_HAVE_BLUETOOTH_MTK := true
BOARD_BLUETOOTH_DOES_NOT_USE_RFKILL := true

# Charger
BOARD_CHARGER_ENABLE_SUSPEND := true
BOARD_CHARGER_DISABLE_INIT_BLANK := true


# Display
TARGET_FORCE_HWC_FOR_VIRTUAL_DISPLAYS := true
NUM_FRAMEBUFFER_SURFACE_BUFFERS := 3
TARGET_RUNNING_WITHOUT_SYNC_FRAMEWORK := true
PRESENT_TIME_OFFSET_FROM_VSYNC_NS := 0
MTK_HWC_SUPPORT := yes
MTK_HWC_VERSION := 1.4.0
#TARGET_USES_C2D_COMPOSITION := true
#TARGET_USES_GRALLOC1 := true
#TARGET_USES_HWC2 := true
#TARGET_USES_ION := true
#TARGET_USES_OVERLAY := true
#USE_OPENGL_RENDERER := true
#MAX_VIRTUAL_DISPLAY_DIMENSION := 4096
#VSYNC_EVENT_PHASE_OFFSET_NS := 2000000
#SF_VSYNC_EVENT_PHASE_OFFSET_NS := 6000000

# Extended Filesystem Support
TARGET_EXFAT_DRIVER := sdfat

# HIDL
DEVICE_MANIFEST_FILE := $(LOCAL_PATH)/manifest.xml

# Partitions
# NOTE: do NOT set TARGET_COPY_OUT_VENDOR=vendor / BOARD_PREBUILT_VENDORIMAGE
# here — this device is legacy system-as-root (init "Switching root to
# '/system'"), so /vendor lives INSIDE the system image and COPY_OUT=vendor
# makes system/vendor a symlink -> /vendor, i.e. /vendor -> /vendor, a mount
# loop ("Too many symbolic links", fastboot bounce). Vendor
# stays the physical mmcblk0p23 partition shadow-mounted at /system/vendor;
# it is written by the OTA via the releasetools.py raw package_extract_file.
BOARD_BOOTIMAGE_PARTITION_SIZE := 0x01000000
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 0x01000000
# Real size of mmcblk0p22 (scatter partition_size 0xA6400000); a larger value
# makes the OTA transfer list erase past the partition end -> E1001/BLKDISCARD EINVAL.
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 2789212160
BOARD_USERDATAIMAGE_PARTITION_SIZE := 26251096064
BOARD_CACHEIMAGE_PARTITION_SIZE := 1610612736
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_FLASH_BLOCK_SIZE := 131072
BOARD_HAS_LARGE_FILESYSTEM := true

# Power
TARGET_HAS_NO_WIFI_STATS := true

# Recovery
TARGET_RECOVERY_FSTAB := device/gpd/xdplus/rootdir/root/fstab.mt8173
# Defaults to $(TARGET_DEVICE_DIR)/../common, which does not exist here.
TARGET_RELEASETOOLS_EXTENSIONS := device/gpd/xdplus
TARGET_USERIMAGES_USE_EXT4 := true

# SELinux. This port's own policy lives in the SYSTEM_EXT slot, not in
# BOARD_SEPOLICY_DIRS: that variable feeds the *vendor* policy, and on this
# device /vendor is the frozen ALLDOCUBE partition whose own nonplat_sepolicy.cil
# init loads -- a vendor_sepolicy.cil built from source is staged into
# out/.../system/vendor/ and then shadowed by the partition mount, so it would
# never be loaded at all. system_ext ships inside system.img, which we do control.
BOARD_PLAT_PRIVATE_SEPOLICY_DIR := device/gpd/xdplus/sepolicy/private

# OpenGL
USE_OPENGL_RENDERER:= true

# WiFi
BOARD_WLAN_DEVICE := MediaTek
WPA_SUPPLICANT_VERSION := VER_0_8_X
BOARD_HOSTAPD_DRIVER := NL80211
BOARD_HOSTAPD_PRIVATE_LIB := lib_driver_cmd_mt66xx
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_mt66xx
WIFI_DRIVER_FW_PATH_PARAM := /dev/wmtWifi
WIFI_DRIVER_FW_PATH_AP := AP
WIFI_DRIVER_FW_PATH_STA := STA
WIFI_DRIVER_FW_PATH_P2P := P2P
WIFI_DRIVER_STATE_CTRL_PARAM := /dev/wmtWifi
WIFI_DRIVER_STATE_ON := 1
WIFI_DRIVER_STATE_OFF := 0

# Enable Minikin text layout engine (will be the default soon)
USE_MINIKIN := true

# Set the device resolution
DEVICE_RESOLUTION := 720x1280

# Fonts
EXTENDED_FONT_FOOTPRINT := true

# Set the system properties
TARGET_SYSTEM_PROP := $(LOCAL_PATH)/system.prop

# Enable a dummy camera (for our camera-less device)
USE_CAMERA_STUB := true

# Set the OTA device name assertion
TARGET_OTA_ASSERT_DEVICE := xdplus

