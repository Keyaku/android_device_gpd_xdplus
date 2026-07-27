#!/system/bin/sh
# xdplus_tweaks.sh — privileged dispatcher for the in-Settings "GPD XD+" menu.
# Runs as root from init.xdplus.rc triggers. The Settings menu only sets the
# sys.xdplus.*/persist.sys.xdplus.* props; all root-side work happens here.
#
# Usage: xdplus_tweaks.sh <command>
#   hdmi_up      mini-HDMI DECOUPLE_MIRROR bringup (PORTING_LOG §117/§118)
#   hdmi_down    tear HDMI down, return to built-in panel only
#   boot         re-apply persisted toggles at boot_completed (currently a no-op)

H=/d/hdmi
# hdmictl now ships in the image (device/gpd/xdplus/hdmi); the /data copy is only
# a fallback for devices flashed before that landed.
CTL=/system/bin/hdmictl
[ -x "$CTL" ] || CTL=/data/local/tmp/hdmictl
# The acqfd-patched HWC blob lives in the VENDOR PARTITION since §72 — no bind
# mount, no composer restart, survives reboots. BLOB is only the legacy fallback.
BLOB=/data/local/tmp/hwcomposer.patched.so
VBLOB=/system/vendor/lib64/hw/hwcomposer.mt8173.so
VBLOB_MD5=54b28199

# NOTE: never name this function 'log' — a same-named function shadows the /system/bin/log
# binary and recurses infinitely, hanging the service. Use the binary by full path.
xlog() { /system/bin/log -t xdplus_tweaks "$*" 2>/dev/null; echo "xdplus_tweaks: $*"; }

# DECOUPLE_MIRROR bringup (§117/§118). The panel stays on hardware overlays
# (OVL0 -> WDMA0 -> RDMA0) and the external display is fed by the MDP blit — no
# GPU composition on either pipe. Replaces the old 1008-latch path (§64-§90),
# which forced the external display onto GPU CLIENT composition and left the
# panel frozen while mirroring (§114).
#
# Preconditions, all discovered the hard way:
#  - persist.sys.xdplus.hdmi_force_validate must have been 0 AT BOOT.
#    SurfaceFlinger reads it once at startup; §90's forced validate makes the
#    external display CLIENT-composed with a single layer, the blob's
#    isMirrorList then fails (chkMir L3819), and no live prop write can fix it.
#  - debug.hwc.mirror_state must be 1 BEFORE display 1 hotplugs: the mirror
#    output queue is only created in HWCDispatcher::onPlugIn.
#  - The keyguard must be dismissed: its KEYGUARD_DIALOG presentation on the
#    external display makes WindowManager report the display as having content,
#    DisplayManager then gives it its own layer stack instead of mirroring
#    stack 0, and the blob refuses (I-size mismatch at chkMir L3819).
hdmi_up() {
	RES="$(getprop persist.sys.xdplus.hdmi_res)"; [ -z "$RES" ] && RES=2
	if [ ! -x "$CTL" ]; then
		xlog "hdmictl missing ($CTL) — image too old, stage it in /data/local/tmp"
		return 1
	fi
	# The patched blob is expected to be part of /vendor already. Only fall back to
	# the old bind-mount + composer restart (~40 s framework bounce) if it is not.
	if ! md5sum "$VBLOB" | grep -q "$VBLOB_MD5"; then
		if [ -f "$BLOB" ]; then
			mount -o bind "$BLOB" "$VBLOB" && \
				kill "$(pidof android.hardware.graphics.composer@2.1-service)"
			xlog "unpatched vendor blob — bind-mounted + composer restarted, rerun after the bounce"
			return 0
		fi
		xlog "unpatched vendor blob and no $BLOB to fall back on — aborting"
		return 1
	fi

	if [ "$(getprop persist.sys.xdplus.hdmi_force_validate)" != "0" ]; then
		xlog "mirror mode not armed (hdmi_force_validate != 0 at boot) — enable 'HDMI mirror mode' in the GPD XD+ menu and reboot first"
		return 1
	fi

	# Mirror queue gate + optional dispatcher pacing knob. Both are debug.hwc.*
	# props the blob only re-reads inside HWCMediator::deviceDump (§67), so latch
	# them with a dumpsys BEFORE the display registers.
	setprop debug.hwc.mirror_state 1
	if [ "$(getprop persist.sys.xdplus.hdmi_novsync)" = "1" ]; then
		setprop debug.hwc.trigger_by_vsync 0
		xlog "trigger_by_vsync=0 (stutter knob)"
	fi
	dumpsys SurfaceFlinger > /dev/null

	# Keyguard gate (see header). 224 = KEYCODE_WAKEUP.
	input keyevent 224; sleep 1
	wm dismiss-keyguard; sleep 2

	# §84: the debugfs nodes can come up 0444, which makes every write below fail
	# with "Permission denied" even as root.
	chmod 666 $H /d/dispsys 2>/dev/null

	xlog "kernel bringup (res=$RES)"
	# No 'disable' step: since §81 the kernel no longer presets is_enabled, so a
	# single enable runs hdmi_drv_init() and creates hdmi_rdma_config_kthread.
	$CTL enable;  sleep 2
	$CTL power 1; sleep 3
	$CTL res "$RES"; sleep 6

	for try in 1 2 3; do
		if [ "$(dumpsys display | grep -c 'HDMI Screen')" = 0 ]; then
			# The res 11 -> res N toggle is what actually registers display 1.
			echo fakecablein:disable > $H
			$CTL res 11; sleep 3
			$CTL res "$RES"; sleep 5
		fi

		if [ "$(dumpsys SurfaceFlinger | grep -c 'DisplayDevice{1')" = 0 ]; then
			xlog "attempt $try: display 1 not registered — retrying"
			continue
		fi
		# Keyguard/presentation check: mirroring means the external display is on
		# the default layer stack. Its own stack = something (usually the keyguard
		# presentation) claimed it and the blob will refuse the mirror.
		if ! dumpsys SurfaceFlinger | grep -A3 'DisplayDevice{1' | grep -q 'layerStack=0'; then
			xlog "attempt $try: external display not mirroring stack 0 (keyguard?) — dismissing and retrying"
			wm dismiss-keyguard; sleep 2
			continue
		fi
		sleep 2
		if dmesg | grep 'mode now' | tail -1 | grep -q DECOUPLE_MIRROR; then
			xlog "HDMI mirror up (DECOUPLE_MIRROR, res=$RES)"
			return 0
		fi
		xlog "attempt $try: display 1 up but session mode is not DECOUPLE_MIRROR — retrying"
	done

	# All attempts missed. Tear down cleanly so the primary panel is not left wedged
	# (a failed bringup must degrade to "working panel, no HDMI", never a black screen).
	xlog "HDMI bringup FAILED after 3 attempts — safe teardown"
	echo fakecablein:disable > $H 2>/dev/null
	$CTL power 0 2>/dev/null
	$CTL disable 2>/dev/null
	setprop debug.hwc.mirror_state 4
	return 1
}

