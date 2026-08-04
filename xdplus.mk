# Copyright (C) 2013 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Keylayout — MUST stay above every inherit-product line. PRODUCT_COPY_FILES
# dedup is first-entry-wins, and full_base → handheld_system → frameworks
# keyboards.mk ships a stock DS4 Vendor_054c_Product_05c4.kl (no D-Pad
# 0x124-0x127 mapping) that would otherwise shadow ours. EventHub searches
# /system BEFORE /data/system/devices, so the on-device override can't save us.
# Explicit path: $(LOCAL_PATH) is not yet set this early.
PRODUCT_COPY_FILES += \
    device/gpd/xdplus/rootdir/system/usr/keylayout/mtk-kpd.kl:system/usr/keylayout/mtk-kpd.kl \
    device/gpd/xdplus/rootdir/system/usr/keylayout/Vendor_054c_Product_05c4.kl:system/usr/keylayout/Vendor_054c_Product_05c4.kl

$(call inherit-product, $(SRC_TARGET_DIR)/product/languages_full.mk)
# SIM-less tablet: full_base (non-telephony) instead of full_base_telephony.
# Drops AOSP telephony.mk extras (Dialer, CarrierConfig, CarrierDefaultApp, ONS,
# CallLogBackup, cellbroadcast apps). The framework telephony baseline
# (Telecom/TeleService/TelephonyProvider from handheld_system) stays — SystemUI/
# Settings need it — but is dormant with no radio HAL + config_voice_capable=false.
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)

$(call inherit-product, frameworks/native/build/phone-xhdpi-1024-dalvik-heap.mk)

# Overlays
DEVICE_PACKAGE_OVERLAYS += $(LOCAL_PATH)/overlay

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:system/etc/permissions/android.hardware.bluetooth_le.xml \
    frameworks/native/data/etc/android.hardware.broadcastradio.xml:system/etc/permissions/android.hardware.broadcastradio.xml \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:system/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.faketouch.xml:system/etc/permissions/android.hardware.faketouch.xml \
    frameworks/native/data/etc/android.hardware.location.gps.xml:system/etc/permissions/android.hardware.location.gps.xml \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:system/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.compass.xml:system/etc/permissions/android.hardware.sensor.compass.xml \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:system/etc/permissions/android.hardware.sensor.gyroscope.xml \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:system/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/native/data/etc/android.hardware.sensor.proximity.xml:system/etc/permissions/android.hardware.sensor.proximity.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.distinct.xml:system/etc/permissions/android.hardware.touchscreen.multitouch.distinct.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:system/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.xml:system/etc/permissions/android.hardware.touchscreen.multitouch.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.xml:system/etc/permissions/android.hardware.touchscreen.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:system/etc/permissions/android.hardware.usb.accessory.xml \
    frameworks/native/data/etc/android.hardware.wifi.direct.xml:system/etc/permissions/android.hardware.wifi.direct.xml \
    frameworks/native/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/android.software.midi.xml:system/etc/permissions/android.software.midi.xml \
    frameworks/native/data/etc/android.software.sip.voip.xml:system/etc/permissions/android.software.sip.voip.xml \
    frameworks/native/data/etc/handheld_core_hardware.xml:system/etc/permissions/handheld_core_hardware.xml
# NOTE: android.software.live_wallpaper.xml removed — LivePicker app ships it in 18.1 (dup-rule)

# GPS
PRODUCT_COPY_FILES += \
     $(LOCAL_PATH)/configs/agps_profiles_conf2.xml:system/etc/agps_profiles_conf2.xml \

# System-side init additions (stop the crash-looping vendor configstore;
# seed wpa/p2p supplicant configs on fresh /data — see xdplus_wifi_seed.sh)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/system/etc/init/init.xdplus.rc:system/etc/init/init.xdplus.rc \
    $(LOCAL_PATH)/rootdir/system/etc/xdplus_wifi_seed.sh:system/etc/xdplus_wifi_seed.sh \
    $(LOCAL_PATH)/rootdir/system/etc/xdplus_tweaks.sh:system/etc/xdplus_tweaks.sh

