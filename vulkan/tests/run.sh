#!/bin/sh
# Build and run the vkshim extension probe on a connected device.
#
# The probe is a standalone NDK binary, deliberately not a build-system module:
# it must never be installed into the image, and it has to be runnable against
# a device without rebuilding the ROM.
#
# Point ANDROID_NDK at an NDK, or let this find one:
#   ANDROID_NDK=/path/to/ndk ./run.sh
# The search order below covers the usual SDK layouts; nothing here may carry a
# machine-specific absolute path.
set -e

find_ndk() {
	[ -n "$ANDROID_NDK" ] && { echo "$ANDROID_NDK"; return; }
	[ -n "$ANDROID_NDK_HOME" ] && { echo "$ANDROID_NDK_HOME"; return; }
	[ -n "$ANDROID_NDK_ROOT" ] && { echo "$ANDROID_NDK_ROOT"; return; }
	for sdk in "$ANDROID_SDK_ROOT" "$ANDROID_HOME" \
		"$HOME/Android/Sdk" "$HOME/.local/share/android/sdk"; do
		[ -d "$sdk/ndk" ] || continue
		# Highest version present.
		ndk=$(ls -1 "$sdk/ndk" 2>/dev/null | sort -V | tail -1)
		[ -n "$ndk" ] && { echo "$sdk/ndk/$ndk"; return; }
	done
}

NDK=$(find_ndk)
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
	echo "no NDK found -- set ANDROID_NDK to one" >&2
	exit 1
fi

CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
if [ ! -x "$CC" ]; then
	echo "no aarch64 clang in $NDK" >&2
	exit 1
fi

cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# ./run.sh rp [control] -- the NULL-framebuffer render-pass guard, a separate
# binary because its control half is a deliberate crash.
if [ "$1" = "rp" ]; then
	shift
	"$CC" -O2 -Wall vkrp.c -lvulkan -o "$OUT/vkrp"
	adb push "$OUT/vkrp" /data/local/tmp/ >/dev/null
	if [ "$1" = "control" ]; then
		# The shim caches each debug.xdplus.* property on first use, so the
		# gate has to be down before the process starts. Restored either way,
		# including on a crash -- leaving it at 0 would ship an unguarded
		# RetroArch to the next person who runs a game.
		adb shell setprop debug.xdplus.vkrpguard 0
		adb shell "chmod +x /data/local/tmp/vkrp && /data/local/tmp/vkrp control" || true
		adb shell setprop debug.xdplus.vkrpguard 1
		echo "guard restored: debug.xdplus.vkrpguard=$(adb shell getprop debug.xdplus.vkrpguard)"
		exit 0
	fi
	adb shell "chmod +x /data/local/tmp/vkrp && /data/local/tmp/vkrp $*"
	exit $?
fi

"$CC" -O2 -Wall vkext.c -lvulkan -o "$OUT/vkext"
adb push "$OUT/vkext" /data/local/tmp/ >/dev/null
adb shell "chmod +x /data/local/tmp/vkext && /data/local/tmp/vkext $*"
