#!/bin/sh
# maxPushDescriptors, the fragment/combined-image-sampler half.
#
# pushfunc.sh measured a floor of 32 with storage buffers in a compute shader.
# maxPushDescriptors is one number across every descriptor type and stage, so
# that left the commonest real use of the extension -- textures in a fragment
# shader -- untested. This runs the same escalation with N combined image
# samplers instead.
#
# Each N runs in its OWN process against a fresh VkDevice, for the same reasons
# as pushfunc.sh: a device-lost poisons everything after it, and a hang must not
# take the rest of the run with it.
#
# ⚠️ Past the limit this drives the GPU with deliberately invalid input.
# Undefined behaviour is the point, so run it with the device reachable, not
# unattended.
#
#   ./pushimg.sh [N...]        default sweep
#   MODE=nopush ./pushimg.sh   the control: same N through an ordinary set
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

GLSLC="$NDK/shader-tools/linux-x86_64/glslc"
[ -x "$GLSLC" ] || GLSLC=$(command -v glslc) || true
[ -n "$GLSLC" ] && [ -x "$GLSLC" ] || { echo "no glslc found" >&2; exit 1; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
"$CC" -O2 -Wall vkpushimg.c -lvulkan -o "$OUT/vkpushimg"
adb push "$OUT/vkpushimg" /data/local/tmp/ >/dev/null
adb shell chmod +x /data/local/tmp/vkpushimg

# A covering triangle from gl_VertexIndex alone -- no vertex buffers, so nothing
# in the measurement depends on vertex input.
cat > "$OUT/tri.vert" <<'EOF'
#version 450
void main() {
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
EOF
"$GLSLC" -fshader-stage=vert "$OUT/tri.vert" -o "$OUT/tri.spv"
adb push "$OUT/tri.spv" /data/local/tmp/ >/dev/null

# One binding, an array of n, constant indices -- dynamic indexing of a
# descriptor array is a separate feature and would confound the measurement.
gen_frag() {
	n=$1
	{
		echo "#version 450"
		echo "layout(set = 0, binding = 0) uniform sampler2D u[$n];"
		echo "layout(location = 0) out vec4 o;"
		echo "void main() {"
		echo "  float s = 0.0;"
		i=0
		while [ "$i" -lt "$n" ]; do
			# Texel i holds i+1 as a byte, so *255 recovers the integer exactly.
			echo "  s += texture(u[$i], vec2(0.5)).r * 255.0;"
			i=$((i + 1))
		done
		echo "  o = vec4(s, 0.0, 0.0, 1.0);"
		echo "}"
	} > "$OUT/frag_$n.frag"
	"$GLSLC" -fshader-stage=frag "$OUT/frag_$n.frag" -o "$OUT/frag_$n.spv"
}

MODE=${MODE:-}
LIST=${*:-"1 4 8 16 24 32 33 40 48 64"}
last_ok=0
for n in $LIST; do
	if ! gen_frag "$n" 2>/dev/null; then
		echo "N=$n SHADER-COMPILE-FAIL (host-side limit, not the driver's)"
		continue
	fi
	adb push "$OUT/frag_$n.spv" /data/local/tmp/ >/dev/null
	out=$(adb shell "/data/local/tmp/vkpushimg $n /data/local/tmp/tri.spv /data/local/tmp/frag_$n.spv $MODE; echo rc=\$?" 2>&1)
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