# Remove camera features the frozen vendor declares (device has no camera).
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/system/etc/sysconfig/xdplus-removed-features.xml:system/etc/sysconfig/xdplus-removed-features.xml

# Audio
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/system/etc/media_profiles.xml:system/etc/media_profiles.xml \
    $(LOCAL_PATH)/rootdir/system/etc/media_codecs.xml:system/etc/media_codecs.xml \
    $(LOCAL_PATH)/rootdir/system/etc/media_codecs_performance.xml:system/etc/media_codecs_performance.xml \
    $(LOCAL_PATH)/rootdir/system/etc/audio_policy.conf:system/etc/audio_policy.conf \
    $(LOCAL_PATH)/rootdir/system/etc/audio_effects.xml:system/etc/audio_effects.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_audio.xml:system/etc/media_codecs_google_audio.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_telephony.xml:system/etc/media_codecs_google_telephony.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_video_le.xml:system/etc/media_codecs_google_video_le.xml


# Thermal
PRODUCT_COPY_FILES += \
     $(LOCAL_PATH)/configs/thermal.conf:system/etc/.tp/thermal.conf \
     $(LOCAL_PATH)/configs/.ht120.mtc:system/etc/.tp/.ht120.mtc \
     $(LOCAL_PATH)/configs/thermal.off.conf:system/etc/.tp/thermal.off.conf
	
# VNDK apex — the prebuilt 8.1 vendor's passthrough HALs (keymaster, gralloc,
# hwcomposer, ...) need it; without it keystore crashloops on missing
# keymaster@3.0 and SurfaceFlinger never comes up (reference system ships it too).
PRODUCT_PACKAGES += com.android.vndk.current

# Pre-R HIDL compat shims — the 8.1 vendor blobs link libhwbinder.so /
# libhidltransport.so, which R merged into libhidlbase and stopped installing.
# Reference system ships the same stubs.
PRODUCT_PACKAGES += libhwbinder libhidltransport

# CallStack shim for vendor graphics blobs (see shims/Android.bp + TARGET_LD_SHIM_LIBS)
PRODUCT_PACKAGES += libshim_callstack libshim_logbase libshim_audio libshim_wifi libshim_broadcastradio libshim_ui_codec

# Health HAL. The 8.1 vendor ships no health HAL. healthd stays as the
# charger-mode battery service (init.mt8173.rc `service battery_charger /charger`,
# `class charger`) — offline charging only, normal boot never starts it.
PRODUCT_PACKAGES += healthd
# Normal-boot health@2.1: without a registered IHealth, BatteryService stalls
# ~4 s on getService before falling back (largest single boot-time item).
# Self-contained system-side service (health/service.cpp) — registers the default
# health impl in-process, no vendor passthrough .so. `class hal` only, so it
# never coexists with the charger-mode healthd above.
PRODUCT_PACKAGES += android.hardware.health@2.1-service.xdplus

# No custom OTA trust anchor is installed. Update payloads are served over
# HTTPS with a publicly-trusted certificate, so the stock system trust store
# is sufficient and nothing device-specific is baked in.
#
# A private-CA variant previously lived here as a wildcard PRODUCT_COPY_FILES
# over a cacerts directory. It was removed rather than left inert: a wildcard
# that matches nothing copies nothing SILENTLY and the build still succeeds,
# so it read as working for as long as it existed. If a custom anchor is ever
# genuinely needed, add it explicitly and verify the file lands on the device.

