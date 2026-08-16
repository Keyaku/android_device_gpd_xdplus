#!/bin/sh
# Functional maxPushDescriptors search: escalate N until pushing N descriptors
# stops producing the right answer.
#
# The blob validates nothing at object-creation time (see README.md), so the
# limit can only be found by using it. Each N runs in its OWN process against a
# fresh VkDevice: a device-lost poisons everything after it in the same process,
# and one N hanging must not take the rest of the run with it.
#
# ⚠️ This drives the GPU with deliberately invalid input once it passes the
# limit. Undefined behaviour is the point of the exercise, so run it with the
# device reachable, not unattended.
set -e
cd "$(dirname "$0")"

find_ndk() {
	[ -n "$ANDROID_NDK" ] && { echo "$ANDROID_NDK"; return; }
	[ -n "$ANDROID_NDK_HOME" ] && { echo "$ANDROID_NDK_HOME"; return; }
	[ -n "$ANDROID_NDK_ROOT" ] && { echo "$ANDROID_NDK_ROOT"; return; }
	for sdk in "$ANDROID_SDK_ROOT" "$ANDROID_HOME" \
		"$HOME/Android/Sdk" "$HOME/.local/share/android/sdk"; do
		[ -d "$sdk/ndk" ] || continue
		ndk=$(ls -1 "$sdk/ndk" 2>/dev/null | sort -V | tail -1)
		[ -n "$ndk" ] && { echo "$sdk/ndk/$ndk"; return; }
	done
}

NDK=$(find_ndk)
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "no NDK found -- set ANDROID_NDK" >&2; exit 1; }
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
[ -x "$CC" ] || { echo "no aarch64 clang in $NDK" >&2; exit 1; }

# glslc ships with the NDK; a host one is fine too.
GLSLC="$NDK/shader-tools/linux-x86_64/glslc"
[ -x "$GLSLC" ] || GLSLC=$(command -v glslc) || true
[ -n "$GLSLC" ] && [ -x "$GLSLC" ] || { echo "no glslc found" >&2; exit 1; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
"$CC" -O2 -Wall vkpush.c -lvulkan -o "$OUT/vkpush"
adb push "$OUT/vkpush" /data/local/tmp/ >/dev/null
adb shell chmod +x /data/local/tmp/vkpush

# The shader is generated per N: each uniform buffer gets its own binding and
# its own name, so nothing depends on dynamic indexing of a descriptor array
# (which is a separate feature and would confound the measurement).
gen_shader() {
	n=$1
	{
		echo "#version 450"
		echo "layout(local_size_x = 1) in;"
		echo "layout(set = 1, binding = 0) buffer Result { uint sum; } r;"
		# One binding, an array of n. Indices are constants, so this needs no
		# dynamic-indexing feature -- that is a separate capability and would
		# confound the measurement.
		if [ "$TYPE" = ssbo ]; then
			echo "layout(set = 0, binding = 0) readonly buffer U { uint v; } u[$n];"
		else
			echo "layout(set = 0, binding = 0) uniform U { uint v; } u[$n];"
		fi
		echo "void main() {"
		echo "  uint s = 0u;"
		i=0
		while [ "$i" -lt "$n" ]; do
			echo "  s += u[$i].v;"
			i=$((i + 1))
		done
		echo "  r.sum = s;"
		echo "}"
	} > "$OUT/push_$n.comp"
	"$GLSLC" -fshader-stage=comp "$OUT/push_$n.comp" -o "$OUT/push_$n.spv"
}

# TYPE=ssbo escapes the USC compiler's 15-uniform-buffer ceiling; see README.
TYPE=${TYPE:-ubo}
[ "$TYPE" = ssbo ] && MODE=ssbo || MODE=""
LIST=${*:-"1 4 8 16 24 32 33 40 48 64 96 128 192 256"}
last_ok=0
for n in $LIST; do
	if ! gen_shader "$n" 2>/dev/null; then
		echo "N=$n SHADER-COMPILE-FAIL (host-side limit, not the driver's)"
		continue
	fi
	adb push "$OUT/push_$n.spv" /data/local/tmp/ >/dev/null
	out=$(adb shell "/data/local/tmp/vkpush $n /data/local/tmp/push_$n.spv $MODE; echo rc=\$?" 2>&1)
	rc=$(echo "$out" | sed -n 's/.*rc=\([0-9]*\).*/\1/p' | tail -1)
	echo "$out" | grep -v '^rc=' | sed 's/^/  /'
	case "$rc" in
	0) last_ok=$n ;;
	*)
		echo
		echo "stopped at N=$n (exit $rc). Largest N producing the correct sum: $last_ok"
		# Prove the device is still alive before believing anything above.
		adb shell 'echo device-alive' 2>&1 | sed 's/^/  /'
		exit 0
		;;
	esac
done
echo
echo "every N in the list produced the correct sum. Largest tested: $last_ok"
