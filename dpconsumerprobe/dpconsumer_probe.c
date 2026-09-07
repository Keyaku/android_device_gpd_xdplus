// dpconsumer_probe -- exercises the DpBlitStream entry points that only
// libgpu_aux and libClearMotionFW import, which dpblit_sweep never touches.
//
// Those two consumers are the last unexercised ones, and neither has a
// user-visible driver: gpu_aux is pulled in by the GPU aux path and
// ClearMotionFW by the frame-rate converter, so nothing on this device calls
// them on demand. Rather than load the blobs, this drives the exact overloads
// they import, since between them they use no class but DpBlitStream.
//
// The test is not "did it return 0". Each case binds the SAME ion buffer
// through every binding variant and compares the destination checksum against
// the fd binding, which dpblit_sweep already validates. A variant that maps
// the wrong address produces a different checksum, or an untouched buffer.
//
// Run it under su: the shell domain cannot reach /dev/mtk_cmdq.
//
// The canonical source is tools/dpconsumer_probe/dpconsumer_probe.c.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PACK(VIDEO, PLANE, COPLANE, HF, VF, BITS, GROUP, SWAP, UID) \
	(((VIDEO) << 27) | ((PLANE) << 24) | ((COPLANE) << 22) | ((HF) << 20) | \
	 ((VF) << 18) | ((BITS) << 8) | ((GROUP) << 6) | ((SWAP) << 5) | (UID))

#define DP_COLOR_RGBA8888 PACK(0, 1, 0, 0, 0, 32, 0, 1, 2)
#define DP_COLOR_YV12     PACK(0, 3, 0, 1, 1,  8, 1, 1, 8)
#define DP_COLOR_NV12     PACK(0, 2, 1, 1, 1,  8, 1, 0, 12)

#define ROT_0  0x0
#define ROT_90 0x4

extern int mt_ion_open(const char *name);
extern int ion_alloc_mm(int fd, unsigned size, unsigned align, unsigned flags, unsigned *handle);
extern int ion_share(int fd, unsigned handle, int *shareFd);
extern void *ion_mmap(int fd, void *addr, unsigned size, unsigned flags, int sync, int shareFd, void *extra);
extern int ion_munmap(int fd, void *va, unsigned size);
extern int ion_custom_ioctl(int fd, unsigned int cmd, void *arg);

// MTK ION userspace ABI. The union at the head is pointer-aligned on lib64,
// so every field after the command word moves; mirrored from ion_uapi.h.
enum { ION_CMD_SYSTEM = 0, ION_CMD_MULTIMEDIA = 1 };
enum { ION_MM_CONFIG_BUFFER = 0 };
enum { ION_SYS_GET_PHYS = 1 };

struct IonMmData {
	uint32_t mm_cmd;
#ifdef __LP64__
	uint32_t pad04;
#endif
	uint32_t handle;
#ifdef __LP64__
	uint32_t handleHi_;
#endif
	int32_t  eModuleID;
	uint32_t security;
	uint32_t coherent;
	uint32_t reserve_iova_start;
	uint32_t reserve_iova_end;
};

struct IonSysData {
	uint32_t sys_cmd;
#ifdef __LP64__
	uint32_t pad04;
#endif
	uint32_t handle;
#ifdef __LP64__
	uint32_t handleHi_;
#endif
	uint32_t phy_addr;
#ifdef __LP64__
	uint32_t pad_;
#endif
	unsigned long len;
};

// The MDP port for engine 0 in the library's own kM4UPortByEngine table.
#define M4U_PORT_MDP 0x1d

// Bind the buffer to its M4U port, then read back the MVA the hardware uses.
static uint32_t mva_of(int ionFd, unsigned handle) {
	struct IonMmData cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.mm_cmd = ION_MM_CONFIG_BUFFER;
	cfg.handle = handle;
	cfg.eModuleID = M4U_PORT_MDP;
	// 0x10000 is the driver's partial-success code, not a failure.
	int rc = ion_custom_ioctl(ionFd, ION_CMD_MULTIMEDIA, &cfg);
	if (rc > 0 && rc != 0x10000) return 0;

	struct IonSysData pa;
	memset(&pa, 0, sizeof(pa));
	pa.sys_cmd = ION_SYS_GET_PHYS;
	pa.handle = handle;
	if (ion_custom_ioctl(ionFd, ION_CMD_SYSTEM, &pa) != 0) return 0;
	return pa.phy_addr;
}

struct DpRect { int32_t x, sub_x, y, sub_y, w, h, sub_w, sub_h; };

