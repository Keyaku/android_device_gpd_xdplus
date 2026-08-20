# xdplus_avcd — native SELinux denial recorder.
#
# Replaces the previous shell implementation that piped `dmesg -w` into a while
# loop. A shell pipeline needs a fifo_file rule from toolbox to this domain,
# which is the wrong shape for a tool whose job is to record policy denials.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := xdplus_avcd
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := xdplus_avcd.c
LOCAL_MULTILIB := first
LOCAL_CFLAGS := -Wall -Werror
include $(BUILD_EXECUTABLE)
