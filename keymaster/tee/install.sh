#!/system/bin/sh
# TEE keymaster bring-up for GPD XD+ (LOS 18.1 on prebuilt 8.1 vendor).
#
# Two vendor-partition fixes the frozen prebuilt needs for a hardware keymaster:
#
# 1. libtz_uree.so — the TEE client that the trustlet HAL keystore.mt8173.so
#    NEEDs (imports UREE_CreateSession/UREE_TeeServiceCall/...). It was missing
#    from the prebuilt 8.1 vendor, which is why keymaster fell back to software.
#    Restoring it makes the MT8173 secure world reachable (TEE spike).
#
# 2. /vendor/manifest.xml keymaster@3.0 transport: passthrough -> hwbinder.
#    The passthrough decl makes clients dlopen the vendor
#    android.hardware.keymaster@3.0-impl.so in-process; that O-era blob wants
#    SoftKeymasterContext::ParseKeyBlob which R's /system libsoftkeymasterdevice.so
#    no longer exports, so the load fails (and would CHECK-abort keystore).
#    Our own android.hardware.keymaster@3.0-service.xdplus serves a binderized,
#    TEE-backed device instead (built from R source via the ng:: factory, no
#    SoftKeymasterContext), so the decl must be hwbinder to route getService to it.
#
# Vendor is a frozen prebuilt never rebuilt from source, so this writes straight
# to the /vendor partition (persists across system-only flashes). Re-run after
# any vendor.img reflash, then reboot (the manifest change is read at boot).
# Requires root.
set -e
DIR="$(dirname "$0")"
mount -o rw,remount /system/vendor 2>/dev/null || mount -o rw,remount /vendor

install_lib() {
	src="$1"; dst="$2"
	cp "$src" "$dst"
	chmod 644 "$dst"
	chcon u:object_r:vendor_file:s0 "$dst"
}

install_lib "$DIR/libtz_uree64.so" /vendor/lib64/libtz_uree.so
install_lib "$DIR/libtz_uree32.so" /vendor/lib/libtz_uree.so

cp "$DIR/vendor_manifest.xml" /vendor/manifest.xml
chmod 644 /vendor/manifest.xml
restorecon /vendor/manifest.xml

echo "TEE keymaster libs + hwbinder manifest installed; reboot to apply."
