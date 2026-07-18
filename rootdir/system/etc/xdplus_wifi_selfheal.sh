#!/system/bin/sh
# Re-enable Wi-Fi when the framework self-disabled it after a combo-chip
# reset, but the user's setting still says "on".
#
# Root cause (discovered 2026-07-18, supersedes the earlier "P2P iface race"
# hypothesis in HANDOFF.md "Wi-Fi shuts itself off"):
#
# The MT6630 combo chip's BT controller firmware hits an assert in
# system/transport/hcit_mtk_stp.c #2452 ~64-104 s after boot, triggered by an
# HCI vendor command the BT HAL sends during stack init. The assert crashes
# the BT controller, the WMT layer does a whole-chip reset (opfunc_hw_rst:
# "wmt core: turn off SDIO WIFI func ok!!"), and the shared SDIO function is
# taken down — unregistering wlan0 at the netdev level. The framework sees
# onUpChanged(false) -> CMD_INTERFACE_DOWN -> REASON_STA_IFACE_DOWN, and
# SelfRecovery.trigger() unconditionally calls recoveryDisableWifi() for
# STA_IFACE_DOWN (no retry, no throttle). shutdownWifi() does NOT clear
# Settings.Global.WIFI_ON, so the user intent ("on") is preserved while the
# runtime parks in DisabledState. Left as-is, Wi-Fi stays dead until the user
# toggles it manually.
#
# The BT assert is a firmware bug present in every available MT6630 patch
# (both our 20161026213119a build and the cube tree's 20180626b001007 build
# hit the same #2452). It cannot be fixed from the host without a newer MTK
# patch (closed-source). This script is the system-side workaround.
#
# Recovery timing: after the BT assert the combo chip's STP transport is
# wedged for ~2-3 minutes; during that window `cmd wifi set-wifi-enabled
# enabled` fails with "WMT turn on WIFI fail!" (STP Not Ready). Once STP
# recovers, re-enabling succeeds and wlan0 is recreated. So the script
# retries with backoff.
#
# Discriminator (vs. a deliberate user disable or airplane mode):
#   * settings get global wifi_on == 1   (user intent preserved by shutdownWifi)
#   * init.svc.wpa_supplicant == stopped (framework is not running supplicant)
# All must hold; otherwise the script is a no-op.
#
# Triggered by init.xdplus.rc as a oneshot service after boot_completed.
# Idempotent; safe to run on every boot.

# Wait out the early-boot BT assert window (assert observed at t=64-104 s).
sleep 90

# Only act if the user wants Wi-Fi on but the framework gave up.
WIFI_ON=$(settings get global wifi_on)
[ "$WIFI_ON" = "1" ] || exit 0

SUPP=$(getprop init.svc.wpa_supplicant)
[ "$SUPP" = "stopped" ] || exit 0

# Re-enable with retry: the combo chip's STP transport may still be
# recovering from the BT firmware assert, in which case the first attempt
# fails with "WMT turn on WIFI fail!". Retry until wlan0 comes up or we run
# out of attempts.
ATTEMPTS=0
MAX_ATTEMPTS=6
while [ $ATTEMPTS -lt $MAX_ATTEMPTS ]; do
	# Already recovered? Check if wlan0 exists and supplicant is running.
	if [ -e /sys/class/net/wlan0 ] && [ "$(getprop init.svc.wpa_supplicant)" = "running" ]; then
		exit 0
	fi

	cmd wifi set-wifi-enabled enabled >/dev/null 2>&1
	# Give the framework + HAL time to bring up wlan0 + supplicant.
	sleep 15
	ATTEMPTS=$((ATTEMPTS + 1))
done

# Final check — log outcome via exit code (init captures service exit).
[ -e /sys/class/net/wlan0 ] || echo "xdplus-wifi-selfheal: failed to recover wlan0 after $MAX_ATTEMPTS attempts" >&2