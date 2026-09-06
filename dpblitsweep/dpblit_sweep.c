// dpblit_sweep -- a table of deterministic MDP blits driven through
// DpBlitStream, so the geometry the tile calculator produces can be compared
// case by case between the stock libdpframework and the reconstruction.
//
// It exists because screenrecord only ever exercises one source size, one
// destination size and one format pair. A misread offset in the tile
// arithmetic is a wrong picture, not a crash, and most of the space that
// could be wrong is never touched by a recording.
//
// Run it under su: the shell domain cannot reach /dev/mtk_cmdq.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PACK(VIDEO, PLANE, COPLANE, HF, VF, BITS, GROUP, SWAP, UID) \
	(((VIDEO) << 27) | ((PLANE) << 24) | ((COPLANE) << 22) | ((HF) << 20) | \
	 ((VF) << 18) | ((BITS) << 8) | ((GROUP) << 6) | ((SWAP) << 5) | (UID))

#define DP_COLOR_RGB565   PACK(0, 1, 0, 0, 0, 16, 0, 0, 0)
#define DP_COLOR_RGBA8888 PACK(0, 1, 0, 0, 0, 32, 0, 1, 2)
#define DP_COLOR_BGRA8888 PACK(0, 1, 0, 0, 0, 32, 0, 0, 2)
#define DP_COLOR_I420     PACK(0, 3, 0, 1, 1,  8, 1, 0, 8)
#define DP_COLOR_YV12     PACK(0, 3, 0, 1, 1,  8, 1, 1, 8)
#define DP_COLOR_NV12     PACK(0, 2, 1, 1, 1,  8, 1, 0, 12)
#define DP_COLOR_NV21     PACK(0, 2, 1, 1, 1,  8, 1, 1, 12)

// The two formats the MTK AVC decoder actually asks for, read off a live
// decode: a block/tile source (32-row blocks, so the pitch is per block row,
// not per line) into plain I420. Written as literals because they come from
// the device, not from the PACK layout.
#define DP_COLOR_BLOCK_YUV 0x0a55004cu   // decoder output, 1280x736, yp 40960
#define DP_COLOR_DEC_I420  0x03140848u   // the I420 the codec hands upstream

// DpBlitStream::ROT_*, from the reference DpBlitStream.h.
#define ROT_0   0x0
#define ROT_90  0x4
#define ROT_180 0x3
#define ROT_270 0x7

extern int mt_ion_open(const char *name);
extern int ion_alloc_mm(int fd, unsigned size, unsigned align, unsigned flags, unsigned *handle);
extern int ion_share(int fd, unsigned handle, int *shareFd);
extern void *ion_mmap(int fd, void *addr, unsigned size, unsigned flags, int sync, int shareFd, void *extra);
extern int ion_munmap(int fd, void *va, unsigned size);

struct DpRect { int32_t x, sub_x, y, sub_y, w, h, sub_w, sub_h; };

extern void _ZN12DpBlitStreamC1Ev(void *);
extern void _ZN12DpBlitStreamD1Ev(void *);
extern int  _ZN12DpBlitStream12setSrcBufferEiPjj(void *, int, uint32_t *, uint32_t);
extern int  _ZN12DpBlitStream12setDstBufferEiPjj(void *, int, uint32_t *, uint32_t);
extern int  _ZN12DpBlitStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRect8DpSecureb(
	void *, int32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, int32_t, struct DpRect *, int32_t, char);
extern int  _ZN12DpBlitStream12setDstConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRect8DpSecureb(
	void *, int32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, int32_t, struct DpRect *, int32_t, char);
extern int  _ZN12DpBlitStream14setOrientationEj(void *, uint32_t);
extern int  _ZN12DpBlitStream10invalidateEv(void *);

// Plane geometry for the formats the sweep uses. Chroma is rounded up so an
// odd width still describes a buffer the hardware cannot run past.
struct Planes { int n; unsigned size[3]; int yPitch, uvPitch; };

static struct Planes planes_for(uint32_t fmt, int w, int h) {
	struct Planes p;
	memset(&p, 0, sizeof(p));
	const int cw = (w + 1) / 2, ch = (h + 1) / 2;
	if (fmt == DP_COLOR_RGBA8888 || fmt == DP_COLOR_BGRA8888) {
		p.n = 1; p.size[0] = (unsigned)(w * h * 4); p.yPitch = w * 4;
	} else if (fmt == DP_COLOR_RGB565) {
		p.n = 1; p.size[0] = (unsigned)(w * h * 2); p.yPitch = w * 2;
	} else if (fmt == DP_COLOR_YV12 || fmt == DP_COLOR_I420 ||
	           fmt == DP_COLOR_DEC_I420) {
		p.n = 3; p.size[0] = (unsigned)(w * h);
		p.size[1] = p.size[2] = (unsigned)(cw * ch);
		p.yPitch = w; p.uvPitch = cw;
	} else if (fmt == DP_COLOR_BLOCK_YUV) {
		// 32-row blocks: the pitch spans a whole block row (w * 32), and the
		// buffer is the ordinary NV12 size. Both are what the decoder passes.
		p.n = 2; p.size[0] = (unsigned)(w * h);
		p.size[1] = (unsigned)(w * h / 2);
		p.yPitch = w * 32; p.uvPitch = w * 16;
	} else { // NV12 / NV21 / the decoder's I420
		p.n = 2; p.size[0] = (unsigned)(w * h);
		p.size[1] = (unsigned)(cw * 2 * ch);
		p.yPitch = w; p.uvPitch = cw * 2;
	}
	return p;
}

