// HWC2 wrapper around the prebuilt MediaTek composer, to keep the mini-HDMI mirror
// alive while the primary display is client composing.
//
// The blob keeps one validate/present state word per display and stamps every display
// from whichever one is being serviced. HWCMediator::displayValidateDisplay gates on
// that word: PRESENT_DONE runs the whole prepare block and then validate(), CSV runs
// validate(), and any higher state skips both -- yet every path falls through the same
// tail, which stamps VALIDATE_DONE.
//
// That stamp is the fault. While the mirror is healthy SurfaceFlinger never validates
// the external display at all: its present is entered in PRESENT, returns inert, and
// stamps only itself, so the primary's next present is entered in PRESENT_DONE, the one
// state that makes the blob build the frame's job. The moment SurfaceFlinger marks any
// layer on the external display CLIENT it starts calling validateDisplay for it. Entered
// above CSV that call does nothing except stamp VALIDATE_DONE, which moves the external
// display's present onto the real path; the real path stamps PRESENT on every display,
// so the primary's present is never entered in PRESENT_DONE again, no job is ever built,
// and both screens stop submitting until the composer is bounced.
//
// So the external display's validate is dropped exactly when the blob would do no work
// in it. States PRESENT_DONE and CSV are passed through untouched -- those are the calls
// that build the mirror, and suppressing them unconditionally loses it.
//
// Verified on both screens: with this in place the panel no longer freezes, and the
// external display, which is still declined while an app marking its layers CLIENT is in
// the foreground, comes back on its own when that app is left.
//
// The blob is loaded by its own name and is not renamed or shadowed. This module is
// selected instead of it with ro.hardware.hwcomposer=xdplus, so reverting to stock
// behaviour is a property away.

#define LOG_TAG "xdplus-hwcshim"

#include <atomic>

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <cutils/properties.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer2.h>
#include <log/log.h>

namespace {

const char* const REAL_PATHS[] = {
	"/vendor/lib64/hw/hwcomposer.mt8173.so",
	"/vendor/lib/hw/hwcomposer.mt8173.so",
};

// HWCDisplay's m_vali_present_state, read out of the shipped blob's displayPresent
// (0x371e0) and its displayValidateDisplay (0x3b82c).
constexpr uintptr_t MEDIATOR_DISPLAYS_OFF = 0x78;
constexpr uintptr_t DISPLAY_STATE_OFF = 0x12c;

// Above this the blob's validate gate (cmp #1 / b.hi) skips both the prepare block and
// validate(), leaving the tail stamp as the call's only effect.
constexpr int32_t STATE_NOOP_ABOVE = 1;

using GetInstanceFn = void* (*)();

hwc2_device_t* gReal = nullptr;
void* gLib = nullptr;
GetInstanceFn gGetInstance = nullptr;
bool gStateReadable = false;

HWC2_PFN_VALIDATE_DISPLAY gRealValidate = nullptr;
HWC2_PFN_PRESENT_DISPLAY gRealPresent = nullptr;

// Both properties are re-read periodically rather than latched, so behaviour can be
// A/B'd and the log armed without restarting the composer.
bool cachedProperty(const char* name, std::atomic<bool>* value, std::atomic<unsigned>* tick) {
	if ((tick->fetch_add(1, std::memory_order_relaxed) & 0x3f) == 0) {
		value->store(property_get_bool(name, false), std::memory_order_relaxed);
	}
	return value->load(std::memory_order_relaxed);
}

// Escape hatch: pass every call straight through, i.e. behave as the bare blob.
bool bypass() {
	static std::atomic<bool> value{false};
	static std::atomic<unsigned> tick{0};
	return cachedProperty("persist.sys.xdplus.hwcshim_bypass", &value, &tick);
}

bool logging() {
	static std::atomic<bool> value{false};
	static std::atomic<unsigned> tick{0};
	return cachedProperty("debug.xdplus.hwcshim_log", &value, &tick);
}

void* displayObject(hwc2_display_t display) {
	if (!gStateReadable) return nullptr;
	uintptr_t mediator = reinterpret_cast<uintptr_t>(gGetInstance());
	if (!mediator) return nullptr;
	uintptr_t vec = *reinterpret_cast<uintptr_t*>(mediator + MEDIATOR_DISPLAYS_OFF);
	if (!vec) return nullptr;
	return *reinterpret_cast<void**>(vec + sizeof(void*) * display);
}

int32_t displayState(hwc2_display_t display) {
	void* d = displayObject(display);
	if (!d) return -1;
	return *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(d) + DISPLAY_STATE_OFF);
}

void setDisplayState(hwc2_display_t display, int32_t state) {
	void* d = displayObject(display);
	if (!d) return;
	*reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(d) + DISPLAY_STATE_OFF) = state;
}

// Entered above CSV the blob's validate skips its work and reports no changed types, so
// SurfaceFlinger never learns the external needs client composition and stops feeding it a
// client target. Writing the state back to CSV makes the call do its work.
bool forceExternalValidate() {
	static std::atomic<bool> value{false};
	static std::atomic<unsigned> tick{0};
	return cachedProperty("persist.sys.xdplus.hwcshim_forcevali", &value, &tick);
}

