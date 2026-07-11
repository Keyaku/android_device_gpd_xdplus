/*
 * broadcastradio V1_0/V1_1 symbols for the prebuilt 8.1 vendor
 * android.hardware.broadcastradio@1.1-service. O-era hidl-gen exported enum /
 * struct toString and struct operator== from the HIDL libs; R makes them
 * header-only or drops them, so the vendor binary can't link. Types are
 * hand-declared (layout-identical to the O-generated ones) so the mangled
 * names match without redefining R's inline header versions.
 */
#include <cstdint>
#include <string>

#include <hidl/HidlSupport.h>

using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;

namespace android {
namespace hardware {
namespace broadcastradio {

namespace V1_0 {

enum class Band : uint32_t { AM = 0, FM = 1, FM_HD = 2, AM_HD = 3 };
enum class Class : uint32_t { AM_FM = 0, SAT = 1, DT = 2 };
enum class Direction : uint32_t { UP = 0, DOWN = 1 };
enum class Deemphasis : uint32_t { D50 = 1, D75 = 2 };
enum class Rds : uint32_t { NONE = 0, WORLD = 1, US = 2 };

struct FmBandConfig {
	Deemphasis deemphasis;
	bool stereo;
	Rds rds;
	bool ta;
	bool af;
	bool ea;
};

struct AmBandConfig {
	bool stereo;
};

std::string toString(Band o) {
	switch (o) {
	case Band::AM: return "AM";
	case Band::FM: return "FM";
	case Band::FM_HD: return "FM_HD";
	case Band::AM_HD: return "AM_HD";
	}
	return "0x" + std::to_string(static_cast<uint32_t>(o));
}

std::string toString(Class o) {
	switch (o) {
	case Class::AM_FM: return "AM_FM";
	case Class::SAT: return "SAT";
	case Class::DT: return "DT";
	}
	return "0x" + std::to_string(static_cast<uint32_t>(o));
}

std::string toString(Direction o) {
	switch (o) {
	case Direction::UP: return "UP";
	case Direction::DOWN: return "DOWN";
	}
	return "0x" + std::to_string(static_cast<uint32_t>(o));
}

bool operator==(const FmBandConfig& l, const FmBandConfig& r) {
	return l.deemphasis == r.deemphasis && l.stereo == r.stereo &&
		l.rds == r.rds && l.ta == r.ta && l.af == r.af && l.ea == r.ea;
}

bool operator==(const AmBandConfig& l, const AmBandConfig& r) {
	return l.stereo == r.stereo;
}

}  // namespace V1_0

namespace V1_1 {

enum class ProgramType : uint32_t {
	AM = 1, FM = 2, AM_HD = 3, FM_HD = 4, DAB = 5, DRMO = 6, SXM = 7,
	VENDOR_START = 1000, VENDOR_END = 1999,
};

enum class IdentifierType : uint32_t {
	AMFM_FREQUENCY = 1, RDS_PI = 2, HD_STATION_ID_EXT = 3, HD_SUBCHANNEL = 4,
	DAB_SIDECC = 5, DAB_ENSEMBLE = 6, DAB_SCID = 7, DAB_FREQUENCY = 8,
	DRMO_SERVICE_ID = 9, DRMO_FREQUENCY = 10, DRMO_MODULATION = 11,
	SXM_SERVICE_ID = 12, SXM_CHANNEL = 13,
	VENDOR_PRIMARY_START = 1000, VENDOR_PRIMARY_END = 1999,
};

struct VendorKeyValue {
	hidl_string key;
	hidl_string value;
};

struct ProgramIdentifier {
	uint32_t type;
	uint64_t value;
};

struct ProgramSelector {
	uint32_t programType;
	ProgramIdentifier primaryId;
	hidl_vec<ProgramIdentifier> secondaryIds;
	hidl_vec<uint64_t> vendorIds;
};

static std::string toStringType(uint32_t o) {
	switch (static_cast<ProgramType>(o)) {
	case ProgramType::AM: return "AM";
	case ProgramType::FM: return "FM";
	case ProgramType::AM_HD: return "AM_HD";
	case ProgramType::FM_HD: return "FM_HD";
	case ProgramType::DAB: return "DAB";
	case ProgramType::DRMO: return "DRMO";
	case ProgramType::SXM: return "SXM";
	default: return "0x" + std::to_string(o);
	}
}

std::string toString(ProgramType o) { return toStringType(static_cast<uint32_t>(o)); }

std::string toString(IdentifierType o) {
	switch (o) {
	case IdentifierType::AMFM_FREQUENCY: return "AMFM_FREQUENCY";
	case IdentifierType::RDS_PI: return "RDS_PI";
	case IdentifierType::HD_STATION_ID_EXT: return "HD_STATION_ID_EXT";
	case IdentifierType::HD_SUBCHANNEL: return "HD_SUBCHANNEL";
	case IdentifierType::DAB_SIDECC: return "DAB_SIDECC";
	case IdentifierType::DAB_ENSEMBLE: return "DAB_ENSEMBLE";
	case IdentifierType::DAB_SCID: return "DAB_SCID";
	case IdentifierType::DAB_FREQUENCY: return "DAB_FREQUENCY";
	case IdentifierType::DRMO_SERVICE_ID: return "DRMO_SERVICE_ID";
	case IdentifierType::DRMO_FREQUENCY: return "DRMO_FREQUENCY";
	case IdentifierType::DRMO_MODULATION: return "DRMO_MODULATION";
	case IdentifierType::SXM_SERVICE_ID: return "SXM_SERVICE_ID";
	case IdentifierType::SXM_CHANNEL: return "SXM_CHANNEL";
	default: return "0x" + std::to_string(static_cast<uint32_t>(o));
	}
}

std::string toString(const VendorKeyValue& o) {
	std::string os = "{.key = ";
	os += o.key.c_str();
	os += ", .value = ";
	os += o.value.c_str();
	os += "}";
	return os;
}

std::string toString(const ProgramSelector& o) {
	std::string os = "{.programType = ";
	os += toStringType(o.programType);
	os += ", .primaryId = {" + std::to_string(o.primaryId.type) + ", " +
		std::to_string(o.primaryId.value) + "}";
	os += ", .secondaryIds = <" + std::to_string(o.secondaryIds.size()) + ">";
	os += ", .vendorIds = <" + std::to_string(o.vendorIds.size()) + ">}";
	return os;
}

}  // namespace V1_1
}  // namespace broadcastradio
}  // namespace hardware
}  // namespace android

/*
 * R libutils aborts when an sp<> wraps a stack address; the O-era vendor
 * broadcastradio service registers a stack-allocated factory (never freed, so
 * harmless). Neutralize the guard for this binary only (shim is injected just
 * for it via TARGET_LD_SHIM_LIBS).
 */
namespace android {
void sp_report_stack_pointer() {}
}  // namespace android