static unsigned total_of(const struct Planes *p) {
	unsigned t = 0;
	for (int i = 0; i < p->n; i++) t += p->size[i];
	return t;
}

static int alloc_ion(int ionFd, unsigned size, int *shareFd, void **va) {
	unsigned handle = 0;
	if (ion_alloc_mm(ionFd, size, 0x40, 3, &handle) != 0) return -1;
	if (ion_share(ionFd, handle, shareFd) != 0) return -1;
	*va = ion_mmap(ionFd, NULL, size, 3, 1, *shareFd, NULL);
	if (*va == NULL || *va == (void *)-1) return -1;
	return 0;
}

struct Case {
	const char *name;
	int sw, sh, dw, dh;
	uint32_t sfmt, dfmt;
	uint32_t rot;
	int cropX, cropY, cropW, cropH;   // 0 width means "the whole source"
};

// Every case is one thing the tile calculator has to get right and that a
// 1:1 640x360 recording never asks it for.
static const struct Case cases[] = {
	{ "baseline",      640, 360,  640, 360, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_0,   0,0,0,0 },
	{ "down2x",       1280, 720,  640, 360, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_0,   0,0,0,0 },
	{ "up2x",          320, 180,  640, 360, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_0,   0,0,0,0 },
	{ "down_odd_ratio",1280, 720,  854, 480, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_0,   0,0,0,0 },
	{ "down_extreme", 1280, 720,  160,  90, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_0,   0,0,0,0 },
	{ "wide_thin",    1280,  64,  640,  32, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_0,   0,0,0,0 },
	{ "tall_thin",      64, 720,   32, 360, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_0,   0,0,0,0 },
	{ "nonmult16",     642, 362,  322, 182, DP_COLOR_RGBA8888, DP_COLOR_RGBA8888, ROT_0,   0,0,0,0 },
	{ "nonmult16_yuv", 642, 362,  642, 362, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_0,   0,0,0,0 },
	{ "rot90",         640, 360,  360, 640, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_90,  0,0,0,0 },
	{ "rot180",        640, 360,  640, 360, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_180, 0,0,0,0 },
	{ "rot270",        640, 360,  360, 640, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_270, 0,0,0,0 },
	{ "rot90_scale",  1280, 720,  360, 640, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_90,  0,0,0,0 },
	{ "to_nv12",       640, 360,  640, 360, DP_COLOR_RGBA8888, DP_COLOR_NV12,     ROT_0,   0,0,0,0 },
	{ "to_nv21",       640, 360,  640, 360, DP_COLOR_RGBA8888, DP_COLOR_NV21,     ROT_0,   0,0,0,0 },
	{ "to_i420",       640, 360,  640, 360, DP_COLOR_RGBA8888, DP_COLOR_I420,     ROT_0,   0,0,0,0 },
	{ "to_rgb565",     640, 360,  640, 360, DP_COLOR_RGBA8888, DP_COLOR_RGB565,   ROT_0,   0,0,0,0 },
	{ "to_bgra",       640, 360,  640, 360, DP_COLOR_RGBA8888, DP_COLOR_BGRA8888, ROT_0,   0,0,0,0 },
	{ "rgb565_src",    640, 360,  640, 360, DP_COLOR_RGB565,   DP_COLOR_YV12,     ROT_0,   0,0,0,0 },
	{ "rgba_to_rgba",  640, 360,  640, 360, DP_COLOR_RGBA8888, DP_COLOR_RGBA8888, ROT_0,   0,0,0,0 },
	{ "crop_centre",  1280, 720,  640, 360, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_0,   320,180,640,360 },
	{ "crop_offset",  1280, 720,  320, 180, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_0,   17,29,640,360 },
	{ "crop_scale",   1280, 720,  854, 480, DP_COLOR_RGBA8888, DP_COLOR_YV12,     ROT_0,   100,100,800,400 },
	// Block-format sources: the decode path, which no case above reaches. The
	// first is exactly what OMX.MTK.VIDEO.DECODER.AVC asks for on a 720p clip.
	{ "dec_block",    1280, 736, 1280, 720, DP_COLOR_BLOCK_YUV, DP_COLOR_DEC_I420, ROT_0,   0,0,1280,720 },
	{ "dec_block_nv12",1280,736, 1280, 720, DP_COLOR_BLOCK_YUV, DP_COLOR_NV12,     ROT_0,   0,0,1280,720 },
	{ "dec_block_half",1280,736,  640, 360, DP_COLOR_BLOCK_YUV, DP_COLOR_YV12,     ROT_0,   0,0,1280,720 },
};