hdmi_down() {
	[ -x "$CTL" ] && { $CTL power 0; $CTL disable; }
	# Park the mirror queue gate so the next plain hotplug does not create the
	# mirror output queue against a torn-down path.
	setprop debug.hwc.mirror_state 4
	xlog "HDMI torn down"
}

# Relay a vkshim compile-progress update (sys.xdplus.vkcompile = "<pkg>:<count>")
# to the Settings receiver. Reads the prop live rather than taking an argument so
# rapid updates coalesce to the newest value. Explicit-component broadcast from
# root reaches the non-exported receiver.
vknotify() {
	MODE="$(getprop persist.sys.xdplus.vknotify)"
	[ -z "$MODE" ] && MODE=notification
	[ "$MODE" = "off" ] && return 0
	V="$(getprop sys.xdplus.vkcompile)"
	[ -z "$V" ] && return 0
	PKG="${V%%:*}"
	CNT="${V##*:}"
	am broadcast -n com.android.settings/.gpd.VkCompileReceiver \
		--es pkg "$PKG" --es count "$CNT" --es mode "$MODE" >/dev/null 2>&1
}

CMD="$1"

# Guard: the SurfaceFlinger-poking paths must never run before the framework is
# fully up. init also gates the triggers on sys.boot_completed, but a manual/
# early invocation must be a no-op too.
if [ "$CMD" != "hdmi_down" ] && [ "$(getprop sys.boot_completed)" != "1" ]; then
	xlog "boot not completed — deferring '$CMD'"
	setprop sys.xdplus.action none
	exit 0
fi

case "$CMD" in
	hdmi_up)   hdmi_up ;;
	hdmi_down) hdmi_down ;;
	# Boot path: nothing to re-apply since the un-freeze toggle was removed
	# (is_skip_validate was §83 KNOWN-BAD: it unfreezes nothing and makes any
	# freeze sticky across reboots). Kept as a hook for future persisted toggles.
	boot)      : ;;
	# Compile-progress relay: exits quietly, and must NOT clear sys.xdplus.action
	# (it never set it).
	vknotify)  vknotify; exit 0 ;;
	*)         xlog "unknown command: $CMD"; exit 1 ;;
esac
setprop sys.xdplus.action none
