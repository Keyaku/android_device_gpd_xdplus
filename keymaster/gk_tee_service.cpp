/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// xdplus: binderized gatekeeper@1.0 service backed by the MT8173 TEE.
//
// It hw_get_module("gatekeeper")s the real trustlet HAL gatekeeper.mt8173.so
// (a legacy gatekeeper_device_t, "MTK HAREWARE GateKeeper HAL", reachable once
// libtz_uree.so was restored to /vendor — TEE spike, PORTING_LOG §29; gatekeeper
// spike §31 proved it enrolls+verifies with a 69-byte HardwareAuthToken and
// needs NO libgatekeeper_mtee.so, its only TEE dep being libtz_uree).
//
// Why this fixes the §30 regression: the gatekeeper trustlet and the keymaster
// trustlet share the TEE root of trust, so the HardwareAuthToken this HAL signs
// is trusted by the HW keymaster (§30). With the software gatekeeper, keymaster
// rejected the token — BeginOperation:-26 KEY_USER_NOT_AUTHENTICATED — breaking
// PIN/lockscreen and every auth-bound key. TEE gatekeeper closes that gap.
//
// Defensive fallback (mirrors the §30 keystore tryGetService guard): if the
// trustlet HAL fails to open, register the AOSP software gatekeeper instead so
// a misconfigured TEE degrades to no-HW-auth rather than crash-looping and
// leaving the framework with no IGatekeeper at all.
#define LOG_TAG "android.hardware.gatekeeper@1.0-service.xdplus"

#include <android/hardware/gatekeeper/1.0/IGatekeeper.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include "gk_tee_adapter.h"
#include "SoftGateKeeperDevice.h"

using android::sp;
using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;
using android::hardware::gatekeeper::V1_0::IGatekeeper;
using android::hardware::gatekeeper::V1_0::implementation::Gatekeeper;
using android::SoftGateKeeperDevice;

int main() {
	configureRpcThreadpool(1, true /* willJoinThreadpool */);

	sp<IGatekeeper> gatekeeper;

	sp<Gatekeeper> tee(new Gatekeeper());
	if (tee->isOpen()) {
		ALOGI("TEE gatekeeper trustlet opened — registering hardware gatekeeper");
		gatekeeper = tee;
	} else {
		ALOGE("TEE gatekeeper unavailable — falling back to software gatekeeper");
		gatekeeper = new SoftGateKeeperDevice();
	}

	auto status = gatekeeper->registerAsService("default");
	if (status != android::OK) {
		ALOGE("registerAsService(default) failed: %d", status);
		return 1;
	}
	ALOGI("gatekeeper@1.0 service registered");

	joinRpcThreadpool();
	return 1;  // Should never reach here.
}
