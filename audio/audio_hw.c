/*
 * Legacy primary audio HAL for the GPD XD+ (MT8176) on a mainline kernel.
 *
 * tinyalsa-backed, primary PCM in/out only. No A2DP, USB, HDMI, offload,
 * compressed audio, voice call or effects.
 *
 * Licensed under the Apache License, Version 2.0.
 */

#define LOG_TAG "audio_hw_xdplus"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>
#include <system/audio.h>

#include <tinyalsa/asoundlib.h>

#define XD_CARD			0
#define XD_PCM_DEVICE_OUT	0
#define XD_PCM_DEVICE_IN	1

#define OUT_SAMPLE_RATE		48000
#define OUT_CHANNEL_COUNT	2
#define OUT_PERIOD_SIZE		960
#define OUT_PERIOD_COUNT	4

#define IN_PERIOD_SIZE		960
#define IN_PERIOD_COUNT	4

/* The uplink rate selector in the codec has no other settings. */
static const uint32_t kInputRates[] = { 8000, 16000, 32000, 48000 };

/*
 * Headset_PGA[LR]_GAIN enum, in hardware order. Index 0 is loudest.
 * RES1/RES2 are reserved and must never be selected.
 */
#define GAIN_ENUM_COUNT		16
#define GAIN_ENUM_MUTE		15
/*
 * AudioFlinger scales the primary output itself and never calls
 * set_volume, so whatever sits here is the analog gain for every sound
 * the device makes. Index 0 is +8 dB, the loudest the PGA offers; the
 * vendor stack runs this speaker at -4 dB.
 */
#define GAIN_ENUM_DEFAULT	12
static const int kGainIndexByStep[] = { 15, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
#define GAIN_STEP_COUNT	((int)(sizeof(kGainIndexByStep) / sizeof(kGainIndexByStep[0])))

struct xd_audio_device {
	struct audio_hw_device device;
	pthread_mutex_t lock;
	struct mixer *mixer;
	int out_route_refs;
	int in_route_refs;
	bool mic_mute;
	audio_mode_t mode;
	int gain_index;
};

struct xd_stream_out {
	struct audio_stream_out stream;
	pthread_mutex_t lock;
	struct xd_audio_device *adev;
	struct pcm *pcm;
	struct pcm_config config;
	bool standby;
	bool routed;
	audio_channel_mask_t channel_mask;
	audio_devices_t devices;
	uint64_t frames_written;
};

struct xd_stream_in {
	struct audio_stream_in stream;
	pthread_mutex_t lock;
	struct xd_audio_device *adev;
	struct pcm *pcm;
	struct pcm_config config;
	bool standby;
	bool routed;
	audio_channel_mask_t channel_mask;
	audio_devices_t devices;
};

/* ---------------------------------------------------------------- mixer */

static struct mixer *adev_mixer_locked(struct xd_audio_device *adev)
{
	if (!adev->mixer) {
		adev->mixer = mixer_open(XD_CARD);
		if (!adev->mixer)
			ALOGE("mixer_open(card %d) failed: %s", XD_CARD, strerror(errno));
	}
	return adev->mixer;
}

static int mixer_set_int(struct mixer *mixer, const char *name, int value)
{
	struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, name);
	unsigned int n, i;
	int ret;

	if (!ctl) {
		ALOGE("mixer control '%s' not found", name);
		return -ENOENT;
	}
	n = mixer_ctl_get_num_values(ctl);
	for (i = 0; i < n; i++) {
		ret = mixer_ctl_set_value(ctl, i, value);
		if (ret != 0) {
			ALOGE("mixer '%s'[%u] = %d failed: %d (%s)", name, i, value,
			      ret, strerror(errno));
			return ret;
		}
	}
	return 0;
}

static int mixer_set_str(struct mixer *mixer, const char *name, const char *value)
{
	struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, name);
	int ret;

	if (!ctl) {
		ALOGE("mixer control '%s' not found", name);
		return -ENOENT;
	}
	ret = mixer_ctl_set_enum_by_string(ctl, value);
	if (ret != 0)
		ALOGE("mixer '%s' = '%s' failed: %d (%s)", name, value, ret,
		      strerror(errno));
	return ret;
}

