/*
 * Manual O-era HidlUtils::audioConfigToHal / audioConfigFromHal for the MTK
 * audio impl blob. These CANNOT be trampolined to the R implementations:
 * audio_offload_info_t grew between O and R (encapsulation_mode, content_id,
 * sync_frames), so R's converter writes past the blob's O-sized stack struct
 * ("stack corruption detected" abort in Device::openInputStream).
 *
 * The structs below mirror the O (8.0/8.1) system/audio.h layouts exactly.
 */
#include <stdint.h>
#include <string.h>

#include <android/hardware/audio/common/2.0/types.h>

using ::android::hardware::audio::common::V2_0::AudioConfig;
using ::android::hardware::audio::common::V2_0::AudioOffloadInfo;

namespace {

// O-era audio_offload_info_t (AUDIO_OFFLOAD_INFO_VERSION 0.1 layout).
struct o_audio_offload_info {
	uint16_t version;
	uint16_t size;
	uint32_t sample_rate;
	uint32_t channel_mask;
	uint32_t format;       // audio_format_t
	uint32_t stream_type;  // audio_stream_type_t
	uint32_t bit_rate;
	int64_t duration_us;
	bool has_video;
	bool is_streaming;
	uint32_t bit_width;
	uint32_t offload_buffer_size;
	uint32_t usage;  // audio_usage_t
};

// O-era audio_config_t.
struct o_audio_config {
	uint32_t sample_rate;
	uint32_t channel_mask;
	uint32_t format;
	o_audio_offload_info offload_info;
	size_t frame_count;
};

void offloadToHal(const AudioOffloadInfo& info, o_audio_offload_info* hal) {
	hal->version = 0x0101;  // AUDIO_OFFLOAD_INFO_VERSION_0_1
	hal->size = sizeof(o_audio_offload_info);
	hal->sample_rate = info.sampleRateHz;
	hal->channel_mask = static_cast<uint32_t>(info.channelMask);
	hal->format = static_cast<uint32_t>(info.format);
	hal->stream_type = static_cast<uint32_t>(info.streamType);
	hal->bit_rate = info.bitRatePerSecond;
	hal->duration_us = info.durationMicroseconds;
	hal->has_video = info.hasVideo;
	hal->is_streaming = info.isStreaming;
	hal->bit_width = info.bitWidth;
	hal->offload_buffer_size = info.bufferSize;
	hal->usage = static_cast<uint32_t>(info.usage);
}

void offloadFromHal(const o_audio_offload_info& hal, AudioOffloadInfo* info) {
	info->sampleRateHz = hal.sample_rate;
	info->channelMask = static_cast<decltype(info->channelMask)>(hal.channel_mask);
	info->format = static_cast<decltype(info->format)>(hal.format);
	info->streamType = static_cast<decltype(info->streamType)>(hal.stream_type);
	info->bitRatePerSecond = hal.bit_rate;
	info->durationMicroseconds = hal.duration_us;
	info->hasVideo = hal.has_video;
	info->isStreaming = hal.is_streaming;
	info->bitWidth = hal.bit_width;
	info->bufferSize = hal.offload_buffer_size;
	info->usage = static_cast<decltype(info->usage)>(hal.usage);
}

}  // namespace

// void android::HidlUtils::audioConfigToHal(const AudioConfig&, audio_config_t*)
extern "C" void
_ZN7android9HidlUtils16audioConfigToHalERKNS_8hardware5audio6common4V2_011AudioConfigEP12audio_config(
		const AudioConfig* config, o_audio_config* halConfig) {
	memset(halConfig, 0, sizeof(*halConfig));
	halConfig->sample_rate = config->sampleRateHz;
	halConfig->channel_mask = static_cast<uint32_t>(config->channelMask);
	halConfig->format = static_cast<uint32_t>(config->format);
	offloadToHal(config->offloadInfo, &halConfig->offload_info);
	halConfig->frame_count = config->frameCount;
}

// void android::HidlUtils::audioConfigFromHal(const audio_config_t&, AudioConfig*)
extern "C" void
_ZN7android9HidlUtils18audioConfigFromHalERK12audio_configPNS_8hardware5audio6common4V2_011AudioConfigE(
		const o_audio_config* halConfig, AudioConfig* config) {
	config->sampleRateHz = halConfig->sample_rate;
	config->channelMask = static_cast<decltype(config->channelMask)>(halConfig->channel_mask);
	config->format = static_cast<decltype(config->format)>(halConfig->format);
	offloadFromHal(halConfig->offload_info, &config->offloadInfo);
	config->frameCount = halConfig->frame_count;
}
