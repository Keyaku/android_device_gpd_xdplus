#!/system/bin/sh
# Widevine DRM bring-up for GPD XD+ (LOS 18.1 on prebuilt 8.1 vendor).
#
# Problem: vendor's libwvhidl.so + libwvdrmengine.so (O-era, API 27) import
#   google::protobuf::internal::empty_string_
# which R's libprotobuf-cpp-lite.so renamed to fixed_address_empty_string.
# The device runs the [legacy] single-namespace linker config, so /system/lib
# is searched before /vendor/lib and R's protobuf always wins on the shared
# soname -> drm-widevine-hal-1-0 crash-loops with:
#   CANNOT LINK ... cannot locate symbol "_ZN6google8protobuf8internal13empty_string_E"
#
# Fix: rename the dependency in both blobs to a unique soname that only exists
# in /vendor/lib, and ship the O-era (VNDK v27) protobuf under that name.
# DT_NEEDED patched in place: "libprotobuf-cpp-lite.so\0" -> "libprotobuf-cpp-v27.so\0\0"
# (equal 24 bytes; trailing NUL is a harmless empty strtab entry).
#
# Vendor is a frozen prebuilt image never rebuilt from source, so this writes
# straight to the /vendor partition (persists across system-only flashes).
# Re-run after any vendor.img reflash. Requires root.
set -e
DIR="$(dirname "$0")"
mount -o rw,remount /system/vendor 2>/dev/null || mount -o rw,remount /vendor
install_lib() {
	src="$1"; dst="$2"
	cp "$src" "$dst"
	chmod 644 "$dst"
	chcon u:object_r:vendor_file:s0 "$dst"
}
install_lib "$DIR/libprotobuf-cpp-v27.so" /vendor/lib/libprotobuf-cpp-v27.so
install_lib "$DIR/libwvhidl.so"           /vendor/lib/libwvhidl.so
install_lib "$DIR/libwvdrmengine.so"      /vendor/lib/mediadrm/libwvdrmengine.so
setprop ctl.restart drm-widevine-hal-1-0
echo "widevine libs installed; restarted drm-widevine-hal-1-0"