static int mixer_set_enum_index(struct mixer *mixer, const char *name, int index)
{
	struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, name);
	const char *s;
	int ret;

	if (!ctl) {
		ALOGE("mixer control '%s' not found", name);
		return -ENOENT;
	}
	if (index < 0 || (unsigned int)index >= mixer_ctl_get_num_enums(ctl)) {
		ALOGE("mixer '%s' enum index %d out of range", name, index);
		return -EINVAL;
	}
	s = mixer_ctl_get_enum_string(ctl, (unsigned int)index);
	if (!s) {
		ALOGE("mixer '%s' enum index %d has no string", name, index);
		return -EINVAL;
	}
	ret = mixer_ctl_set_enum_by_string(ctl, s);
	if (ret != 0)
		ALOGE("mixer '%s' = '%s' failed: %d (%s)", name, s, ret, strerror(errno));
	return ret;
}

/* Playback DAPM path. pcm_write errors out immediately if this is unset. */
static int adev_route_out_enable(struct xd_audio_device *adev)
{
	struct mixer *mixer;
	int ret = 0;

	mixer = adev_mixer_locked(adev);
	if (!mixer)
		return -ENODEV;

	if (adev->out_route_refs++ > 0)
		return 0;

	ret |= mixer_set_int(mixer, "O03 I05 Switch", 1);
	ret |= mixer_set_int(mixer, "O04 I06 Switch", 1);
	ret |= mixer_set_str(mixer, "Speaker_Amp_Switch", "On");
	ret |= mixer_set_enum_index(mixer, "Headset_PGAL_GAIN", adev->gain_index);
	ret |= mixer_set_enum_index(mixer, "Headset_PGAR_GAIN", adev->gain_index);

	if (ret != 0)
		ALOGE("playback routing incomplete; audio may be silent");
	else
		ALOGI("playback routing enabled (gain index %d)", adev->gain_index);
	return 0;
}

static void adev_route_out_disable(struct xd_audio_device *adev)
{
	struct mixer *mixer;

	if (adev->out_route_refs <= 0)
		return;
	if (--adev->out_route_refs > 0)
		return;

	mixer = adev_mixer_locked(adev);
	if (!mixer)
		return;

	/* Leaving the amp on keeps the external power amplifier enabled. */
	mixer_set_str(mixer, "Speaker_Amp_Switch", "Off");
	mixer_set_int(mixer, "O03 I05 Switch", 0);
	mixer_set_int(mixer, "O04 I06 Switch", 0);
	ALOGI("playback routing disabled");
}

static int adev_route_in_enable(struct xd_audio_device *adev)
{
	struct mixer *mixer;
	int ret = 0;

	mixer = adev_mixer_locked(adev);
	if (!mixer)
		return -ENODEV;

	if (adev->in_route_refs++ > 0)
		return 0;

	ret |= mixer_set_int(mixer, "O09 I03 Switch", 1);
	ret |= mixer_set_int(mixer, "O10 I04 Switch", 1);
	ret |= mixer_set_str(mixer, "Audio_ADC_1_Switch", "On");
	ret |= mixer_set_str(mixer, "Audio_Preamp1_Switch", "AIN1");

	if (ret != 0)
		ALOGE("capture routing incomplete; capture may be silent");
	else
		ALOGI("capture routing enabled");
	return 0;
}

static void adev_route_in_disable(struct xd_audio_device *adev)
{
	struct mixer *mixer;

	if (adev->in_route_refs <= 0)
		return;
	if (--adev->in_route_refs > 0)
		return;

	mixer = adev_mixer_locked(adev);
	if (!mixer)
		return;

	mixer_set_str(mixer, "Audio_Preamp1_Switch", "OPEN");
	mixer_set_str(mixer, "Audio_ADC_1_Switch", "Off");
	mixer_set_int(mixer, "O09 I03 Switch", 0);
	mixer_set_int(mixer, "O10 I04 Switch", 0);
	ALOGI("capture routing disabled");
}

/* ------------------------------------------------------------- helpers */