# System_ext keymaster + gatekeeper (see keymaster/Android.bp).
# - keymaster@3.0-service.xdplus: binderized, TEE-backed (wraps trustlet HAL
#   keystore.mt8173.so via the now-loadable vendor impl).
#   Provides the real TRUSTED_ENVIRONMENT keymaster for FBE.
# - keymaster@4.1-service.xdplus: software fallback (SOFTWARE slot / legacy keys).
# - gatekeeper@1.0-service.xdplus: binderized, TEE-backed (wraps trustlet HAL
#   gatekeeper.mt8173.so; the TEE spike proved enroll/verify with no libgatekeeper_mtee).
#   Shares the trustlet root of trust with the HW keymaster so HardwareAuthTokens
#   are accepted — fixes the -26 KEY_USER_NOT_AUTHENTICATED PIN regression.
#   Falls back to software gatekeeper internally if the trustlet won't open.
PRODUCT_PACKAGES += android.hardware.keymaster@3.0-service.xdplus android.hardware.keymaster@4.1-service.xdplus android.hardware.gatekeeper@1.0-service.xdplus

# System-side configstore@1.1 (see configstore/service.cpp): vendor's 8.1
# configstore SIGSYS-loops on its stale seccomp policy and never registers
# ISurfaceFlingerConfigs; MTK GED in every app RenderThread blocks forever
# waiting for it → windows never draw (splash forever, mCurrentFocus=null).
PRODUCT_PACKAGES += android.hardware.configstore@1.1-service.xdplus

# Stub dumpstate HAL (no vendor impl; hung getService ANRed Developer options)
PRODUCT_PACKAGES += android.hardware.dumpstate@1.1-service.xdplus

# System-side OMX HAL (vendor one unlinkable: O-era libstagefright ABI). Loads
# the vendor MTK codec plugin (libstagefrighthw) via OMXMaster.
PRODUCT_PACKAGES += android.hardware.media.omx@1.0-service.xdplus

# Vulkan big-stack shim into /system/lib{,64} (see vulkan/Android.mk):
# non-Treble default namespace can't see /vendor/lib64/hw, loader dlopens
# bare "vulkan.mt8173.so"; shim forwards to the vendor blob but compiles
# pipelines on a 64 MB stack (DDK 1.9 libusc recursion overflow).
PRODUCT_PACKAGES += vulkan.mt8173

# mini-HDMI bringup helper (see hdmi/Android.mk). Sends the MTK_HDMI_* ioctls
# the vendor HWC blob never sends. Baked in so xdplus_tweaks.sh's HDMI actions
# no longer depend on an artifact hand-pushed to /data/local/tmp.
PRODUCT_PACKAGES += hdmictl

# Old HIDL libs the vendor audio HAL links against (not installed by default in R)
PRODUCT_PACKAGES += android.hardware.soundtrigger@2.0 android.hardware.audio.common@2.0-util libaudioroute libaudiospdif

# The @2.0 audio HAL client. AOSP R dropped it; LineageOS 18.1 keeps both the
# library (frameworks/av/media/libaudiohal/impl/Android.bp) and its entry in the
# loader's version table (FactoryHalHidl.cpp), so this builds from source — the
# MTK binderized audio@2.0 service can't run (its -impl blob needs O-era
# HidlUtils manglings nothing in R provides) and audioserver reaches the vendor
# audio.primary HAL through this client instead.
PRODUCT_PACKAGES += libaudiohal@2.0

# System-side generic soundtrigger impl (see shims/soundtrigger/Android.bp)
PRODUCT_PACKAGES += android.hardware.soundtrigger@2.0-impl-xdplus

# First-stage fstab in the boot ramdisk (see rootdir/ramdisk/fstab.mt8173)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/ramdisk/fstab.mt8173:$(TARGET_COPY_OUT_RAMDISK)/fstab.mt8173

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/system/etc/hostapd/hostapd_default.conf:system/etc/hostapd/hostapd_default.conf \
    $(LOCAL_PATH)/rootdir/system/etc/hostapd/hostapd.accept:system/etc/hostapd/hostapd.accept \
    $(LOCAL_PATH)/rootdir/system/etc/hostapd/hostapd.deny:system/etc/hostapd/hostapd.deny
	
PRODUCT_TAGS += dalvik.gc.type-precise

# NOTE: Legacy A-only root init/fstab copies removed. In 18.1 Treble these ship
# from the (reused prebuilt) vendor partition at /vendor/etc/init + /vendor/etc/fstab.
# Their old MTK triggers (early_property:/fs_property:) are also rejected by
# host_init_verifier in Android 11.

