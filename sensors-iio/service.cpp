/*
 * Sensors HAL 2.1 for the XD+ on the mainline kernel. The AOSP default HAL's
 * machinery with its fake sensors replaced by one accelerometer: the msa311
 * IIO device, polled through sysfs.
 *
 * vendor.xd612.accel.map remaps axes, e.g. "-y,x,z" (default "x,y,z").
 */
#define LOG_TAG "xdplus-iio-sensors"

#include <SensorsV2_1.h>

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <dirent.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include <cmath>
#include <string>

using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;
using android::hardware::sensors::V1_0::SensorFlagBits;
using android::hardware::sensors::V2_1::SensorType;
using android::hardware::sensors::V2_1::ISensors;
using android::hardware::sensors::V2_1::implementation::SensorsV2_1;
using android::hardware::sensors::V2_X::implementation::ISensorsEventCallback;
using android::hardware::sensors::V2_X::implementation::Sensor;
using android::hardware::sensors::V1_0::EventPayload;

namespace {

constexpr float kGravity = 9.80665f;

std::string findIio(const std::string& prefix) {
    const std::string base = "/sys/bus/iio/devices/";
    std::unique_ptr<DIR, int (*)(DIR*)> dir(opendir(base.c_str()), closedir);
    if (!dir) return "";
    while (dirent* e = readdir(dir.get())) {
        std::string path = base + e->d_name;
        std::string name;
        if (android::base::ReadFileToString(path + "/name", &name) &&
            android::base::StartsWith(android::base::Trim(name), prefix))
            return path;
    }
    return "";
}

bool readNum(const std::string& path, double* out) {
    std::string s;
    if (!android::base::ReadFileToString(path, &s)) return false;
    char* end = nullptr;
    *out = strtod(s.c_str(), &end);
    return end != s.c_str();
}

class IioAccelSensor : public Sensor {
  public:
    IioAccelSensor(int32_t handle, ISensorsEventCallback* callback) : Sensor(callback) {
        mSensorInfo.sensorHandle = handle;
        mSensorInfo.name = "MSA300 Accelerometer";
        mSensorInfo.vendor = "MEMSensing";
        mSensorInfo.version = 1;
        mSensorInfo.type = SensorType::ACCELEROMETER;
        mSensorInfo.typeAsString = "";
        mSensorInfo.maxRange = 4 * kGravity;
        mSensorInfo.resolution = kGravity / 1024;
        mSensorInfo.power = 0.1f;
        mSensorInfo.minDelay = 10 * 1000;
        mSensorInfo.maxDelay = 1000 * 1000;
        mSensorInfo.fifoReservedEventCount = 0;
        mSensorInfo.fifoMaxEventCount = 0;
        mSensorInfo.requiredPermission = "";
        mSensorInfo.flags = static_cast<uint32_t>(SensorFlagBits::CONTINUOUS_MODE);

        mDev = findIio("msa311");
        if (mDev.empty()) ALOGE("no msa311 IIO device");
        parseMap(android::base::GetProperty("vendor.xd612.accel.map", "x,y,z"));
    }

  protected:
    void readEventPayload(EventPayload& payload) override {
        double raw[3] = {0, 0, 0}, scale = 0;
        if (!mDev.empty()) {
            readNum(mDev + "/in_accel_scale", &scale);
            readNum(mDev + "/in_accel_x_raw", &raw[0]);
            readNum(mDev + "/in_accel_y_raw", &raw[1]);
            readNum(mDev + "/in_accel_z_raw", &raw[2]);
        }
        float v[3];
        for (int i = 0; i < 3; i++) v[i] = mSign[i] * raw[mAxis[i]] * scale;
        payload.vec3.x = v[0];
        payload.vec3.y = v[1];
        payload.vec3.z = v[2];
        payload.vec3.status = android::hardware::sensors::V1_0::SensorStatus::ACCURACY_HIGH;
    }

  private:
    void parseMap(const std::string& map) {
        auto parts = android::base::Split(map, ",");
        for (int i = 0; i < 3; i++) {
            mAxis[i] = i;
            mSign[i] = 1;
            if (parts.size() != 3) continue;
            std::string p = android::base::Trim(parts[i]);
            if (!p.empty() && p[0] == '-') {
                mSign[i] = -1;
                p = p.substr(1);
            }
            if (p == "x" || p == "y" || p == "z") mAxis[i] = p[0] - 'x';
        }
    }

    std::string mDev;
    int mAxis[3];
    int mSign[3];
};

struct XdplusSensors : public SensorsV2_1 {
    XdplusSensors() {
        // Parked, never destroyed: ~Sensor() deadlocks (it releases the lock
        // without unlocking, so the run thread can never see mStopThread).
        new decltype(mSensors)(std::move(mSensors));
        mSensors.clear();
        AddSensor<IioAccelSensor>();
    }
};

}  // namespace

int main(int /* argc */, char** /* argv */) {
    configureRpcThreadpool(1, true);
    android::sp<ISensors> sensors = new XdplusSensors();
    if (sensors->registerAsService() != android::OK) {
        ALOGE("Failed to register Sensors HAL instance");
        return -1;
    }
    joinRpcThreadpool();
    return 1;
}