static bool rate_is_supported_in(uint32_t rate)
{
	size_t i;

	for (i = 0; i < sizeof(kInputRates) / sizeof(kInputRates[0]); i++)
		if (kInputRates[i] == rate)
			return true;
	return false;
}

static int64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* ------------------------------------------------- audio_stream_out ops */

static int out_close_pcm_locked(struct xd_stream_out *out)
{
	if (out->pcm) {
		pcm_close(out->pcm);
		out->pcm = NULL;
	}
	if (out->routed) {
		pthread_mutex_lock(&out->adev->lock);
		adev_route_out_disable(out->adev);
		pthread_mutex_unlock(&out->adev->lock);
		out->routed = false;
	}
	out->standby = true;
	return 0;
}

static int out_open_pcm_locked(struct xd_stream_out *out)
{
	struct pcm *pcm;

	if (out->pcm)
		return 0;

	pthread_mutex_lock(&out->adev->lock);
	adev_route_out_enable(out->adev);
	pthread_mutex_unlock(&out->adev->lock);
	out->routed = true;

	pcm = pcm_open(XD_CARD, XD_PCM_DEVICE_OUT, PCM_OUT | PCM_MONOTONIC, &out->config);
	if (!pcm || !pcm_is_ready(pcm)) {
		ALOGE("pcm_open(card %d, dev %d, out) failed: %s", XD_CARD,
		      XD_PCM_DEVICE_OUT, pcm ? pcm_get_error(pcm) : "no handle");
		if (pcm)
			pcm_close(pcm);
		pthread_mutex_lock(&out->adev->lock);
		adev_route_out_disable(out->adev);
		pthread_mutex_unlock(&out->adev->lock);
		out->routed = false;
		return -ENODEV;
	}
	out->pcm = pcm;
	out->standby = false;
	ALOGI("output PCM open: %u Hz, %u ch, period %u x %u", out->config.rate,
	      out->config.channels, out->config.period_size, out->config.period_count);
	return 0;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
	const struct xd_stream_out *out = (const struct xd_stream_out *)stream;

	return out->config.rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
	const struct xd_stream_out *out = (const struct xd_stream_out *)stream;

	/* The rate is fixed once the stream is open; AudioFlinger resamples. */
	return (rate == out->config.rate) ? 0 : -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
	const struct xd_stream_out *out = (const struct xd_stream_out *)stream;

	return out->config.period_size * audio_stream_out_frame_size(&out->stream);
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
	const struct xd_stream_out *out = (const struct xd_stream_out *)stream;

	return out->channel_mask;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
	return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
	/* The codec only does S16_LE. */
	return (format == AUDIO_FORMAT_PCM_16_BIT) ? 0 : -ENOSYS;
}

static int out_standby(struct audio_stream *stream)
{
	struct xd_stream_out *out = (struct xd_stream_out *)stream;

	pthread_mutex_lock(&out->lock);
	if (!out->standby)
		out_close_pcm_locked(out);
	pthread_mutex_unlock(&out->lock);
	return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
	const struct xd_stream_out *out = (const struct xd_stream_out *)stream;

	dprintf(fd, "  out: rate %u ch %u period %u x %u standby %d routed %d frames %llu\n",
		out->config.rate, out->config.channels, out->config.period_size,
		out->config.period_count, out->standby, out->routed,
		(unsigned long long)out->frames_written);
	return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
	struct xd_stream_out *out = (struct xd_stream_out *)stream;
	const char *p = strstr(kvpairs ? kvpairs : "", AUDIO_PARAMETER_STREAM_ROUTING "=");

	/* Only routing is honoured; there is one output path on this board. */
	if (p) {
		int devices = atoi(p + strlen(AUDIO_PARAMETER_STREAM_ROUTING "="));
		pthread_mutex_lock(&out->lock);
		out->devices = (audio_devices_t)devices;
		pthread_mutex_unlock(&out->lock);
	}
	return 0;
}

