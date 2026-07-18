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
CTL=/data/local/tmp/hdmictl
BLOB=/data/local/tmp/hwcomposer.patched.so
VBLOB=/system/vendor/lib64/hw/hwcomposer.mt8173.so

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
	if [ ! -x "$CTL" ] || [ ! -f "$BLOB" ]; then
		xlog "HDMI artifacts missing ($CTL / $BLOB) — stage them first"
		return 1
	fi
	if ! mount | grep -q "$VBLOB" || ! md5sum "$VBLOB" | grep -q 54b28199; then
		mount -o bind "$BLOB" "$VBLOB" && \
			kill "$(pidof android.hardware.graphics.composer@2.1-service)"
		xlog "composer bind-mounted + restarted — rerun bringup after the bounce"
		return 0
	fi

	xlog "kernel bringup (res=$RES)"
	$CTL disable; sleep 2
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

		echo fakecablein:enable      > $H
		echo regw:0x14000118=0x300   > $H
		echo regw:0x1401d000=1       > $H
		echo regw:0x1020910c=0xff0f0000 > $H
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
	*)         xlog "unknown command: $CMD"; exit 1 ;;
esac
setprop sys.xdplus.action none
