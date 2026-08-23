# Empty package whose only job is LOCAL_OVERRIDES_PACKAGES: it excludes the named
# apps from the image. This is the reliable way to drop a package that an inherited
# product (here handheld_product.mk, via full_base) added — AOSP defers product
# inheritance, so `$(filter-out ...)` in a device makefile can't remove it, but an
# installed module that "overrides" it makes the build skip installing it.
#
# Removes: Camera2 (device has no camera; features already stripped via
# sysconfig/xdplus-removed-features.xml), plus apps this handheld has no use for.
#
# ThemePicker and StorageManager each leave a caller behind; both are neutralised
# by static overlays under overlay/, not by source patches:
#   ThemePicker    -> Trebuchet pins ACTION_SET_WALLPAPER to com.android.wallpaper,
#                     so overlay/packages/apps/Trebuchet blanks wallpaper_picker_package.
#   StorageManager -> Settings' storage donut fires an unguarded ACTION_MANAGE_STORAGE,
#                     so overlay/packages/apps/Settings hides that button.
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_PACKAGE_NAME := xdplusRemovedApps
LOCAL_OVERRIDES_PACKAGES := \
    Camera2 \
    Backgrounds \
    Eleven \
    Etar \
    PhotoTable \
    StorageManager \
    ThemePicker

LOCAL_SDK_VERSION := current
LOCAL_CERTIFICATE := platform
LOCAL_MANIFEST_FILE := AndroidManifest.xml
LOCAL_MODULE_TAGS := optional

include $(BUILD_PACKAGE)