# Correct bootanimation size for the screen
TARGET_SCREEN_HEIGHT := 1280
TARGET_SCREEN_WIDTH := 720

PRODUCT_PACKAGES += \
    audio.a2dp.default \
    audio.usb.default \
    audio.r_submix.default \
    libaudio-resampler \
    tinymix

# USE_CUSTOM_AUDIO_POLICY := 1

# Wifi
 PRODUCT_PACKAGES += \
    libwpa_client \
    hostapd \
    dhcpcd.conf \
    wpa_supplicant \
    wpa_supplicant.conf \
    wificond

PRODUCT_PACKAGES += \
    libion \
    libcurl

PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    ro.adb.secure=0 \
    ro.secure=0 \
    persist.sys.usb.config=adb

# MTK LK does not pass androidboot.bootloader on the kernel cmdline, so init
# derives ro.bootloader="unknown". Own it with a device string (immutable ro.
# prop set here wins over init's ro.boot.bootloader fallback). Refine to the
# real LK/preloader version if it is ever extracted from the lk/preloader part.
PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    ro.bootloader=GPD-XDPLUS-1.0
	
PRODUCT_PACKAGES += \
    librs_jni \
    com.android.future.usb.accessory

PRODUCT_PACKAGES += \
    charger \
    charger_res_images \
    libnl_2 \
    libtinyxml

PRODUCT_PACKAGES += \
    setup_fs \
    e2fsck \

# Dynamically set props
PRODUCT_SYSTEM_PROPERTY_BLACKLIST := \
    ro.product.name \
    ro.product.manufacturer \
    ro.product.model

# Bluetooth
PRODUCT_PACKAGES += \
    android.hardware.bluetooth@1.0-impl \
    android.hardware.bluetooth@1.0-service

PRODUCT_PACKAGES += \
    android.hardware.sensors@1.0-impl \
    android.hardware.sensors@1.0-service \
    sensors.xdplus

# WiFi
PRODUCT_PACKAGES += \
    android.hardware.wifi@1.0-service

# USB
PRODUCT_PACKAGES += \
    android.hardware.usb@1.0-service

# Graphics
PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator@2.0-service \
    android.hardware.graphics.allocator@2.0-impl \
    android.hardware.graphics.mapper@2.0-impl \
    android.hardware.graphics.composer@2.1-impl \
    android.hardware.graphics.composer@2.1-service

# Camera HAL removed: device has no camera. The (system) generic camera.device /
# provider@2.4 passthrough impls are unused here anyway — the vendor's own
# camerahalserver is the only camera HAL, and it enumerates 0 devices. Dropping
# these debloats system; features are stripped via sysconfig/xdplus-removed-features.xml.

# Vibrator
PRODUCT_PACKAGES += \
    android.hardware.vibrator@1.0-impl \
    android.hardware.vibrator@1.0-service

# Power
PRODUCT_PACKAGES += \
    android.hardware.power@1.0-impl \
    android.hardware.power@1.0-service

# Lights
PRODUCT_PACKAGES += \
    android.hardware.light@2.0-impl \
    android.hardware.light@2.0-service

# Audio
PRODUCT_PACKAGES += \
    android.hardware.audio@2.0-impl \
    android.hardware.audio.effect@2.0-impl

# Memtrack
PRODUCT_PACKAGES += \
    android.hardware.memtrack@1.0-impl \
    android.hardware.memtrack@1.0-service

# Keymaster
PRODUCT_PACKAGES += \
    android.hardware.keymaster@3.0-impl

# HIDL manifest handled via DEVICE_MANIFEST_FILE in BoardConfig.mk (18.1 VINTF)

	
PRODUCT_AAPT_CONFIG := normal hdpi
PRODUCT_AAPT_PREF_CONFIG := hdpi

# call the proprietary setup
$(call inherit-product, vendor/gpd/xdplus/xdplus-vendor.mk)
