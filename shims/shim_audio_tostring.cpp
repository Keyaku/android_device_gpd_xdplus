/*
 * android::hardware::audio::V2_0::toString(Result) shim for the prebuilt 8.1
 * vendor audio blobs. Old hidl-gen exported this from the HIDL lib; R makes it
 * inline, so the symbol vanished. One real implementation, matching the O-era
 * enum values.
 */
#include <string>

namespace android {
namespace hardware {
namespace audio {
namespace V2_0 {

enum class Result : int32_t;

std::string toString(Result r) {
	switch (static_cast<int32_t>(r)) {
		case 0: return "OK";
		case -1: return "NOT_INITIALIZED";
		case -2: return "INVALID_ARGUMENTS";
		case -3: return "INVALID_STATE";
		case -4: return "NOT_SUPPORTED";
		default: return std::to_string(static_cast<int32_t>(r));
	}
}

}  // namespace V2_0
}  // namespace audio
}  // namespace hardware
}  // namespace android
