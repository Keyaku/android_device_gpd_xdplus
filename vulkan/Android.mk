# Vulkan HAL big-stack shim installed as /system/${LIB}/vulkan.mt8173.so.
#
# History: this dir originally installed plain symlinks
# /system/${LIB}/vulkan.mt8173.so -> /vendor/${LIB}/hw/vulkan.mt8173.so,
# because ro.treble.enabled=false gives every process a single "default"
# linker namespace whose search paths are /system/${LIB}:/vendor/${LIB}:...
# — NOT /vendor/${LIB}/hw — and the Vulkan loader dlopens the driver by
# bare soname (no hw/ probing in the non-Treble path). Verified live
# 2026-07-11: PowerVR Rogue GX6250, Vulkan 1.0.49 enumerates.
#
# The symlink is now replaced by vkshim.c: the DDK 1.9 USC shader compiler
# recurses unboundedly and overflows normal app thread stacks (SwanStation
# SIGSEGV in libusc.so), so the shim forwards the whole hwvulkan module to
# the real vendor blob but runs vkCreate{Graphics,Compute}Pipelines on a
# 64 MB-stack thread. The real blob stays at /vendor/${LIB}/hw/ and still
# resolves its libsrv_um/gralloc deps from /vendor/${LIB}.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := vulkan.mt8173
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := vkshim.c
LOCAL_MULTILIB := both
LOCAL_SHARED_LIBRARIES := liblog libdl
LOCAL_HEADER_LIBRARIES := hwvulkan_headers vulkan_headers libhardware_headers
include $(BUILD_SHARED_LIBRARY)
