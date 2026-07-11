/*
 * android::hardware::wifi::V1_0::toString(sp<IWifiChipEventCallback> const&)
 * shim for the prebuilt 8.1 vendor android.hardware.wifi@1.0-service. O-era
 * hidl-gen exported interface-sp toString overloads from the HIDL lib; R
 * dropped them, so the vendor binary can't link. Logging-only helper — return
 * the same shape the generated one did.
 */
#include <string>

#include <utils/StrongPointer.h>

namespace android {
namespace hardware {
namespace wifi {
namespace V1_0 {

struct IWifiChipEventCallback;

std::string toString(const ::android::sp<IWifiChipEventCallback>& o) {
	std::string os = "[class or subclass of ";
	os += "android.hardware.wifi@1.0::IWifiChipEventCallback";
	os += "]";
	os += (o == nullptr) ? "(null)" : "@";
	return os;
}

}  // namespace V1_0
}  // namespace wifi
}  // namespace hardware
}  // namespace android
