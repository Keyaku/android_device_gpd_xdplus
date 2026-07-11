/*
 * Minimal system-side dumpstate@1.1 HAL for xdplus.
 *
 * The 8.1 vendor ships no dumpstate HAL at all, and with VINTF unenforced a
 * client's getService(IDumpstateDevice) blocks forever instead of returning
 * null — Settings' Developer-options dashboard does exactly that on resume
 * (BugReportHandler preference) and ANRs. A registered stub that dumps
 * nothing keeps every client responsive.
 */

#define LOG_TAG "dumpstate@1.1-service.xdplus"

#include <android-base/properties.h>
#include <android/hardware/dumpstate/1.1/IDumpstateDevice.h>
#include <android/hardware/dumpstate/1.1/types.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

using android::sp;
using android::status_t;
using android::hardware::configureRpcThreadpool;
using android::hardware::hidl_handle;
using android::hardware::joinRpcThreadpool;
using android::hardware::Return;
using android::hardware::Void;
using android::hardware::dumpstate::V1_1::DumpstateMode;
using android::hardware::dumpstate::V1_1::DumpstateStatus;
using android::hardware::dumpstate::V1_1::IDumpstateDevice;

namespace {

const char kVerboseLoggingProperty[] = "persist.dumpstate.verbose_logging.enabled";

struct DumpstateDevice : public IDumpstateDevice {
	Return<void> dumpstateBoard(const hidl_handle& /*handle*/) override {
		return Void();
	}
	Return<DumpstateStatus> dumpstateBoard_1_1(const hidl_handle& /*handle*/,
	                                           DumpstateMode /*mode*/,
	                                           uint64_t /*timeoutMillis*/) override {
		return DumpstateStatus::UNSUPPORTED_MODE;
	}
	Return<void> setVerboseLoggingEnabled(bool enable) override {
		android::base::SetProperty(kVerboseLoggingProperty, enable ? "true" : "false");
		return Void();
	}
	Return<bool> getVerboseLoggingEnabled() override {
		return android::base::GetBoolProperty(kVerboseLoggingProperty, false);
	}
};

}  // namespace

int main() {
	android::base::WaitForProperty("hwservicemanager.ready", "true");
	configureRpcThreadpool(2, true /* callerWillJoin */);

	sp<IDumpstateDevice> dumpstate = new DumpstateDevice;
	status_t status = dumpstate->registerAsService();
	if (status != android::OK) {
		ALOGE("Could not register IDumpstateDevice (%d), exiting for init restart", status);
		return 1;
	}
	ALOGI("IDumpstateDevice 1.0/1.1 registered from /system");

	joinRpcThreadpool();
	return 0;
}