// Layout asserted in dp_types.h; only the first three words are set here.
struct DpPqParam {
	uint32_t reserved0;
	uint32_t scenario;
	uint32_t videoID;
	uint8_t  tail[0x98 - 0x0c];
};

extern void _ZN12DpBlitStreamC1Ev(void *);
extern void _ZN12DpBlitStreamD1Ev(void *);
extern int  _ZN12DpBlitStream12setSrcBufferEiPjj(void *, int, uint32_t *, uint32_t);
extern int  _ZN12DpBlitStream12setDstBufferEiPjj(void *, int, uint32_t *, uint32_t);
// The four overloads with no coverage before this probe.
extern int  _ZN12DpBlitStream12setSrcBufferEPPvPjj(void *, void **, uint32_t *, uint32_t);
extern int  _ZN12DpBlitStream12setDstBufferEPPvPjj(void *, void **, uint32_t *, uint32_t);
extern int  _ZN12DpBlitStream12setSrcBufferEPPvS1_Pjj(void *, void **, void **, uint32_t *, uint32_t);
extern int  _ZN12DpBlitStream12setDstBufferEPPvS1_Pjj(void *, void **, void **, uint32_t *, uint32_t);
extern int  _ZN12DpBlitStream14setPQParameterERK9DpPqParam(void *, const struct DpPqParam *);
extern int  _ZN12DpBlitStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRect8DpSecureb(
	void *, int32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, int32_t, struct DpRect *, int32_t, char);
extern int  _ZN12DpBlitStream12setDstConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRect8DpSecureb(
	void *, int32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, int32_t, struct DpRect *, int32_t, char);
extern int  _ZN12DpBlitStream14setOrientationEj(void *, uint32_t);
extern int  _ZN12DpBlitStream10invalidateEv(void *);

struct Planes { int n; unsigned size[3]; int yPitch, uvPitch; };