static char *out_get_parameters(const struct audio_stream *stream, const char *keys)
{
	const struct xd_stream_out *out = (const struct xd_stream_out *)stream;
	char buf[128];

	if (keys && strstr(keys, AUDIO_PARAMETER_STREAM_ROUTING)) {
		snprintf(buf, sizeof(buf), "%s=%d", AUDIO_PARAMETER_STREAM_ROUTING,
			 (int)out->devices);
		return strdup(buf);
	}
	return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
	const struct xd_stream_out *out = (const struct xd_stream_out *)stream;

	return (out->config.period_size * out->config.period_count * 1000) /
	       out->config.rate;
}

static int out_set_volume(struct audio_stream_out *stream, float left, float right)
{
	struct xd_stream_out *out = (struct xd_stream_out *)stream;
	struct xd_audio_device *adev = out->adev;
	float v = (left + right) / 2.0f;
	int step, index;

	if (v < 0.0f)
		v = 0.0f;
	if (v > 1.0f)
		v = 1.0f;

	/* The codec has no continuous volume, only a 16-entry PGA enum. */
	step = (int)(v * (GAIN_STEP_COUNT - 1) + 0.5f);
	index = (v == 0.0f) ? GAIN_ENUM_MUTE : kGainIndexByStep[step];

	pthread_mutex_lock(&adev->lock);
	adev->gain_index = index;
	if (adev->out_route_refs > 0 && adev_mixer_locked(adev)) {
		mixer_set_enum_index(adev->mixer, "Headset_PGAL_GAIN", index);
		mixer_set_enum_index(adev->mixer, "Headset_PGAR_GAIN", index);
	}
	pthread_mutex_unlock(&adev->lock);
	return 0;
}

static ssize_t out_write(struct audio_stream_out *stream, const void *buffer,
			 size_t bytes)
{
	struct xd_stream_out *out = (struct xd_stream_out *)stream;
	int ret;

	pthread_mutex_lock(&out->lock);

	if (!out->pcm) {
		ret = out_open_pcm_locked(out);
		if (ret != 0) {
			pthread_mutex_unlock(&out->lock);
			/* Pace the caller so a dead PCM does not spin AudioFlinger. */
			usleep(bytes * 1000000LL /
			       audio_stream_out_frame_size(stream) / out->config.rate);
			return bytes;
		}
	}

	ret = pcm_write(out->pcm, buffer, bytes);
	if (ret != 0) {
		ALOGE("pcm_write(%zu bytes) failed: %d (%s)", bytes, ret,
		      pcm_get_error(out->pcm));
		out_close_pcm_locked(out);
		pthread_mutex_unlock(&out->lock);
		usleep(bytes * 1000000LL /
		       audio_stream_out_frame_size(stream) / out->config.rate);
		return bytes;
	}

	out->frames_written += bytes / audio_stream_out_frame_size(stream);
	pthread_mutex_unlock(&out->lock);
	return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
				   uint32_t *dsp_frames)
{
	struct xd_stream_out *out = (struct xd_stream_out *)stream;

	if (!dsp_frames)
		return -EINVAL;
	pthread_mutex_lock(&out->lock);
	*dsp_frames = (uint32_t)out->frames_written;
	pthread_mutex_unlock(&out->lock);
	return 0;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
					 uint64_t *frames, struct timespec *timestamp)
{
	struct xd_stream_out *out = (struct xd_stream_out *)stream;
	unsigned int avail = 0;
	int ret = -ENODATA;

	if (!frames || !timestamp)
		return -EINVAL;

	pthread_mutex_lock(&out->lock);
	if (out->pcm && pcm_get_htimestamp(out->pcm, &avail, timestamp) == 0) {
		size_t kernel_buffer = out->config.period_size * out->config.period_count;
		uint64_t queued = (avail < kernel_buffer) ? (kernel_buffer - avail) : 0;

		*frames = (out->frames_written > queued) ? out->frames_written - queued : 0;
		ret = 0;
	} else if (out->standby) {
		int64_t ns = now_ns();

		*frames = out->frames_written;
		timestamp->tv_sec = ns / 1000000000LL;
		timestamp->tv_nsec = ns % 1000000000LL;
		ret = 0;
	}
	pthread_mutex_unlock(&out->lock);
	return ret;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
	return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
	return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
					int64_t *timestamp)
{
	return -EINVAL;
}

/* -------------------------------------------------- audio_stream_in ops */

