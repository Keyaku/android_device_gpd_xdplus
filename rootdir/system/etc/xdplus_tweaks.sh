#!/system/bin/sh
# xdplus_tweaks.sh — privileged dispatcher for the in-Settings "GPD XD+" menu.
# Runs as root from init.xdplus.rc triggers. The Settings menu only sets the
# sys.xdplus.*/persist.sys.xdplus.* props; all root-side work happens here.
#
# Usage: xdplus_tweaks.sh <command>
#   hdmi_up      full mini-HDMI mirror bringup (PORTING_LOG §64)
#   hdmi_down    tear HDMI down, return to built-in panel only
#   unfreeze     apply persist.sys.xdplus.hdmi_unfreeze -> debug.hwc.is_skip_validate
#   boot         re-apply persisted toggles at boot_completed

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

# Apply the persisted un-freeze value to the composer's debug.hwc.is_skip_validate.
# We deliberately do NOT poke SurfaceFlinger here: a live `dumpsys SurfaceFlinger`
# right after flipping is_skip_validate wedges system_server into a reboot on this
# build. The composer reads the prop naturally at its next deviceDump / on the next
# boot's SurfaceFlinger startup, so the toggle takes effect on the next reboot.
apply_unfreeze() {
	if [ "$(getprop persist.sys.xdplus.hdmi_unfreeze)" = "1" ]; then
		setprop debug.hwc.is_skip_validate 0
		xlog "panel un-freeze ON (is_skip_validate=0; effective next reboot)"
	else
		setprop debug.hwc.is_skip_validate ""
		xlog "panel un-freeze OFF (effective next reboot)"
	fi
	return 0
}

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

	# Dispatcher pacing knob for the mirrored-output stutter. Set before the first
	# dumpsys below, which is what latches it (the blob only re-reads its debug
	# props inside HWCMediator::deviceDump — §67).
	if [ "$(getprop persist.sys.xdplus.hdmi_novsync)" = "1" ]; then
		setprop debug.hwc.trigger_by_vsync 0
		xlog "trigger_by_vsync=0 (stutter knob)"
	fi

	xlog "kernel bringup (res=$RES)"
	# No 'disable' step: since §81 the kernel no longer presets is_enabled, so a
	# single enable runs hdmi_drv_init() and creates hdmi_rdma_config_kthread.
	$CTL enable;  sleep 2
	$CTL power 1; sleep 3
	$CTL res "$RES"; sleep 6

	input keyevent 224; sleep 1
	wm dismiss-keyguard; sleep 3

	for try in 1 2 3; do
		if [ "$(dumpsys display | grep -c 'HDMI Screen')" = 0 ]; then
			echo fakecablein:disable > $H
			$CTL res 11; sleep 3
			$CTL res "$RES"; sleep 5
		fi
		xlog "latch (attempt $try)"
		service call SurfaceFlinger 1008 i32 1; sleep 8
		service call SurfaceFlinger 1008 i32 0; sleep 2

		# CRITICAL ordering (PORTING_LOG §64/§61): only apply the fakecablein shield +
		# repair trio AFTER confirming the 1008 latch actually put external onto CLIENT
		# composition. Shielding a latch that did NOT take gates the kthread's TX before
		# it is configured -> no HDMI signal AND a wedged (black) primary panel. If the
		# latch missed, retry cleanly instead of shielding a broken state.
		C=$(dumpsys SurfaceFlinger | grep -A3 'DisplayDevice{1' | grep -c 'usesClientComposition=true')
		if [ "$C" -lt 1 ]; then
			xlog "latch $try: external not CLIENT-composited — skipping shield, retrying"
			continue
		fi

		echo fakecablein:enable > $H
		# The CG-ungate / DPI-EN / TMDS-CON3 repair trio is done by the kernel since
		# §81 (keep-alive in hdmi_timer_impl, ~2 Hz). Only poke the registers by hand
		# if the keep-alive has been turned off or an old kernel is running.
		if [ "$(getprop persist.sys.xdplus.hdmi_trio)" = "1" ]; then
			echo regw:0x14000118=0x300      > $H
			echo regw:0x1401d000=1          > $H
			echo regw:0x1020910c=0xff0f0000 > $H
			xlog "applied userspace repair trio (hdmi_trio=1)"
		fi
		sleep 1
		echo regr:0x1401d040 > $H; A=$(cat $H); sleep 0.4
		echo regr:0x1401d040 > $H; B=$(cat $H)
		if [ "$A" != "$B" ]; then
			xlog "HDMI mirror up (scan $A->$B, external CLIENT-latched)"
			apply_unfreeze
			return 0
		fi
		xlog "latch $try: shield applied but scan not advancing ($A -> $B)"
	done

	# All attempts missed. Tear down cleanly so the primary panel is not left wedged
	# (a failed bringup must degrade to "working panel, no HDMI", never a black screen).
	xlog "HDMI bringup FAILED after 3 attempts — safe teardown"
	echo fakecablein:disable > $H 2>/dev/null
	$CTL power 0 2>/dev/null
	$CTL disable 2>/dev/null
	return 1
}

hdmi_down() {
	[ -x "$CTL" ] && { $CTL power 0; $CTL disable; }
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
	# Live toggle from the menu (post-boot): safe to poke the composer.
	unfreeze)  apply_unfreeze ;;
	# Boot path: apply the persisted value but do NOT poke SurfaceFlinger while it
	# is still settling. The composer picks it up on its next natural deviceDump.
	boot)      apply_unfreeze ;;
	# Compile-progress relay: exits quietly, and must NOT clear sys.xdplus.action
	# (it never set it).
	vknotify)  vknotify; exit 0 ;;
	*)         xlog "unknown command: $CMD"; exit 1 ;;
esac
setprop sys.xdplus.action none