static struct Planes planes_for(uint32_t fmt, int w, int h) {
	struct Planes p;
	memset(&p, 0, sizeof(p));
	const int cw = (w + 1) / 2, ch = (h + 1) / 2;
	if (fmt == DP_COLOR_RGBA8888) {
		p.n = 1; p.size[0] = (unsigned)(w * h * 4); p.yPitch = w * 4;
	} else if (fmt == DP_COLOR_YV12) {
		p.n = 3; p.size[0] = (unsigned)(w * h);
		p.size[1] = p.size[2] = (unsigned)(cw * ch);
		p.yPitch = w; p.uvPitch = cw;
	} else { // NV12
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

static int alloc_ion(int ionFd, unsigned size, int *shareFd, void **va, unsigned *handleOut) {
	unsigned handle = 0;
	if (ion_alloc_mm(ionFd, size, 0x40, 3, &handle) != 0) return -1;
	if (handleOut) *handleOut = handle;
	if (ion_share(ionFd, handle, shareFd) != 0) return -1;
	*va = ion_mmap(ionFd, NULL, size, 3, 1, *shareFd, NULL);
	if (*va == NULL || *va == (void *)-1) return -1;
	return 0;
}

// Plane base pointers into one contiguous ion allocation, which is how both
// consumers hand a multi-plane buffer over.
static void plane_ptrs(void *base, const struct Planes *p, void **out) {
	uint8_t *b = (uint8_t *)base;
	unsigned off = 0;
	for (int i = 0; i < p->n; i++) { out[i] = b + off; off += p->size[i]; }
}

enum BindMode { BIND_FD, BIND_VA, BIND_VA_MVA_ZERO, BIND_VA_MVA_VA, BIND_VA_MVA_REAL };

static const char *bind_name(enum BindMode m) {
	switch (m) {
	case BIND_FD:           return "fd";
	case BIND_VA:           return "valist";
	case BIND_VA_MVA_ZERO:  return "va+mva0";
	case BIND_VA_MVA_VA:    return "va+mva=va";
	default:                return "va+mva=real";
	}
}

// Consumer that imports each binding, for reading the result table.
static const char *bind_owner(enum BindMode m) {
	switch (m) {
	case BIND_FD:  return "gpu_aux/sweep";
	case BIND_VA:  return "gpu_aux";
	default:       return "ClearMotionFW";
	}
}

struct Case { const char *name; int sw, sh, dw, dh; uint32_t sfmt, dfmt; uint32_t rot; };

static const struct Case cases[] = {
	{ "baseline",  640, 360, 640, 360, DP_COLOR_RGBA8888, DP_COLOR_YV12, ROT_0  },
	{ "scale_down",1280, 720, 640, 360, DP_COLOR_RGBA8888, DP_COLOR_YV12, ROT_0  },
	{ "to_nv12",   640, 360, 640, 360, DP_COLOR_RGBA8888, DP_COLOR_NV12, ROT_0  },
	{ "rot90",     640, 360, 360, 640, DP_COLOR_RGBA8888, DP_COLOR_YV12, ROT_90 },
};

// One blit. Returns the destination checksum, or 0 with *ok cleared.
static uint32_t run_case(int ionFd, const struct Case *k, enum BindMode mode,
                         int *ok, int *statuses, unsigned *untouchedOut) {
	struct Planes sp = planes_for(k->sfmt, k->sw, k->sh);
	struct Planes dp = planes_for(k->dfmt, k->dw, k->dh);
	const unsigned srcSize = total_of(&sp), dstSize = total_of(&dp);

	int srcFd = -1, dstFd = -1;
	void *srcVA = NULL, *dstVA = NULL;
	*ok = 0;
	unsigned srcHandle = 0, dstHandle = 0;
	if (alloc_ion(ionFd, srcSize, &srcFd, &srcVA, &srcHandle) != 0 ||
	    alloc_ion(ionFd, dstSize, &dstFd, &dstVA, &dstHandle) != 0)
		return 0;

	uint8_t *s = (uint8_t *)srcVA;
	for (unsigned i = 0; i < srcSize; i++)
		s[i] = (uint8_t)((i * 7u + (i / 977u) * 13u) & 0xff);
	memset(dstVA, 0xa5, dstSize);

	char obj[0x400];
	memset(obj, 0, sizeof(obj));
	_ZN12DpBlitStreamC1Ev(obj);

	void *sPtr[3] = { 0 }, *dPtr[3] = { 0 };
	void *sMva[3] = { 0 }, *dMva[3] = { 0 };
	plane_ptrs(srcVA, &sp, sPtr);
	plane_ptrs(dstVA, &dp, dPtr);

	int sb = -1, db = -1;
	switch (mode) {
	case BIND_FD:
		sb = _ZN12DpBlitStream12setSrcBufferEiPjj(obj, srcFd, sp.size, (uint32_t)sp.n);
		db = _ZN12DpBlitStream12setDstBufferEiPjj(obj, dstFd, dp.size, (uint32_t)dp.n);
		break;
	case BIND_VA:
		sb = _ZN12DpBlitStream12setSrcBufferEPPvPjj(obj, sPtr, sp.size, (uint32_t)sp.n);
		db = _ZN12DpBlitStream12setDstBufferEPPvPjj(obj, dPtr, dp.size, (uint32_t)dp.n);
		break;
	case BIND_VA_MVA_ZERO:
		// MVA left null: if the library resolves it, the blit matches fd.
		sb = _ZN12DpBlitStream12setSrcBufferEPPvS1_Pjj(obj, sPtr, sMva, sp.size, (uint32_t)sp.n);
		db = _ZN12DpBlitStream12setDstBufferEPPvS1_Pjj(obj, dPtr, dMva, dp.size, (uint32_t)dp.n);
		break;
	case BIND_VA_MVA_VA:
		// MVA = VA, the other reading of the parameter. Both cannot be right.
		memcpy(sMva, sPtr, sizeof(sMva));
		memcpy(dMva, dPtr, sizeof(dMva));
		sb = _ZN12DpBlitStream12setSrcBufferEPPvS1_Pjj(obj, sPtr, sMva, sp.size, (uint32_t)sp.n);
		db = _ZN12DpBlitStream12setDstBufferEPPvS1_Pjj(obj, dPtr, dMva, dp.size, (uint32_t)dp.n);
		break;
	default: {
		// Real MVAs, from the same two ioctls DpIonHandler::mapHWAddress uses.
		// The list holds 32-bit MVAs, so each slot is written as one.
		uint32_t sBase = mva_of(ionFd, srcHandle), dBase = mva_of(ionFd, dstHandle);
		if (sBase == 0 || dBase == 0)
			fprintf(stderr, "  (MVA lookup failed: src=0x%08x dst=0x%08x)\n", sBase, dBase);
		uint32_t sOff = 0, dOff = 0;
		for (int i = 0; i < sp.n; i++) { ((uint32_t *)sMva)[i] = sBase + sOff; sOff += sp.size[i]; }
		for (int i = 0; i < dp.n; i++) { ((uint32_t *)dMva)[i] = dBase + dOff; dOff += dp.size[i]; }
		sb = _ZN12DpBlitStream12setSrcBufferEPPvS1_Pjj(obj, sPtr, sMva, sp.size, (uint32_t)sp.n);
		db = _ZN12DpBlitStream12setDstBufferEPPvS1_Pjj(obj, dPtr, dMva, dp.size, (uint32_t)dp.n);
		break;
	}
	}

	struct DpRect srcRoi = { 0, 0, 0, 0, k->sw, k->sh, 0, 0 };
	struct DpRect dstRoi = { 0, 0, 0, 0, k->dw, k->dh, 0, 0 };
	int sc = _ZN12DpBlitStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRect8DpSecureb(
		obj, k->sw, k->sh, sp.yPitch, sp.uvPitch, k->sfmt, 0, 0, &srcRoi, 0, 1);
	int dc = _ZN12DpBlitStream12setDstConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRect8DpSecureb(
		obj, k->dw, k->dh, dp.yPitch, dp.uvPitch, k->dfmt, 0, 0, &dstRoi, 0, 1);
	if (k->rot != ROT_0)
		_ZN12DpBlitStream14setOrientationEj(obj, k->rot);

	int inv = _ZN12DpBlitStream10invalidateEv(obj);
	statuses[0] = sb; statuses[1] = db; statuses[2] = sc;
	statuses[3] = dc; statuses[4] = inv;

	const uint8_t *d = (const uint8_t *)dstVA;
	uint32_t sum = 0;
	unsigned untouched = 0;
	for (unsigned i = 0; i < dstSize; i++) {
		sum = sum * 31u + d[i];
		if (d[i] == 0xa5) untouched++;
	}
	*untouchedOut = untouched;

	_ZN12DpBlitStreamD1Ev(obj);
	ion_munmap(ionFd, srcVA, srcSize);
	ion_munmap(ionFd, dstVA, dstSize);
	close(srcFd);
	close(dstFd);
	*ok = 1;
	return sum;
}

// setPQParameter is reached by both consumers and by nothing else, so it has
// never run. The scenario check is unreachable while m_pqSupport is 0: the
// reconstruction stubs the PQ surface, so every scenario returns SUCCESS.
static void probe_pq(void) {
	printf("\n-- setPQParameter (imported by gpu_aux and ClearMotionFW) --\n");
	for (uint32_t sc = 0; sc <= 5; sc++) {
		char obj[0x400];
		memset(obj, 0, sizeof(obj));
		_ZN12DpBlitStreamC1Ev(obj);

		struct DpPqParam p;
		memset(&p, 0, sizeof(p));
		p.scenario = sc;
		p.videoID = 0x1234;

		int rc = _ZN12DpBlitStream14setPQParameterERK9DpPqParam(obj, &p);
		// Stock validates the scenario; ours short-circuits before it. A
		// non-zero rc here would mean PQ stopped being inert.
		const int wouldAccept = (sc <= 4) && (((1u << (sc & 0x1f)) & 0x16u) != 0);
		printf("  scenario=%u rc=%-3d stock=%s%s\n", sc, rc,
		       wouldAccept ? "accept" : "reject",
		       rc == 0 ? "" : "   <== NOT INERT");
		_ZN12DpBlitStreamD1Ev(obj);
	}
}

int main(int argc, char **argv) {
	const int pqOnly = (argc > 1 && strcmp(argv[1], "pq") == 0);
	int ionFd = mt_ion_open("dpconsumer_probe");
	if (ionFd < 0) { fprintf(stderr, "mt_ion_open failed\n"); return 1; }

	if (!pqOnly) {
		printf("%-11s %-11s %-14s %-9s %-9s %s\n",
		       "case", "bind", "consumer", "checksum", "untouched", "status");
		for (int c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
			uint32_t ref = 0;
			int haveRef = 0;
			for (int m = BIND_FD; m <= BIND_VA_MVA_REAL; m++) {
				int ok = 0, st[5] = { 0 };
				unsigned untouched = 0;
				uint32_t sum = run_case(ionFd, &cases[c], (enum BindMode)m,
				                        &ok, st, &untouched);
				if (!ok) { printf("%-11s %-11s ALLOC-FAIL\n", cases[c].name, bind_name(m)); continue; }
				if (m == BIND_FD) { ref = sum; haveRef = 1; }

				const char *verdict = "";
				if (haveRef && m != BIND_FD)
					verdict = (sum == ref) ? "MATCH" : "DIFFERS-FROM-FD";
				printf("%-11s %-11s %-14s %08x  %8u  sb=%d db=%d sc=%d dc=%d inv=%d %s\n",
				       cases[c].name, bind_name((enum BindMode)m), bind_owner((enum BindMode)m),
				       sum, untouched, st[0], st[1], st[2], st[3], st[4], verdict);
				fflush(stdout);
			}
		}
	}

	probe_pq();
	return 0;
}
