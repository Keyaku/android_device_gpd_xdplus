/*
** Copyright 2016, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

// xdplus: binderized keymaster@3.0 service backed by the MT8173 TEE.
//
// It hw_get_module("keystore")s the real trustlet HAL keystore.mt8173.so (a
// keymaster1 hw_module, "Keymaster MTEE based HAL", reachable only once
// libtz_uree.so was restored to /vendor — TEE spike) and wraps
// it to a 3.0 HIDL IKeymasterDevice with the R ng:: factory (libkeymaster3device,
// keymaster1_passthrough_context path). We deliberately do NOT reuse the vendor
// android.hardware.keymaster@3.0-impl.so: that blob is an O-era build linked
// against SoftKeymasterContext::ParseKeyBlob, which R's /system libsoftkeymaster-
// device.so no longer exports, so the legacy single-namespace linker fails it.
// The ng path avoids SoftKeymasterContext entirely and links only R libs.
//
// keystore's built-in support::Keymaster3 adapter presents this device to the
// framework as a 4.x TRUSTED_ENVIRONMENT keymaster (real hardware-backed keys
// -> working FBE). Runs isolated so a TEE load failure cannot abort keystore.
#define LOG_TAG "android.hardware.keymaster@3.0-service.xdplus"

#include <android/hardware/keymaster/3.0/IKeymasterDevice.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include <AndroidKeymaster3Device.h>
#include <hardware/hardware.h>
#include <hardware/keymaster0.h>
#include <hardware/keymaster1.h>
#include <hardware/keymaster2.h>
#include <hardware/keymaster_common.h>
#include <hardware/keymaster_defs.h>

using android::sp;
using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;
using android::hardware::keymaster::V3_0::IKeymasterDevice;

static IKeymasterDevice* createTeeKeymaster3Device() {
	const hw_module_t* mod = nullptr;
	int rc = hw_get_module_by_class(KEYSTORE_HARDWARE_MODULE_ID, NULL, &mod);
	if (rc) {
		ALOGE("hw_get_module(keystore) failed: %d — no TEE keymaster", rc);
		return nullptr;
	}
	ALOGI("keystore module: %s api=0x%x", mod->name ? mod->name : "?",
	      mod->module_api_version);

	if (mod->module_api_version < KEYMASTER_MODULE_API_VERSION_1_0) {
		keymaster0_device_t* dev = nullptr;
		if (keymaster0_open(mod, &dev)) {
			ALOGE("keymaster0_open failed");
			return nullptr;
		}
		return ::keymaster::ng::CreateKeymasterDevice(dev);
	} else if (mod->module_api_version == KEYMASTER_MODULE_API_VERSION_1_0) {
		keymaster1_device_t* dev = nullptr;
		int r = keymaster1_open(mod, &dev);
		if (r) {
			ALOGE("keymaster1_open failed: %d", r);
			if (dev) dev->common.close(&dev->common);
			return nullptr;
		}
		return ::keymaster::ng::CreateKeymasterDevice(dev);
	} else {
		keymaster2_device_t* dev = nullptr;
		if (keymaster2_open(mod, &dev)) {
			ALOGE("keymaster2_open failed");
			return nullptr;
		}
		return ::keymaster::ng::CreateKeymasterDevice(dev);
	}
}

int main() {
	configureRpcThreadpool(1, true /* willJoinThreadpool */);

	sp<IKeymasterDevice> keymaster = createTeeKeymaster3Device();
	if (keymaster == nullptr) {
		ALOGE("Failed to create TEE-backed keymaster@3.0 device");
		return 1;
	}

	auto status = keymaster->registerAsService("default");
	if (status != android::OK) {
		ALOGE("registerAsService(default) failed: %d", status);
		return 1;
	}
	ALOGI("keymaster@3.0 TEE service registered");

	joinRpcThreadpool();
	return 1;  // Should never reach here.
}
