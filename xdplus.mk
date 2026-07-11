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

$(call inherit-product, $(SRC_TARGET_DIR)/product/languages_full.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

$(call inherit-product, frameworks/native/build/phone-xhdpi-1024-dalvik-heap.mk)

# Overlays
DEVICE_PACKAGE_OVERLAYS += $(LOCAL_PATH)/overlay

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:system/etc/permissions/android.hardware.bluetooth_le.xml \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:system/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.camera.flash-autofocus.xml:system/etc/permissions/android.hardware.camera.flash-autofocus.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:system/etc/permissions/android.hardware.camera.front.xml \
    frameworks/native/data/etc/android.hardware.camera.manual_sensor.xml:system/etc/permissions/android.hardware.camera.manual_sensor.xml \
    frameworks/native/data/etc/android.hardware.faketouch.xml:system/etc/permissions/android.hardware.faketouch.xml \
    frameworks/native/data/etc/android.hardware.location.gps.xml:system/etc/permissions/android.hardware.location.gps.xml \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:system/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.compass.xml:system/etc/permissions/android.hardware.sensor.compass.xml \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:system/etc/permissions/android.hardware.sensor.gyroscope.xml \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:system/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/native/data/etc/android.hardware.sensor.proximity.xml:system/etc/permissions/android.hardware.sensor.proximity.xml \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:system/etc/permissions/android.hardware.telephony.gsm.xml \
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

# System-side init additions (stop the crash-looping vendor configstore)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/system/etc/init/init.xdplus.rc:system/etc/init/init.xdplus.rc

# Audio
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/system/etc/media_profiles.xml:system/etc/media_profiles.xml \
    $(LOCAL_PATH)/rootdir/system/etc/media_codecs.xml:system/etc/media_codecs.xml \
    $(LOCAL_PATH)/rootdir/system/etc/media_codecs_performance.xml:system/etc/media_codecs_performance.xml \
    $(LOCAL_PATH)/rootdir/system/etc/audio_policy.conf:system/etc/audio_policy.conf \
    frameworks/av/media/libstagefright/data/media_codecs_google_audio.xml:system/etc/media_codecs_google_audio.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_telephony.xml:system/etc/media_codecs_google_telephony.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_video_le.xml:system/etc/media_codecs_google_video_le.xml


# Thermal
PRODUCT_COPY_FILES += \
     $(LOCAL_PATH)/configs/thermal.conf:system/etc/.tp/thermal.conf \
     $(LOCAL_PATH)/configs/.ht120.mtc:system/etc/.tp/.ht120.mtc \
     $(LOCAL_PATH)/configs/thermal.off.conf:system/etc/.tp/thermal.off.conf \
     $(LOCAL_PATH)/configs/sensors/_hals.conf:system/vendor/etc/sensors/_hals.conf
	
# VNDK apex — the prebuilt 8.1 vendor's passthrough HALs (keymaster, gralloc,
# hwcomposer, ...) need it; without it keystore crashloops on missing
# keymaster@3.0 and SurfaceFlinger never comes up (reference system ships it too).
PRODUCT_PACKAGES += com.android.vndk.current

# Pre-R HIDL compat shims — the 8.1 vendor blobs link libhwbinder.so /
# libhidltransport.so, which R merged into libhidlbase and stopped installing.
# Reference system ships the same stubs.
PRODUCT_PACKAGES += libhwbinder libhidltransport

# CallStack shim for vendor graphics blobs (see shims/Android.bp + TARGET_LD_SHIM_LIBS)
PRODUCT_PACKAGES += libshim_callstack libshim_logbase libshim_audio libshim_wifi

# Legacy system-side health@2.0 ("backup" instance) — the 8.1 vendor has no
# health HAL and BatteryService crashes system_server without one. Reference
# system ships healthd + manifest_healthd.xml the same way.
PRODUCT_PACKAGES += healthd

# System_ext software keymaster@4.1 + gatekeeper@1.0 (see keymaster/Android.bp).
# Vendor's keymaster@3.0/gatekeeper HALs are unloadable; these let keystore and
# gatekeeperd come up so system_server stops NPE-crashing on a null keystore.
PRODUCT_PACKAGES += android.hardware.keymaster@4.1-service.xdplus android.hardware.gatekeeper@1.0-service.software.xdplus

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

# Vulkan driver symlinks into /system/lib{,64} (see vulkan/Android.mk):
# non-Treble default namespace can't see /vendor/lib64/hw, loader dlopens
# bare "vulkan.mt8173.so" → 0 physical devices without these.
PRODUCT_PACKAGES += vulkan.mt8173_symlink64 vulkan.mt8173_symlink32

# Old HIDL libs the vendor audio HAL links against (not installed by default in R)
PRODUCT_PACKAGES += android.hardware.soundtrigger@2.0 android.hardware.audio.common@2.0-util libaudioroute libaudiospdif

# Reference libaudiohal with 2.0 client support (see prebuilt/audiohal/Android.bp)
PRODUCT_PACKAGES += libaudiohal@2.0

# System-side generic soundtrigger impl (see shims/soundtrigger/Android.bp)
PRODUCT_PACKAGES += android.hardware.soundtrigger@2.0-impl-xdplus

# First-stage fstab in the boot ramdisk (see rootdir/ramdisk/fstab.mt8173)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/ramdisk/fstab.mt8173:$(TARGET_COPY_OUT_RAMDISK)/fstab.mt8173

# Keylayout
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/system/usr/keylayout/mtk-kpd.kl:system/usr/keylayout/mtk-kpd.kl \

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
    camera.disable_zsl_mode=1 \
    ro.adb.secure=0 \
    ro.secure=0 \
    persist.sys.usb.config=adb
	
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

# Camera HAL
PRODUCT_PACKAGES += \
    camera.device@1.0-impl \
    camera.device@3.2-impl \
    android.hardware.camera.provider@2.4-impl \
    android.hardware.camera.provider@2.4-service

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

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/seccomp_policy/mediacodec.policy:system/vendor/etc/seccomp_policy/mediacodec.policy
	
PRODUCT_AAPT_CONFIG := normal hdpi
PRODUCT_AAPT_PREF_CONFIG := hdpi

# call the proprietary setup
$(call inherit-product, vendor/gpd/xdplus/xdplus-vendor.mk)