int main(int argc, char **argv) {
	const int reps = (argc > 1) ? atoi(argv[1]) : 1;
	// Case selection, so one case can be run first in a fresh process and
	// "this geometry is wrong" told apart from "this is the second stream".
	const int from = (argc > 2) ? atoi(argv[2]) : 0;
	const int count = (argc > 3) ? atoi(argv[3]) : -1;
	int ionFd = mt_ion_open("dpblit_sweep");
	if (ionFd < 0) { fprintf(stderr, "mt_ion_open failed\n"); return 1; }

	int ncases = (int)(sizeof(cases) / sizeof(cases[0]));
	if (count >= 0 && from + count < ncases) ncases = from + count;
	for (int c = from; c < ncases; c++) {
		const struct Case *k = &cases[c];
		struct Planes sp = planes_for(k->sfmt, k->sw, k->sh);
		struct Planes dp = planes_for(k->dfmt, k->dw, k->dh);
		const unsigned srcSize = total_of(&sp), dstSize = total_of(&dp);

		int srcFd = -1, dstFd = -1;
		void *srcVA = NULL, *dstVA = NULL;
		if (alloc_ion(ionFd, srcSize, &srcFd, &srcVA) != 0 ||
		    alloc_ion(ionFd, dstSize, &dstFd, &dstVA) != 0) {
			printf("%-15s ALLOC-FAIL\n", k->name);
			continue;
		}

		// No constant row and no constant column, so a blit that drops or
		// transposes an axis moves the checksum on its own.
		uint8_t *s = (uint8_t *)srcVA;
		for (unsigned i = 0; i < srcSize; i++)
			s[i] = (uint8_t)((i * 7u + (i / 977u) * 13u) & 0xff);
		memset(dstVA, 0xa5, dstSize);

		char obj[0x400];
		memset(obj, 0, sizeof(obj));
		_ZN12DpBlitStreamC1Ev(obj);

		const int useCrop = (k->cropW != 0);
		struct DpRect srcRoi = { useCrop ? k->cropX : 0, 0, useCrop ? k->cropY : 0, 0,
		                         useCrop ? k->cropW : k->sw, useCrop ? k->cropH : k->sh, 0, 0 };
		struct DpRect dstRoi = { 0, 0, 0, 0, k->dw, k->dh, 0, 0 };

		int sb, sc, db, dc;
		sb = _ZN12DpBlitStream12setSrcBufferEiPjj(obj, srcFd, sp.size, (uint32_t)sp.n);
		sc = _ZN12DpBlitStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRect8DpSecureb(
			obj, k->sw, k->sh, sp.yPitch, sp.uvPitch, k->sfmt, 0, 0, &srcRoi, 0, 1);
		db = _ZN12DpBlitStream12setDstBufferEiPjj(obj, dstFd, dp.size, (uint32_t)dp.n);
		dc = _ZN12DpBlitStream12setDstConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRect8DpSecureb(
			obj, k->dw, k->dh, dp.yPitch, dp.uvPitch, k->dfmt, 0, 0, &dstRoi, 0, 1);
		if (k->rot != ROT_0)
			_ZN12DpBlitStream14setOrientationEj(obj, k->rot);
		// Reported before the blit: invalidate() can block, and then the
		// result line never prints and the setter status is lost.
		printf("%-15s sb=%d sc=%d db=%d dc=%d ", k->name, sb, sc, db, dc);
		fflush(stdout);

		int inv = 0;
		for (int r = 0; r < reps; r++) {
			inv = _ZN12DpBlitStream10invalidateEv(obj);
			if (inv != 0) break;
		}

		const uint8_t *d = (const uint8_t *)dstVA;
		unsigned off = 0, untouched = 0;
		uint32_t sums[3] = { 0, 0, 0 };
		for (int p = 0; p < dp.n; p++) {
			uint32_t sum = 0;
			for (unsigned i = 0; i < dp.size[p]; i++) {
				uint8_t b = d[off + i];
				sum = sum * 31u + b;
				if (b == 0xa5) untouched++;
			}
			sums[p] = sum;
			off += dp.size[p];
		}
		printf("inv=%d P%d %08x %08x %08x untouched=%u/%u\n",
		       inv, dp.n, sums[0], sums[1], sums[2], untouched, dstSize);
		fflush(stdout);

		_ZN12DpBlitStreamD1Ev(obj);
		ion_munmap(ionFd, srcVA, srcSize);
		ion_munmap(ionFd, dstVA, dstSize);
		close(srcFd);
		close(dstFd);
	}
	return 0;
}