static int in_close_pcm_locked(struct xd_stream_in *in)
{
	if (in->pcm) {
		pcm_close(in->pcm);
		in->pcm = NULL;
	}
	if (in->routed) {
		pthread_mutex_lock(&in->adev->lock);
		adev_route_in_disable(in->adev);
		pthread_mutex_unlock(&in->adev->lock);
		in->routed = false;
	}
	in->standby = true;
	return 0;
}

static int in_open_pcm_locked(struct xd_stream_in *in)
{
	struct pcm *pcm;

	if (in->pcm)
		return 0;

	pthread_mutex_lock(&in->adev->lock);
	adev_route_in_enable(in->adev);
	pthread_mutex_unlock(&in->adev->lock);
	in->routed = true;

	pcm = pcm_open(XD_CARD, XD_PCM_DEVICE_IN, PCM_IN | PCM_MONOTONIC, &in->config);
	if (!pcm || !pcm_is_ready(pcm)) {
		ALOGE("pcm_open(card %d, dev %d, in) failed: %s", XD_CARD,
		      XD_PCM_DEVICE_IN, pcm ? pcm_get_error(pcm) : "no handle");
		if (pcm)
			pcm_close(pcm);
		pthread_mutex_lock(&in->adev->lock);
		adev_route_in_disable(in->adev);
		pthread_mutex_unlock(&in->adev->lock);
		in->routed = false;
		return -ENODEV;
	}
	in->pcm = pcm;
	in->standby = false;
	ALOGI("input PCM open: %u Hz, %u ch", in->config.rate, in->config.channels);
	return 0;
}

static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
	const struct xd_stream_in *in = (const struct xd_stream_in *)stream;

	return in->config.rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
	const struct xd_stream_in *in = (const struct xd_stream_in *)stream;

	return (rate == in->config.rate) ? 0 : -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
	const struct xd_stream_in *in = (const struct xd_stream_in *)stream;

	return in->config.period_size * audio_stream_in_frame_size(&in->stream);
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
	const struct xd_stream_in *in = (const struct xd_stream_in *)stream;

	return in->channel_mask;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
	return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
	return (format == AUDIO_FORMAT_PCM_16_BIT) ? 0 : -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
	struct xd_stream_in *in = (struct xd_stream_in *)stream;

	pthread_mutex_lock(&in->lock);
	if (!in->standby)
		in_close_pcm_locked(in);
	pthread_mutex_unlock(&in->lock);
	return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
	const struct xd_stream_in *in = (const struct xd_stream_in *)stream;

	dprintf(fd, "  in: rate %u ch %u period %u x %u standby %d routed %d\n",
		in->config.rate, in->config.channels, in->config.period_size,
		in->config.period_count, in->standby, in->routed);
	return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
	struct xd_stream_in *in = (struct xd_stream_in *)stream;
	const char *p = strstr(kvpairs ? kvpairs : "", AUDIO_PARAMETER_STREAM_ROUTING "=");

	if (p) {
		int devices = atoi(p + strlen(AUDIO_PARAMETER_STREAM_ROUTING "="));
		pthread_mutex_lock(&in->lock);
		in->devices = (audio_devices_t)devices;
		pthread_mutex_unlock(&in->lock);
	}
	return 0;
}

static char *in_get_parameters(const struct audio_stream *stream, const char *keys)
{
	const struct xd_stream_in *in = (const struct xd_stream_in *)stream;
	char buf[128];

	if (keys && strstr(keys, AUDIO_PARAMETER_STREAM_ROUTING)) {
		snprintf(buf, sizeof(buf), "%s=%d", AUDIO_PARAMETER_STREAM_ROUTING,
			 (int)in->devices);
		return strdup(buf);
	}
	return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
	return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void *buffer, size_t bytes)
{
	struct xd_stream_in *in = (struct xd_stream_in *)stream;
	bool mute;
	int ret;

	pthread_mutex_lock(&in->lock);

	if (!in->pcm) {
		ret = in_open_pcm_locked(in);
		if (ret != 0) {
			pthread_mutex_unlock(&in->lock);
			memset(buffer, 0, bytes);
			usleep(bytes * 1000000LL /
			       audio_stream_in_frame_size(stream) / in->config.rate);
			return bytes;
		}
	}

	ret = pcm_read(in->pcm, buffer, bytes);
	if (ret != 0) {
		ALOGE("pcm_read(%zu bytes) failed: %d (%s)", bytes, ret,
		      pcm_get_error(in->pcm));
		in_close_pcm_locked(in);
		pthread_mutex_unlock(&in->lock);
		memset(buffer, 0, bytes);
		usleep(bytes * 1000000LL /
		       audio_stream_in_frame_size(stream) / in->config.rate);
		return bytes;
	}
	pthread_mutex_unlock(&in->lock);

	pthread_mutex_lock(&in->adev->lock);
	mute = in->adev->mic_mute;
	pthread_mutex_unlock(&in->adev->lock);
	if (mute)
		memset(buffer, 0, bytes);

	return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
	return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
	return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
	return 0;
}

