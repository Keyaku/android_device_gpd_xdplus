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

# xdplus_hdmid — event-driven HDMI plug/unplug watcher, /system/bin/xdplus_hdmid.
#
# Sleeps on the kernel's uevent netlink socket and runs xdplus_tweaks.sh's
# hdmi_up/hdmi_down on a cable transition. Deliberately not a shell poll loop:
# see the header comment in xdplus_hdmid.c for why netlink is the only
# zero-wakeup option here (the switch class never calls sysfs_notify, so
# poll() on the sysfs attribute cannot work).

include $(CLEAR_VARS)
LOCAL_MODULE := xdplus_hdmid
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := xdplus_hdmid.c
LOCAL_MULTILIB := first
LOCAL_CFLAGS := -Wall -Werror
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_INIT_RC := xdplus_hdmid.rc
include $(BUILD_EXECUTABLE)
