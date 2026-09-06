// dpdec_test -- headless hardware-decode harness, so the decode consumer of
// libdpframework can be measured instead of assumed.
//
// The encode side was proven by screenrecord, and the blit surface by
// dpblit_sweep, but decode had never been checked against anything: the one
// player on this build exits immediately under the stock blob too, so it is
// not an instrument. This drives OMX.MTK.VIDEO.DECODER.AVC directly through
// the NDK MediaCodec API, with no surface, and checksums every output frame.
//
// The decoder itself runs in android.hardware.media.omx@1.0-service, which is
// where the 32-bit libdpframework is loaded -- so this process is the driver,
// not the subject. Whether the MDP is involved at all is a separate question,
// answered by watching /proc/mtk_cmdq_debug/record for that service's pid.
//
// Deterministic by construction: same file, same decoder, same buffer walk,
// so stock and the reconstruction can be compared byte for byte.
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DECODER_NAME "OMX.MTK.VIDEO.DECODER.AVC"

// The same rolling sum dpblit_sweep uses, so the two harnesses' numbers are
// read the same way.
static uint32_t checksum(const uint8_t *p, size_t n) {
	uint32_t sum = 0;
	for (size_t i = 0; i < n; i++) {
		sum = (sum << 1) ^ (sum >> 31) ^ p[i];
	}
	return sum;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: dpdec_test <file.mp4> [max_frames]\n");
		return 2;
	}
	const int maxFrames = (argc > 2) ? atoi(argv[2]) : 60;

	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s failed\n", argv[1]);
		return 1;
	}
	off_t size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);

	AMediaExtractor *ex = AMediaExtractor_new();
	if (AMediaExtractor_setDataSourceFd(ex, fd, 0, size) != AMEDIA_OK) {
		fprintf(stderr, "setDataSourceFd failed\n");
		return 1;
	}

	int track = -1;
	AMediaFormat *fmt = NULL;
	for (size_t i = 0; i < AMediaExtractor_getTrackCount(ex); i++) {
		AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, i);
		const char *mime = NULL;
		if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) &&
		    strncmp(mime, "video/", 6) == 0) {
			track = (int)i;
			fmt = f;
			break;
		}
		AMediaFormat_delete(f);
	}
	if (track < 0) {
		fprintf(stderr, "no video track\n");
		return 1;
	}
	AMediaExtractor_selectTrack(ex, track);

	// By name, not by type: createDecoderByType would hand back the Codec2
	// software decoder and measure nothing about the vendor stack.
	AMediaCodec *codec = AMediaCodec_createCodecByName(DECODER_NAME);
	if (codec == NULL) {
		fprintf(stderr, "createCodecByName(%s) failed\n", DECODER_NAME);
		return 1;
	}
	if (AMediaCodec_configure(codec, fmt, NULL, NULL, 0) != AMEDIA_OK) {
		fprintf(stderr, "configure failed\n");
		return 1;
	}
	if (AMediaCodec_start(codec) != AMEDIA_OK) {
		fprintf(stderr, "start failed\n");
		return 1;
	}

	int frames = 0;
	int inputDone = 0;
	uint32_t all = 0;
	size_t totalBytes = 0;
	while (frames < maxFrames) {
		if (!inputDone) {
			ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 20000);
			if (ib >= 0) {
				size_t cap = 0;
				uint8_t *buf = AMediaCodec_getInputBuffer(codec, ib, &cap);
				ssize_t n = AMediaExtractor_readSampleData(ex, buf, cap);
				if (n < 0) {
					AMediaCodec_queueInputBuffer(codec, ib, 0, 0, 0,
					                             AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
					inputDone = 1;
				} else {
					int64_t pts = AMediaExtractor_getSampleTime(ex);
					AMediaCodec_queueInputBuffer(codec, ib, 0, (size_t)n, pts, 0);
					AMediaExtractor_advance(ex);
				}
			}
		}
		AMediaCodecBufferInfo info;
		ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec, &info, 20000);
		if (ob >= 0) {
			size_t cap = 0;
			uint8_t *buf = AMediaCodec_getOutputBuffer(codec, ob, &cap);
			if (buf != NULL && info.size > 0) {
				uint32_t s = checksum(buf + info.offset, (size_t)info.size);
				all = (all << 1) ^ (all >> 31) ^ s;
				totalBytes += (size_t)info.size;
				printf("frame %3d size=%-8d sum=%08x\n", frames, info.size, s);
				frames++;
			}
			AMediaCodec_releaseOutputBuffer(codec, ob, false);
			if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
				break;
			}
		} else if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			AMediaFormat *of = AMediaCodec_getOutputFormat(codec);
			printf("format %s\n", AMediaFormat_toString(of));
			AMediaFormat_delete(of);
		} else if (ob == AMEDIACODEC_INFO_TRY_AGAIN_LATER && inputDone) {
			break;
		}
	}

	printf("RESULT frames=%d bytes=%zu sum=%08x\n", frames, totalBytes, all);
	AMediaCodec_stop(codec);
	AMediaCodec_delete(codec);
	AMediaFormat_delete(fmt);
	AMediaExtractor_delete(ex);
	close(fd);
	return frames > 0 ? 0 : 1;
}