/* ------------------------------------------------- audio_hw_device ops */

static int adev_open_output_stream(struct audio_hw_device *dev,
				   audio_io_handle_t handle,
				   audio_devices_t devices,
				   audio_output_flags_t flags,
				   struct audio_config *config,
				   struct audio_stream_out **stream_out,
				   const char *address)
{
	struct xd_audio_device *adev = (struct xd_audio_device *)dev;
	struct xd_stream_out *out;

	*stream_out = NULL;

	out = (struct xd_stream_out *)calloc(1, sizeof(struct xd_stream_out));
	if (!out)
		return -ENOMEM;

	out->stream.common.get_sample_rate = out_get_sample_rate;
	out->stream.common.set_sample_rate = out_set_sample_rate;
	out->stream.common.get_buffer_size = out_get_buffer_size;
	out->stream.common.get_channels = out_get_channels;
	out->stream.common.get_format = out_get_format;
	out->stream.common.set_format = out_set_format;
	out->stream.common.standby = out_standby;
	out->stream.common.dump = out_dump;
	out->stream.common.set_parameters = out_set_parameters;
	out->stream.common.get_parameters = out_get_parameters;
	out->stream.common.add_audio_effect = out_add_audio_effect;
	out->stream.common.remove_audio_effect = out_remove_audio_effect;
	out->stream.get_latency = out_get_latency;
	out->stream.set_volume = out_set_volume;
	out->stream.write = out_write;
	out->stream.get_render_position = out_get_render_position;
	out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
	out->stream.get_presentation_position = out_get_presentation_position;

	pthread_mutex_init(&out->lock, NULL);
	out->adev = adev;
	out->standby = true;
	out->devices = devices;

	out->config.format = PCM_FORMAT_S16_LE;
	out->config.rate = OUT_SAMPLE_RATE;
	out->config.channels = OUT_CHANNEL_COUNT;
	out->config.period_size = OUT_PERIOD_SIZE;
	out->config.period_count = OUT_PERIOD_COUNT;
	out->channel_mask = AUDIO_CHANNEL_OUT_STEREO;

	/* The single hardware path is 48 kHz stereo S16; AudioFlinger adapts. */
	config->sample_rate = out->config.rate;
	config->channel_mask = out->channel_mask;
	config->format = AUDIO_FORMAT_PCM_16_BIT;

	ALOGI("open_output_stream: 48000 Hz stereo S16_LE, devices 0x%x, flags 0x%x",
	      devices, flags);
	*stream_out = &out->stream;
	return 0;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
				     struct audio_stream_out *stream)
{
	struct xd_stream_out *out = (struct xd_stream_out *)stream;

	pthread_mutex_lock(&out->lock);
	out_close_pcm_locked(out);
	pthread_mutex_unlock(&out->lock);
	pthread_mutex_destroy(&out->lock);
	free(out);
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
					 const struct audio_config *config)
{
	size_t channels = audio_channel_count_from_in_mask(config->channel_mask);
	uint32_t rate = config->sample_rate ? config->sample_rate : 48000;
	size_t frames;

	if (channels < 1)
		channels = 1;
	if (channels > 2)
		channels = 2;
	frames = (size_t)IN_PERIOD_SIZE * rate / 48000;
	if (frames == 0)
		frames = 1;
	return frames * channels * sizeof(int16_t);
}

