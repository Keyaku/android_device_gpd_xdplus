#!/system/bin/sh
# Seed the wpa_supplicant configs on a fresh /data (factory reset / first boot).
#
# This port's init only mkdir's /data/misc/wifi; nothing recreates the two .conf
# templates that init.connectivity.rc starts wpa_supplicant with (-c .../wpa_
# supplicant.conf for wlan0 and -c .../p2p_supplicant.conf for p2p0). After a
# /data wipe both are gone, so supplicant dies ("Failed to open/parse config"),
# ISupplicant never registers, and Wi-Fi can never enable. See PORTING_LOG §33.
#
# Copy-if-missing only: the framework keeps saved networks in WifiConfigStore.xml,
# not here, but we still never clobber an existing file so a user/runtime edit
# survives. Runs as root at post-fs-data (well before Wi-Fi is toggled on).
TEMPLATE=/vendor/etc/wifi/wpa_supplicant.conf
DIR=/data/misc/wifi

[ -f "$TEMPLATE" ] || exit 0

mkdir -p "$DIR"
chown wifi:wifi "$DIR"
chmod 0770 "$DIR"
restorecon "$DIR"

for name in wpa_supplicant p2p_supplicant; do
	conf="$DIR/$name.conf"
	if [ ! -f "$conf" ]; then
		cp "$TEMPLATE" "$conf"
		chown wifi:wifi "$conf"
		chmod 0660 "$conf"
		restorecon "$conf"
	fi
done
