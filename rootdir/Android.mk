# This port's two privileged shell services, installed as executables in
# /system/bin rather than copied into /system/etc.
#
# They used to be `service ... /system/bin/sh /system/etc/<script>.sh` in
# init.xdplus.rc, which meant init's exec target was the shell itself: under
# SELinux enforcement the only outcomes are execute_no_trans (denied -- measured)
# or a domain transition on shell_exec that would capture every init-launched
# sh on the device. Installed here as prebuilt EXECUTABLES they land 0755 with
# their own exec labels, so each gets a real domain of its own.
#
# PRODUCT_COPY_FILES cannot replace this: it installs 0644, and a non-executable
# file cannot be the target of a domain transition.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := xdplus_tweaks
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := system/bin/xdplus_tweaks
LOCAL_MODULE_SUFFIX :=
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := xdplus_wifi_seed
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := system/bin/xdplus_wifi_seed
LOCAL_MODULE_SUFFIX :=
include $(BUILD_PREBUILT)
