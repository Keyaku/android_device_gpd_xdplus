# hdmictl — manual MTK HDMI bringup helper, installed as /system/bin/hdmictl.
#
# The vendor HWC blob never sends MTK_HDMI_VIDEO_CONFIG, so the kernel never
# raises the res_hdmi switch that triggers the blob's external display bringup;
# this tool issues the missing ioctls on /dev/hdmitx. It used to be a static
# host-cross build staged in /data/local/tmp, which meant HDMI bringup silently
# depended on a hand-pushed artifact. Build it with the
# platform instead so xdplus_tweaks.sh can rely on it existing.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := hdmictl
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := hdmictl.c
LOCAL_MULTILIB := first
LOCAL_CFLAGS := -Wall
include $(BUILD_EXECUTABLE)