static int adev_open_input_stream(struct audio_hw_device *dev,
				  audio_io_handle_t handle,
				  audio_devices_t devices,
				  struct audio_config *config,
				  struct audio_stream_in **stream_in,
				  audio_input_flags_t flags,
				  const char *address,
				  audio_source_t source)
{
	struct xd_audio_device *adev = (struct xd_audio_device *)dev;
	struct xd_stream_in *in;
	unsigned int channels;

	*stream_in = NULL;

	if (config->format != AUDIO_FORMAT_DEFAULT &&
	    config->format != AUDIO_FORMAT_PCM_16_BIT) {
		ALOGE("open_input_stream: unsupported format 0x%x, suggesting S16",
		      config->format);
		config->format = AUDIO_FORMAT_PCM_16_BIT;
		return -EINVAL;
	}

	/* The uplink rate selector has only these four settings. */
	if (!rate_is_supported_in(config->sample_rate)) {
		ALOGE("open_input_stream: rate %u unsupported, suggesting 48000",
		      config->sample_rate);
		config->sample_rate = 48000;
		return -EINVAL;
	}

	channels = audio_channel_count_from_in_mask(config->channel_mask);
	if (channels < 1 || channels > 2) {
		ALOGE("open_input_stream: %u channels unsupported, suggesting stereo",
		      channels);
		config->channel_mask = AUDIO_CHANNEL_IN_STEREO;
		return -EINVAL;
	}

	in = (struct xd_stream_in *)calloc(1, sizeof(struct xd_stream_in));
	if (!in)
		return -ENOMEM;

	in->stream.common.get_sample_rate = in_get_sample_rate;
	in->stream.common.set_sample_rate = in_set_sample_rate;
	in->stream.common.get_buffer_size = in_get_buffer_size;
	in->stream.common.get_channels = in_get_channels;
	in->stream.common.get_format = in_get_format;
	in->stream.common.set_format = in_set_format;
	in->stream.common.standby = in_standby;
	in->stream.common.dump = in_dump;
	in->stream.common.set_parameters = in_set_parameters;
	in->stream.common.get_parameters = in_get_parameters;
	in->stream.common.add_audio_effect = in_add_audio_effect;
	in->stream.common.remove_audio_effect = in_remove_audio_effect;
	in->stream.set_gain = in_set_gain;
	in->stream.read = in_read;
	in->stream.get_input_frames_lost = in_get_input_frames_lost;

	pthread_mutex_init(&in->lock, NULL);
	in->adev = adev;
	in->standby = true;
	in->devices = devices;
	in->channel_mask = config->channel_mask;

	in->config.format = PCM_FORMAT_S16_LE;
	in->config.rate = config->sample_rate;
	in->config.channels = channels;
	in->config.period_size = (unsigned int)IN_PERIOD_SIZE * config->sample_rate / 48000;
	in->config.period_count = IN_PERIOD_COUNT;

	ALOGI("open_input_stream: %u Hz, %u ch, devices 0x%x, source %d",
	      in->config.rate, channels, devices, source);
	*stream_in = &in->stream;
	return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
				    struct audio_stream_in *stream)
{
	struct xd_stream_in *in = (struct xd_stream_in *)stream;

	pthread_mutex_lock(&in->lock);
	in_close_pcm_locked(in);
	pthread_mutex_unlock(&in->lock);
	pthread_mutex_destroy(&in->lock);
	free(in);
}

static int adev_init_check(const struct audio_hw_device *dev)
{
	struct xd_audio_device *adev = (struct xd_audio_device *)dev;
	int ret = 0;

	pthread_mutex_lock(&adev->lock);
	if (!adev_mixer_locked(adev))
		ret = -ENODEV;
	pthread_mutex_unlock(&adev->lock);
	if (ret != 0)
		ALOGE("init_check: no mixer on card %d", XD_CARD);
	return ret;
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
	ALOGI("set_parameters: %s", kvpairs ? kvpairs : "(null)");
	return 0;
}

