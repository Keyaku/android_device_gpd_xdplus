#define LOG_TAG "android.hardware.health@2.1-service.xdplus"

#include <memory>
#include <string_view>

#include <android-base/logging.h>
#include <android/hardware/health/2.1/IHealth.h>
#include <health/utils.h>
#include <health2impl/BinderHealth.h>
#include <health2impl/Health.h>

using ::android::sp;
using ::android::hardware::health::InitHealthdConfig;
using ::android::hardware::health::V2_1::IHealth;
using ::android::hardware::health::V2_1::implementation::BinderHealth;
using ::android::hardware::health::V2_1::implementation::Health;

using namespace std::literals;

static constexpr const char* gInstanceName = "default";

// Self-contained, system-side health@2.1 HAL for xdplus.
//
// Why this exists: the frozen ALLDOCUBE 8.1 vendor ships NO health HAL, so in a
// normal boot nothing registers IHealth and BatteryService stalls ~4 s on
// getService before falling back (PORTING_LOG §45, the single largest boot-time
// item). Registering IHealth here cures that wait.
//
// Why it is not the stock service: AOSP's android.hardware.health@2.1-service is
// a thin binder wrapper that dlopen()s a SEPARATE passthrough -impl .so which is
// vendor-only ("not core"). We cannot rebuild /vendor on this legacy
// system-as-root device (§46). So instead of the wrapper+dlopen split, we build
// ONE binary that constructs the default Health impl directly in-process —
// byte-for-byte the same thing the stock passthrough impl.cpp's HIDL_FETCH_IHealth
// does (default healthd_config, the generic /sys/class/power_supply reader in
// libbatterymonitor) — and wraps it in BinderHealth here. No vendor artifact.
// Mirrors the configstore/dumpstate ".xdplus" system-side backup services.
//
// Coexistence: the charger-mode healthd (init.mt8173.rc: `service battery_charger
// /charger`, `class charger`) is left untouched. This service runs `class hal`
// only — normal boot — so the two never run at the same time (mutually exclusive
// boot modes). We deliberately do NOT add `charger` to our class, nor
// `overrides: healthd`, precisely to keep offline charging on the existing path.
int main() {
    auto config = std::make_unique<healthd_config>();
    InitHealthdConfig(config.get());

    // Default config: generic sysfs battery reader, no board hooks (MT8173's
    // power_supply nodes are standard). Customize via healthd_board_init here if
    // a future defect needs it.
    sp<IHealth> impl = new Health(std::move(config));
    sp<BinderHealth> binder = new BinderHealth(gInstanceName, impl);
    return binder->StartLoop();
}
