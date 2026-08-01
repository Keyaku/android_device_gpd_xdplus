#!/system/bin/sh
# xdplus_tweaks.sh — privileged dispatcher for the in-Settings "GPD XD+" menu.
# Runs as root from init.xdplus.rc triggers. The Settings menu only sets the
# sys.xdplus.*/persist.sys.xdplus.* props; all root-side work happens here.
#
# Usage: xdplus_tweaks.sh <command>
#   hdmi_up      mini-HDMI DECOUPLE_MIRROR bringup
#   hdmi_down    tear HDMI down, return to built-in panel only
#   boot         re-apply persisted toggles at boot_completed, prune the cache dir
#   vkcache_prune  hold /data/vkshim under its total budget (root-only work)
#   vkcache_clear  delete every shader cache (root-only work)

H=/d/hdmi
# hdmictl now ships in the image (device/gpd/xdplus/hdmi); the /data copy is only
# a fallback for devices flashed before that landed.
CTL=/system/bin/hdmictl
[ -x "$CTL" ] || CTL=/data/local/tmp/hdmictl
# The acqfd-patched HWC blob lives in the VENDOR PARTITION — no bind
# mount, no composer restart, survives reboots. BLOB is only the legacy fallback.
BLOB=/data/local/tmp/hwcomposer.patched.so
VBLOB=/system/vendor/lib64/hw/hwcomposer.mt8173.so
VBLOB_MD5=54b28199

# NOTE: never name this function 'log' — a same-named function shadows the /system/bin/log
# binary and recurses infinitely, hanging the service. Use the binary by full path.
xlog() { /system/bin/log -t xdplus_tweaks "$*" 2>/dev/null; echo "xdplus_tweaks: $*"; }

# DECOUPLE_MIRROR bringup. The panel stays on hardware overlays
# (OVL0 -> WDMA0 -> RDMA0) and the external display is fed by the MDP blit — no
# GPU composition on either pipe. Replaces the old 1008-latch path,
# which forced the external display onto GPU CLIENT composition and left the
# panel frozen while mirroring.
#
# Preconditions, all discovered the hard way:
#  - persist.sys.xdplus.hdmi_force_validate must have been 0 AT BOOT.
#    SurfaceFlinger reads it once at startup; the forced validate makes the
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
	# props the blob only re-reads inside HWCMediator::deviceDump, so latch
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

	# NOTE: the debugfs nodes can come up 0444, which makes every write below fail
	# with "Permission denied" even as root.
	chmod 666 $H /d/dispsys 2>/dev/null

	xlog "kernel bringup (res=$RES)"
	# No 'disable' step: the kernel no longer presets is_enabled, so a
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

# /data/vkshim is sticky (see init.xdplus.rc), so an app can only unlink its own
# cache and the Settings menu — running as system, not root — cannot unlink
# anyone's. Enforcing the whole-directory budget is therefore root's job. The
# per-app cap still lives in the shim, which only ever touches its own file.
#
# Oldest first, because a cache that has not been written in a long time belongs
# to something the user has not played in a long time.
# sdcardfs appid repair.
#
# sdcardfs derives the owner of /storage/emulated/0/Android/data/<pkg> from the
# value in /config/sdcardfs/<pkg>/appid: inode uid = userid * 100000 + appid.
# The correct value is the app's full uid (10152 for u0_a152). Measured on this
# device: every entry held uid - 10000 (152), so every app was "other" on its own
# data directory - mode 0771 leaves it --x, and the app gets EACCES writing files
# it owns. RetroArch's "Failed to save config file" was this, and it flips per
# boot, so a boot where it works proves nothing.
#
# The producer of the wrong value has not been identified (PackageManagerService
# is the only writer in the tree and it writes ps.appId, which is the full uid),
# so this repairs the result rather than the cause. Writing the *correct* value
# takes effect immediately: the kernel skips an equal-value write, which is why
# PackageManager re-asserting its own value never fixes anything, but a different
# value updates the hash and re-derives the tree.
#
# Runs once at boot_completed. An app installed later keeps the wrong value until
# the next boot.
sdcardfs_appid_fix() {
	LIST=/data/system/packages.list
	CFG=/config/sdcardfs
	[ -r "$LIST" ] && [ -d "$CFG" ] || return 0

	FIXED=0
	while read -r PKG UID REST; do
		[ -n "$PKG" ] && [ -n "$UID" ] || continue
		case "$UID" in ''|*[!0-9]*) continue ;; esac
		F="$CFG/$PKG/appid"
		[ -f "$F" ] || continue
		CUR="$(cat "$F" 2>/dev/null)"
		[ "$CUR" = "$UID" ] && continue
		echo "$UID" > "$F" 2>/dev/null
		FIXED=$((FIXED + 1))
	done < "$LIST"

	[ "$FIXED" -gt 0 ] && xlog "sdcardfs appid repaired for $FIXED package(s)"
	return 0
}

vkcache_prune() {
	DIR=/data/vkshim
	[ -d "$DIR" ] || return 0
	MB="$(getprop persist.sys.xdplus.vkcachedirmax)"
	case "$MB" in ''|*[!0-9]*) MB=192 ;; esac
	[ "$MB" -eq 0 ] && MB=192
	BUDGET=$((MB * 1024 * 1024))

	TOTAL=0
	for f in "$DIR"/*.pcache; do
		[ -f "$f" ] || continue
		TOTAL=$((TOTAL + $(stat -c %s "$f" 2>/dev/null || echo 0)))
	done
	[ "$TOTAL" -le "$BUDGET" ] && return 0

	for f in $(ls -rt "$DIR"/*.pcache 2>/dev/null); do
		[ "$TOTAL" -le "$BUDGET" ] && break
		SZ=$(stat -c %s "$f" 2>/dev/null) || continue
		rm -f "$f" || continue
		TOTAL=$((TOTAL - SZ))
		xlog "cache dir over ${MB}MB budget, evicted $f ($SZ bytes)"
	done
}

# An app keeps its own cache alive until it exits, so this takes effect on the
# next launch of anything currently running.
vkcache_clear() {
	rm -f /data/vkshim/*.pcache
	xlog "shader caches cleared"
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
	# Boot path: the un-freeze toggle was removed (is_skip_validate was
	# KNOWN-BAD: it unfreezes nothing and makes any freeze sticky across
	# reboots), so the only work here is holding the shader-cache directory
	# under budget — the one place that can, now that the directory is sticky.
	boot)      sdcardfs_appid_fix; vkcache_prune ;;
	vkcache_prune) vkcache_prune ;;
	vkcache_clear) vkcache_clear ;;
	# Compile-progress relay: exits quietly, and must NOT clear sys.xdplus.action
	# (it never set it).
	vknotify)  vknotify; exit 0 ;;
	*)         xlog "unknown command: $CMD"; exit 1 ;;
esac
setprop sys.xdplus.action none
