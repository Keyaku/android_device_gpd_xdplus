# Vulkan driver symlinks into /system/lib{,64}.
#
# ro.treble.enabled=false gives every process a single "default" linker
# namespace whose search paths are /system/${LIB}:/vendor/${LIB}:... —
# NOT /vendor/${LIB}/hw. The Vulkan loader dlopens "vulkan.mt8173.so" by
# bare soname (no hw/ subdir probing in the non-Treble path), so it never
# finds the driver and vkEnumeratePhysicalDevices returns 0 devices.
# Symlinking the vendor driver into /system/${LIB} puts it on the default
# namespace search path; its libsrv_um/gralloc deps resolve from
# /vendor/lib{,64} which IS on the path. Verified live 2026-07-11:
# PowerVR Rogue GX6250, Vulkan 1.0.49, driver 4893595 enumerates.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := vulkan.mt8173_symlink64
LOCAL_MODULE_CLASS := FAKE
LOCAL_POST_INSTALL_CMD := mkdir -p $(TARGET_OUT)/lib64 && ln -sf /vendor/lib64/hw/vulkan.mt8173.so $(TARGET_OUT)/lib64/vulkan.mt8173.so
include $(BUILD_PHONY_PACKAGE)

include $(CLEAR_VARS)
LOCAL_MODULE := vulkan.mt8173_symlink32
LOCAL_MODULE_CLASS := FAKE
LOCAL_POST_INSTALL_CMD := mkdir -p $(TARGET_OUT)/lib && ln -sf /vendor/lib/hw/vulkan.mt8173.so $(TARGET_OUT)/lib/vulkan.mt8173.so
include $(BUILD_PHONY_PACKAGE)
