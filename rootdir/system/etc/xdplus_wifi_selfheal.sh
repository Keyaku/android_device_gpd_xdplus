#!/system/bin/sh
# Re-enable Wi-Fi when the framework self-disabled it after a HAL-level iface
# loss, but the user's setting still says "on".
#
# Symptom: on the first boot of the hdmi4 kernel, the gen2 wlan driver tore
# down wlan0 twice within ~90 s of boot (seconds after the P2P iface came up),
# and WifiSelfRecovery's throttled path (ActiveModeWarden shutdownWifi() on
# CMD_RECOVERY_DISABLE_WIFI, ActiveModeWarden.java:762) moved WifiController
# EnabledState -> DisabledState. shutdownWifi() does NOT clear the user
# setting, so Settings.Global.WIFI_ON stays 1 while the runtime is parked.
# Left as-is, the framework never auto-re-enables and Wi-Fi stays dead until
# the user toggles it manually. See HANDOFF.md "Wi-Fi shuts itself off" and
# PORTING_LOG (the 2026-07-17 evening diagnosis).
#
# Discriminator (vs. a deliberate user disable or airplane mode):
#   * settings get global wifi_on == 1   (user intent preserved by shutdownWifi)
#   * init.svc.wpa_supplicant == stopped (framework is not running supplicant)
#   * wlan.driver.status == ok          (driver/firmware loaded; not a hw fault)
# All three must hold; otherwise the script is a no-op.
#
# Triggered by init.xdplus.rc as a oneshot service after boot_completed, with
# a delay so the SelfRecovery disable (observed ~7-25 s after boot) has time to
# land before we re-enable. Idempotent; safe to run on every boot.

# Wait out the early-boot SelfRecovery window.
sleep 30

WIFI_ON=$(settings get global wifi_on)
SUPP=$(getprop init.svc.wpa_supplicant)
DRV=$(getprop wlan.driver.status)

# User wanted Wi-Fi off (or airplane mode, which also flips wifi_on to 0
# via handleAirplaneModeToggled) -> respect that, do nothing.
[ "$WIFI_ON" = "1" ] || exit 0

# Supplicant already running -> framework recovered on its own, don't touch.
[ "$SUPP" = "stopped" ] || exit 0

# Driver not loaded -> re-enabling would just fail; leave it for the next boot.
[ "$DRV" = "ok" ] || exit 0

# User intent = on, framework gave up, driver is fine -> re-enable.
cmd wifi set-wifi-enabled enabled