static char *adev_get_parameters(const struct audio_hw_device *dev, const char *keys)
{
	return strdup("");
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
	return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
	return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev, float *volume)
{
	return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev, bool muted)
{
	return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev, bool *muted)
{
	return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
	struct xd_audio_device *adev = (struct xd_audio_device *)dev;

	pthread_mutex_lock(&adev->lock);
	adev->mode = mode;
	pthread_mutex_unlock(&adev->lock);
	return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
	struct xd_audio_device *adev = (struct xd_audio_device *)dev;

	pthread_mutex_lock(&adev->lock);
	adev->mic_mute = state;
	pthread_mutex_unlock(&adev->lock);
	return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
	struct xd_audio_device *adev = (struct xd_audio_device *)dev;

	if (!state)
		return -EINVAL;
	pthread_mutex_lock(&adev->lock);
	*state = adev->mic_mute;
	pthread_mutex_unlock(&adev->lock);
	return 0;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
	struct xd_audio_device *adev = (struct xd_audio_device *)device;

	pthread_mutex_lock(&adev->lock);
	dprintf(fd, "xdplus audio HAL: card %d, mixer %s, out refs %d, in refs %d,"
		" mic_mute %d, mode %d, gain index %d\n",
		XD_CARD, adev->mixer ? "open" : "closed", adev->out_route_refs,
		adev->in_route_refs, adev->mic_mute, adev->mode, adev->gain_index);
	pthread_mutex_unlock(&adev->lock);
	return 0;
}

static int adev_close(hw_device_t *device)
{
	struct xd_audio_device *adev = (struct xd_audio_device *)device;

	if (!adev)
		return 0;
	pthread_mutex_lock(&adev->lock);
	if (adev->mixer) {
		mixer_close(adev->mixer);
		adev->mixer = NULL;
	}
	pthread_mutex_unlock(&adev->lock);
	pthread_mutex_destroy(&adev->lock);
	free(adev);
	return 0;
}

static int adev_open(const hw_module_t *module, const char *name, hw_device_t **device)
{
	struct xd_audio_device *adev;

	if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) {
		ALOGE("adev_open: unknown interface '%s'", name);
		return -EINVAL;
	}

	adev = calloc(1, sizeof(struct xd_audio_device));
	if (!adev)
		return -ENOMEM;

	pthread_mutex_init(&adev->lock, NULL);
	adev->gain_index = GAIN_ENUM_DEFAULT;
	adev->mode = AUDIO_MODE_NORMAL;

	adev->device.common.tag = HARDWARE_DEVICE_TAG;
	adev->device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
	adev->device.common.module = (struct hw_module_t *)module;
	adev->device.common.close = adev_close;

	adev->device.init_check = adev_init_check;
	adev->device.set_voice_volume = adev_set_voice_volume;
	adev->device.set_master_volume = adev_set_master_volume;
	adev->device.get_master_volume = adev_get_master_volume;
	adev->device.set_master_mute = adev_set_master_mute;
	adev->device.get_master_mute = adev_get_master_mute;
	adev->device.set_mode = adev_set_mode;
	adev->device.set_mic_mute = adev_set_mic_mute;
	adev->device.get_mic_mute = adev_get_mic_mute;
	adev->device.set_parameters = adev_set_parameters;
	adev->device.get_parameters = adev_get_parameters;
	adev->device.get_input_buffer_size = adev_get_input_buffer_size;
	adev->device.open_output_stream = adev_open_output_stream;
	adev->device.close_output_stream = adev_close_output_stream;
	adev->device.open_input_stream = adev_open_input_stream;
	adev->device.close_input_stream = adev_close_input_stream;
	adev->device.dump = adev_dump;

	*device = &adev->device.common;
	ALOGI("xdplus audio HAL opened");
	return 0;
}

static struct hw_module_methods_t hal_module_methods = {
	.open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
	.common = {
		.tag = HARDWARE_MODULE_TAG,
		.module_api_version = AUDIO_MODULE_API_VERSION_0_1,
		.hal_api_version = HARDWARE_HAL_API_VERSION,
		.id = AUDIO_HARDWARE_MODULE_ID,
		.name = "GPD XD+ audio HW HAL",
		.author = "GPD XD+ LineageOS port",
		.methods = &hal_module_methods,
	},
};
