/*
 * O-era android::HidlUtils::* trampolines for the prebuilt MTK audio impl
 * (android.hardware.audio@2.0-impl-mediatek.so). R keeps identical functions
 * under android::hardware::audio::common::V2_0::implementation::HidlUtils in
 * android.hardware.audio.common@2.0-util.so — only the mangling differs.
 *
 * Trampolines forward the first four argument registers and the return
 * register verbatim; every target function takes <=4 register args (including
 * the hidden sret pointer for the unique_ptr-returning one) with identical
 * signatures, so the calling convention matches exactly.
 */

#define TRAMPOLINE(OLD, NEW)                                              \
	extern "C" void* NEW(void*, void*, void*, void*);                    \
	extern "C" void* OLD(void* a, void* b, void* c, void* d) {           \
		return NEW(a, b, c, d);                                           \
	}

TRAMPOLINE(_ZN7android9HidlUtils14audioPortToHalERKNS_8hardware5audio6common4V2_09AudioPortEP10audio_port,
           _ZN7android8hardware5audio6common4V2_014implementation9HidlUtils14audioPortToHalERKNS3_9AudioPortEP10audio_port)
// NOTE: audioConfigToHal / audioConfigFromHal intentionally NOT trampolined —
// audio_offload_info_t grew after O, R's converters smash the blob's stack.
// Real O-layout implementations live in shim_audio_config.cpp.
TRAMPOLINE(_ZN7android9HidlUtils16audioPortFromHalERK10audio_portPNS_8hardware5audio6common4V2_09AudioPortE,
           _ZN7android8hardware5audio6common4V2_014implementation9HidlUtils16audioPortFromHalERK10audio_portPNS3_9AudioPortE)
TRAMPOLINE(_ZN7android9HidlUtils20audioPortConfigToHalERKNS_8hardware5audio6common4V2_015AudioPortConfigEP17audio_port_config,
           _ZN7android8hardware5audio6common4V2_014implementation9HidlUtils20audioPortConfigToHalERKNS3_15AudioPortConfigEP17audio_port_config)
TRAMPOLINE(_ZN7android9HidlUtils21audioPortConfigsToHalERKNS_8hardware8hidl_vecINS1_5audio6common4V2_015AudioPortConfigEEE,
           _ZN7android8hardware5audio6common4V2_014implementation9HidlUtils21audioPortConfigsToHalERKNS0_8hidl_vecINS3_15AudioPortConfigEEE)
TRAMPOLINE(_ZN7android9HidlUtils11uuidFromHalERK12audio_uuid_sPNS_8hardware5audio6common4V2_04UuidE,
           _ZN7android8hardware5audio6common4V2_014implementation9HidlUtils11uuidFromHalERK12audio_uuid_sPNS3_4UuidE)
TRAMPOLINE(_ZN7android9HidlUtils9uuidToHalERKNS_8hardware5audio6common4V2_04UuidEP12audio_uuid_s,
           _ZN7android8hardware5audio6common4V2_014implementation9HidlUtils9uuidToHalERKNS3_4UuidEP12audio_uuid_s)
