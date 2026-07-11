/*
 * System-side media.omx@1.0 HAL for xdplus.
 *
 * The prebuilt 8.1 vendor android.hardware.media.omx@1.0-service can't link:
 * it wants O-era libstagefright symbols (MediaCodecsXmlParser::defaultSearchDirs,
 * the old OmxStore ctor) whose classes changed ABI in R — unshimmable without
 * object-layout roulette. Instead run R's own OMX service from /system (same
 * pattern as configstore/dumpstate). R's OMXMaster dlopens the vendor codec
 * plugin (libstagefrighthw.so, MTK) through the O-era-stable OMXPluginBase
 * interface, so hardware codecs still come from the vendor blob.
 *
 * This is frameworks/av/services/mediacodec/main_codecservice.cpp minus
 * minijail (the vendor policy predates our userspace; system policy optional).
 */

#define LOG_TAG "omx@1.0-service.xdplus"

#include <android-base/logging.h>
#include <binder/ProcessState.h>
#include <cutils/properties.h>
#include <hidl/HidlTransportSupport.h>
#include <media/stagefright/omx/1.0/Omx.h>
#include <media/stagefright/omx/1.0/OmxStore.h>

#include <signal.h>

using namespace android;

int main(int argc __unused, char** argv) {
	strcpy(argv[0], "media.codec");
	LOG(INFO) << "xdplus system omx service starting";
	signal(SIGPIPE, SIG_IGN);

	android::ProcessState::initWithDriver("/dev/vndbinder");
	android::ProcessState::self()->startThreadPool();

	::android::hardware::configureRpcThreadpool(64, false);

	using namespace ::android::hardware::media::omx::V1_0;
	sp<IOmx> omx = new implementation::Omx();
	if (omx == nullptr) {
		LOG(ERROR) << "Cannot create IOmx HAL service.";
	} else if (omx->registerAsService() != OK) {
		LOG(ERROR) << "Cannot register IOmx HAL service.";
	} else {
		LOG(INFO) << "IOmx HAL service created.";
	}
	sp<IOmxStore> omxStore = new implementation::OmxStore(
	        property_get_int64("vendor.media.omx", 1) ? omx : nullptr);
	if (omxStore == nullptr) {
		LOG(ERROR) << "Cannot create IOmxStore HAL service.";
	} else if (omxStore->registerAsService() != OK) {
		LOG(ERROR) << "Cannot register IOmxStore HAL service.";
	}

	::android::hardware::joinRpcThreadpool();
}
