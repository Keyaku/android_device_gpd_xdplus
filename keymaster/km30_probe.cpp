// xdplus TEE keymaster@3.0 probe. Builds the 3.0 HIDL device IN-PROCESS exactly
// as km30_service does (hw_get_module keystore -> km1_open -> ng::CreateKeymaster-
// Device) and calls its methods directly — no binder, no servicemanager, no VINTF
// manifest — to isolate whether the TEE-backed device itself works.
// getHardwareFeatures isSecure=true proves it is trustlet-backed (not software);
// generateKey err=OK proves a real key op runs through the 3.0 HIDL surface.
#define LOG_TAG "km30_probe"

#include <android/hardware/keymaster/3.0/IKeymasterDevice.h>
#include <android/hardware/keymaster/3.0/types.h>
#include <hidl/HidlSupport.h>
#include <stdio.h>
#include <unistd.h>

#include <AndroidKeymaster3Device.h>
#include <hardware/hardware.h>
#include <hardware/keymaster1.h>
#include <hardware/keymaster_common.h>

using android::sp;
using android::hardware::hidl_vec;
using android::hardware::keymaster::V3_0::ErrorCode;
using android::hardware::keymaster::V3_0::IKeymasterDevice;
using android::hardware::keymaster::V3_0::KeyCharacteristics;
using android::hardware::keymaster::V3_0::KeyParameter;
using android::hardware::keymaster::V3_0::Tag;
using android::hardware::keymaster::V3_0::TagType;
using android::hardware::keymaster::V3_0::KeyPurpose;
using android::hardware::keymaster::V3_0::Algorithm;
using android::hardware::keymaster::V3_0::BlockMode;
using android::hardware::keymaster::V3_0::PaddingMode;

static KeyParameter enumParam(Tag tag, uint32_t v) {
	KeyParameter p;
	p.tag = tag;
	p.f.integer = v;
	return p;
}
static KeyParameter uintParam(Tag tag, uint32_t v) {
	KeyParameter p;
	p.tag = tag;
	p.f.integer = v;
	return p;
}
static KeyParameter boolParam(Tag tag) {
	KeyParameter p;
	p.tag = tag;
	p.f.boolValue = true;
	return p;
}

int main() {
	const hw_module_t* mod = nullptr;
	int rc = hw_get_module_by_class(KEYSTORE_HARDWARE_MODULE_ID, NULL, &mod);
	if (rc) {
		printf("KM30PROBE-FAIL: hw_get_module(keystore)=%d\n", rc);
		return 1;
	}
	printf("keystore module: %s api=0x%x\n", mod->name ? mod->name : "?",
	       mod->module_api_version);
	keymaster1_device_t* dev = nullptr;
	rc = keymaster1_open(mod, &dev);
	if (rc || !dev) {
		printf("KM30PROBE-FAIL: keymaster1_open=%d\n", rc);
		return 1;
	}
	sp<IKeymasterDevice> km = ::keymaster::ng::CreateKeymasterDevice(dev);
	if (km == nullptr) {
		printf("KM30PROBE-FAIL: CreateKeymasterDevice returned null\n");
		return 1;
	}
	printf("built in-process @3.0 device from TEE km1\n");

	km->getHardwareFeatures([](bool isSecure, bool ec, bool sym, bool attest,
				   bool digests, const auto& name, const auto& author) {
		printf("getHardwareFeatures: isSecure=%d ec=%d sym=%d attest=%d "
		       "digests=%d name=%s author=%s\n",
		       isSecure, ec, sym, attest, digests, name.c_str(),
		       author.c_str());
		printf(isSecure ? "KM30PROBE-SECURE: TEE-backed\n"
				: "KM30PROBE-SOFTWARE: fell back to software\n");
	});

	std::vector<KeyParameter> params{
		enumParam(Tag::ALGORITHM, (uint32_t)Algorithm::AES),
		uintParam(Tag::KEY_SIZE, 128),
		enumParam(Tag::PURPOSE, (uint32_t)KeyPurpose::ENCRYPT),
		enumParam(Tag::PURPOSE, (uint32_t)KeyPurpose::DECRYPT),
		enumParam(Tag::BLOCK_MODE, (uint32_t)BlockMode::ECB),
		enumParam(Tag::PADDING, (uint32_t)PaddingMode::NONE),
		boolParam(Tag::NO_AUTH_REQUIRED),
	};
	ErrorCode gErr = ErrorCode::UNKNOWN_ERROR;
	km->generateKey(hidl_vec<KeyParameter>(params),
			[&](ErrorCode e, const hidl_vec<uint8_t>& blob,
			    const KeyCharacteristics&) {
				gErr = e;
				printf("generateKey err=%d blobBytes=%zu\n", (int)e,
				       blob.size());
			});
	if (gErr == ErrorCode::OK)
		printf("KM30PROBE-GENKEY-OK: real key op via 3.0 HIDL\n");
	else
		printf("KM30PROBE-GENKEY-ERR: %d\n", (int)gErr);
	return 0;
}