int32_t onValidate(hwc2_device_t* device, hwc2_display_t display, uint32_t* outNumTypes,
				   uint32_t* outNumRequests) {
	int32_t before = displayState(display);

	if (!bypass() && display != HWC_DISPLAY_PRIMARY && before > STATE_NOOP_ABOVE &&
		forceExternalValidate()) {
		setDisplayState(display, STATE_NOOP_ABOVE);
		if (logging()) {
			ALOGD("validate(%" PRIu64 ") state %d forced to %d", display, before,
				  STATE_NOOP_ABOVE);
		}
		before = STATE_NOOP_ABOVE;
	}

	// A negative state means the offsets did not resolve, so nothing is known about what
	// this call would do -- pass it through rather than guess.
	if (!bypass() && display != HWC_DISPLAY_PRIMARY && before > STATE_NOOP_ABOVE) {
		*outNumTypes = 0;
		*outNumRequests = 0;
		if (logging()) {
			ALOGD("validate(%" PRIu64 ") dropped, state=%d would be a no-op", display, before);
		}
		return HWC2_ERROR_NONE;
	}

	const int32_t err = gRealValidate(device, display, outNumTypes, outNumRequests);
	if (logging()) {
		ALOGD("validate(%" PRIu64 ") state %d -> %d, err=%d types=%u reqs=%u", display, before,
			  displayState(display), err, *outNumTypes, *outNumRequests);
	}
	return err;
}

int32_t onPresent(hwc2_device_t* device, hwc2_display_t display, int32_t* outPresentFence) {
	const int32_t before = logging() ? displayState(display) : -1;
	const int32_t err = gRealPresent(device, display, outPresentFence);
	if (logging()) {
		ALOGD("present(%" PRIu64 ") state %d -> %d, err=%d fence=%d", display, before,
			  displayState(display), err, *outPresentFence);
	}
	return err;
}

hwc2_function_pointer_t (*gRealGetFunction)(hwc2_device_t*, int32_t) = nullptr;

hwc2_function_pointer_t onGetFunction(hwc2_device_t* device, int32_t descriptor) {
	hwc2_function_pointer_t real = gRealGetFunction(device, descriptor);
	if (!real) return nullptr;

	switch (descriptor) {
		case HWC2_FUNCTION_VALIDATE_DISPLAY:
			gRealValidate = reinterpret_cast<HWC2_PFN_VALIDATE_DISPLAY>(real);
			return reinterpret_cast<hwc2_function_pointer_t>(onValidate);
		case HWC2_FUNCTION_PRESENT_DISPLAY:
			gRealPresent = reinterpret_cast<HWC2_PFN_PRESENT_DISPLAY>(real);
			return reinterpret_cast<hwc2_function_pointer_t>(onPresent);
		default:
			return real;
	}
}

int shimOpen(const hw_module_t* /*module*/, const char* name, hw_device_t** device) {
	for (const char* path : REAL_PATHS) {
		gLib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (gLib) break;
	}
	if (!gLib) {
		ALOGE("cannot load real composer: %s", dlerror());
		return -EINVAL;
	}

	auto* hmi = reinterpret_cast<hw_module_t*>(dlsym(gLib, HAL_MODULE_INFO_SYM_AS_STR));
	if (!hmi || !hmi->methods || !hmi->methods->open) {
		ALOGE("real composer has no usable module symbol");
		return -EINVAL;
	}

	hw_device_t* realDevice = nullptr;
	const int err = hmi->methods->open(hmi, name, &realDevice);
	if (err || !realDevice) {
		ALOGE("real composer open failed: %d", err);
		return err ? err : -EINVAL;
	}
	gReal = reinterpret_cast<hwc2_device_t*>(realDevice);

	gGetInstance = reinterpret_cast<GetInstanceFn>(
			dlsym(gLib, "_ZN7android9SingletonI11HWCMediatorE11getInstanceEv"));
	gStateReadable = gGetInstance != nullptr;
	if (!gStateReadable) {
		// Without the state word every call has to be passed through, which is stock
		// behaviour. Loud, because it means a blob this was not built against.
		ALOGE("HWCMediator::getInstance not found -- passing everything through");
	}

	// Patch in place: unwrapped entry points must get the blob's own device.
	gRealGetFunction = gReal->getFunction;
	gReal->getFunction = onGetFunction;

	ALOGI("wrapped real composer %p", gReal);
	*device = &gReal->common;
	return 0;
}

hw_module_methods_t gMethods = {
	.open = shimOpen,
};

}  // namespace

hw_module_t HAL_MODULE_INFO_SYM = {
	.tag = HARDWARE_MODULE_TAG,
	.version_major = 2,
	.version_minor = 0,
	.id = HWC_HARDWARE_MODULE_ID,
	.name = "xdplus hwcomposer shim",
	.author = "xdplus",
	.methods = &gMethods,
};
