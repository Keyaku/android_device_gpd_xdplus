/*
 * System-side configstore@1.1 HAL for xdplus.
 *
 * The prebuilt 8.1 vendor's android.hardware.configstore@1.0-service crash-loops
 * with SIGSYS: its minijail seccomp policy (written for the 8.1 userspace) kills
 * the process on syscalls made by the A11 libs it now links against. It therefore
 * never registers ISurfaceFlingerConfigs — and the MTK/PowerVR GPU userspace (GED)
 * loaded into every app blocks its RenderThread forever in
 * getService("android.hardware.configstore@1.0::ISurfaceFlingerConfigs"), so no
 * app window ever draws (splash forever, mCurrentFocus=null, "unresponsive UI").
 *
 * This service registers the same interfaces from /system, without minijail
 * (system service, no vendor seccomp policy applies). Values match BoardConfig:
 * NUM_FRAMEBUFFER_SURFACE_BUFFERS=3, PRESENT_TIME_OFFSET_FROM_VSYNC_NS=0,
 * everything else left "unspecified" so clients use their own defaults.
 */

#define LOG_TAG "configstore@1.1-service.xdplus"

#include <unistd.h>

#include <android-base/properties.h>
#include <android/hardware/configstore/1.1/ISurfaceFlingerConfigs.h>
#include <hidl/HidlTransportSupport.h>
#include <android/hidl/manager/1.0/IServiceManager.h>
#include <hidl/ServiceManagement.h>
#include <log/log.h>

using android::sp;
using android::status_t;
using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;
using android::hardware::Return;
using android::hardware::Void;
using android::hardware::configstore::V1_1::DisplayOrientation;
using android::hardware::configstore::V1_1::ISurfaceFlingerConfigs;

namespace {

struct SurfaceFlingerConfigs : public ISurfaceFlingerConfigs {
	// V1_0
	Return<void> vsyncEventPhaseOffsetNs(vsyncEventPhaseOffsetNs_cb cb) override {
		cb({false, 0});
		return Void();
	}
	Return<void> vsyncSfEventPhaseOffsetNs(vsyncSfEventPhaseOffsetNs_cb cb) override {
		cb({false, 0});
		return Void();
	}
	Return<void> useContextPriority(useContextPriority_cb cb) override {
		cb({false, false});
		return Void();
	}
	Return<void> hasWideColorDisplay(hasWideColorDisplay_cb cb) override {
		cb({true, false});
		return Void();
	}
	Return<void> hasHDRDisplay(hasHDRDisplay_cb cb) override {
		cb({true, false});
		return Void();
	}
	Return<void> presentTimeOffsetFromVSyncNs(presentTimeOffsetFromVSyncNs_cb cb) override {
		cb({true, 0});
		return Void();
	}
	Return<void> useHwcForRGBtoYUV(useHwcForRGBtoYUV_cb cb) override {
		cb({true, false});
		return Void();
	}
	Return<void> maxVirtualDisplaySize(maxVirtualDisplaySize_cb cb) override {
		cb({false, 0});
		return Void();
	}
	Return<void> hasSyncFramework(hasSyncFramework_cb cb) override {
		cb({true, true});
		return Void();
	}
	Return<void> useVrFlinger(useVrFlinger_cb cb) override {
		cb({false, false});
		return Void();
	}
	Return<void> maxFrameBufferAcquiredBuffers(maxFrameBufferAcquiredBuffers_cb cb) override {
		cb({true, 3});  // NUM_FRAMEBUFFER_SURFACE_BUFFERS in BoardConfig.mk
		return Void();
	}
	Return<void> startGraphicsAllocatorService(startGraphicsAllocatorService_cb cb) override {
		cb({true, false});
		return Void();
	}
	// V1_1
	Return<void> primaryDisplayOrientation(primaryDisplayOrientation_cb cb) override {
		cb({false, DisplayOrientation::ORIENTATION_0});
		return Void();
	}
};

}  // namespace

int main() {
	// Boot-time race seen on device: registering before hwservicemanager is
	// fully up leaves this process alive but the interface unregistered (and
	// every GED client blocked again). Wait for the manager, register, then
	// keep verifying the registration — exit(1) lets init restart us cleanly.
	android::base::WaitForProperty("hwservicemanager.ready", "true");

	configureRpcThreadpool(4, false /* callerWillJoin */);

	sp<ISurfaceFlingerConfigs> configs = new SurfaceFlingerConfigs;
	status_t status = configs->registerAsService();
	if (status != android::OK) {
		ALOGE("Could not register ISurfaceFlingerConfigs (%d), exiting for init restart", status);
		return 1;
	}
	ALOGI("ISurfaceFlingerConfigs 1.0/1.1 registered from /system");

	using android::hidl::manager::V1_0::IServiceManager;
	sp<IServiceManager> sm = android::hardware::defaultServiceManager();
	while (true) {
		sleep(30);
		if (sm == nullptr) sm = android::hardware::defaultServiceManager();
		if (sm == nullptr) continue;
		auto ret = sm->get(ISurfaceFlingerConfigs::descriptor, "default");
		sp<android::hidl::base::V1_0::IBase> base = ret.isOk() ? sp<android::hidl::base::V1_0::IBase>(ret) : nullptr;
		if (base == nullptr) {
			ALOGE("ISurfaceFlingerConfigs registration lost, exiting for init restart");
			return 1;
		}
	}
}
