// dpasync_sweep -- the same idea as dpblit_sweep, one API up: deterministic
// blits driven through DpAsyncBlitStream, the class the composer actually
// uses and that no harness had ever touched.
//
// dpblit_sweep drives seven DpBlitStream symbols. hwcomposer imports thirty,
// fifteen of them DpAsyncBlitStream: the job/fence lifecycle (createJob,
// setConfigBegin/End, cancelJob), the four-port destination fan-out, the
// separate setSrcCrop, and setUser. None of that is reachable from the sync
// class, so it is measured here, on the bench, against a stock reference,
// rather than first met with a cable plugged in.
//
// ⚠️ The composer only ever drives port 0 -- every AsyncBliterHandler call
// site is setDst(0, ...), and the mirror is a second job, not a second port.
// The fan-out cases stay because the API exposes four ports and the path
// composer's engine assignment is what decides which shapes are legal.
//
// The call order below is BliterNode::invalidate's, argument for argument.
//
// Run it under su: the shell domain cannot reach /dev/mtk_cmdq.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

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

// DpBlitStream::ROT_*, from the reference DpBlitStream.h.
#define ROT_0   0x0
#define ROT_90  0x4
#define ROT_180 0x3
#define ROT_270 0x7

// hwcomposer's two setUser values, read out of AsyncBliterHandler's
// constructor: 5 on a non-primary display (the mirror), 0 on the primary.
// 5 maps the stream to scenario 4; 0 is refused and leaves it at 1. Both are
// single-thread path variants, and they are the only two the composer uses.
#define USER_PRIMARY 0u
#define USER_MIRROR  5u
// The other accepted values, which no consumer on this device passes. 1 and 2
// map to scenario 2; 4 maps to scenario 3, and scenario 3 is the ONLY way in
// to the multi-thread path variant -- DpPath<DpTileEngine,DpMultiThread>, a
// 1200-line file that nothing else in the system can reach.
#define USER_SC2     1u
#define USER_MULTI   4u

extern int mt_ion_open(const char *name);
extern int ion_alloc_mm(int fd, unsigned size, unsigned align, unsigned flags, unsigned *handle);
extern int ion_share(int fd, unsigned handle, int *shareFd);
extern void *ion_mmap(int fd, void *addr, unsigned size, unsigned flags, int sync, int shareFd, void *extra);
extern int ion_munmap(int fd, void *va, unsigned size);
extern int sync_wait(int fd, int timeout);

// 0x18 bytes, six int32_t. setSrcCrop takes it BY VALUE, so the trailing pair
// dpblit_sweep carries would change the ABI here, not just waste stack.
struct DpRect { int32_t x, sub_x, y, sub_y, w, h; };

// Only scenario (+0x04) and videoID (+0x08) are read before the copy; the
// rest is the PQ path and stays opaque. scenario must be 1 or 2 or the setter
// refuses -- 0 is not an accepted value.
struct DpPqParam { uint32_t reserved0, scenario, videoID; uint8_t tail[0x98 - 0x0c]; };

extern void _ZN17DpAsyncBlitStreamC1Ev(void *);
extern void _ZN17DpAsyncBlitStreamD1Ev(void *);
extern int  _ZN17DpAsyncBlitStream7setUserEj(void *, uint32_t);
extern int  _ZN17DpAsyncBlitStream9createJobERjRi(void *, uint32_t *, int32_t *);
extern int  _ZN17DpAsyncBlitStream9cancelJobEj(void *, uint32_t);
extern int  _ZN17DpAsyncBlitStream14setConfigBeginEj(void *, uint32_t);
extern int  _ZN17DpAsyncBlitStream12setConfigEndEv(void *);
extern int  _ZN17DpAsyncBlitStream12setSrcBufferEiPjji(void *, int, uint32_t *, uint32_t, int32_t);
extern int  _ZN17DpAsyncBlitStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormat8DpSecureb(
	void *, int32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, char);
extern int  _ZN17DpAsyncBlitStream12setDstBufferEiiPjji(void *, int32_t, int, uint32_t *, uint32_t, int32_t);
// The secure overloads. BliterNode takes these instead of the fd forms
// whenever the source usage is protected or the port carries a dst handle, so
// they sit on the mirror's own path and no harness has ever driven them.
extern int  _ZN17DpAsyncBlitStream12setSrcBufferEPPvPjji(void *, void **, uint32_t *, uint32_t, int32_t);
extern int  _ZN17DpAsyncBlitStream12setDstBufferEiPPvPjji(void *, int32_t, void **, uint32_t *, uint32_t, int32_t);
extern int  _ZN17DpAsyncBlitStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRect8DpSecureb(
	void *, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, int32_t,
	struct DpRect *, int32_t, char);
// ⚠️ By value in C++, but both blobs pass it indirectly -- 64-bit because it
// is over 16 bytes, 32-bit because the class is non-trivially-copyable there.
// A C by-value parameter puts it on the stack on arm32 and stock faults.
extern int  _ZN17DpAsyncBlitStream10setSrcCropEi6DpRect(void *, int32_t, struct DpRect *);
extern int  _ZN17DpAsyncBlitStream14setOrientationEij(void *, int32_t, uint32_t);
extern int  _ZN17DpAsyncBlitStream14setPQParameterEiRK9DpPqParam(void *, int32_t, const struct DpPqParam *);
extern int  _ZN17DpAsyncBlitStream10invalidateEv(void *);
// ⚠️ static, despite the ordinary-looking mangling: there is no `this`. A
// call that passes the stream object shifts every argument by one and the
// answer is false for everything, which reads exactly like a hardware limit.
extern char _ZN17DpAsyncBlitStream14queryHWSupportEjjjji13DP_COLOR_ENUMS0_(
	uint32_t, uint32_t, uint32_t, uint32_t, int32_t, uint32_t, uint32_t);
extern char _ZN12DpBlitStream14queryHWSupportEjjjji13DP_COLOR_ENUMS0_(
	uint32_t, uint32_t, uint32_t, uint32_t, int32_t, uint32_t, uint32_t);

// sizeof(DpAsyncBlitStream) is 0x940 on lib64 and 0x88c on lib32; one buffer
// covers both with room to spare, and the constructor writes what it needs.
#define OBJ_BYTES 0x1000

// The composer's mirror queue is three deep; four leaves room to ask whether
// the depth itself matters without another rebuild.
#define DST_RING_MAX 4

struct Planes { int n; unsigned size[3]; int yPitch, uvPitch; };

static struct Planes planes_for(uint32_t fmt, int w, int h) {
	struct Planes p;
	memset(&p, 0, sizeof(p));
	const int cw = (w + 1) / 2, ch = (h + 1) / 2;
	if (fmt == DP_COLOR_RGBA8888 || fmt == DP_COLOR_BGRA8888) {
		p.n = 1; p.size[0] = (unsigned)(w * h * 4); p.yPitch = w * 4;
	} else if (fmt == DP_COLOR_RGB565) {
		p.n = 1; p.size[0] = (unsigned)(w * h * 2); p.yPitch = w * 2;
	} else if (fmt == DP_COLOR_YV12 || fmt == DP_COLOR_I420) {
		p.n = 3; p.size[0] = (unsigned)(w * h);
		p.size[1] = p.size[2] = (unsigned)(cw * ch);
		p.yPitch = w; p.uvPitch = cw;
	} else { // NV12 / NV21
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

static int alloc_ion(int ionFd, unsigned size, int *shareFd, void **va, unsigned *outHandle) {
	unsigned handle = 0;
	if (ion_alloc_mm(ionFd, size, 0x40, 3, &handle) != 0) return -1;
	if (ion_share(ionFd, handle, shareFd) != 0) return -1;
	*va = ion_mmap(ionFd, NULL, size, 3, 1, *shareFd, NULL);
	if (*va == NULL || *va == (void *)-1) return -1;
	if (outHandle != NULL) *outHandle = handle;
	return 0;
}

// One destination port. crop is the SOURCE roi for that port, set through the
// separate setSrcCrop the sync class does not have; a zero width means the
// whole source.
struct Port {
	int dw, dh;
	uint32_t dfmt;
	uint32_t rot;
	int cropX, cropY, cropW, cropH;
};

struct Case {
	const char *name;
	uint32_t user;
	int sw, sh;
	uint32_t sfmt;
	int nports;
	struct Port port[4];
	int pqScenario;   // 0 = do not call setPQParameter at all
	int cancelExtra;  // create and cancel a second job around this one
	// 1 when no consumer on this device can produce this configuration, or
	// when the harness cannot honestly produce it (the secure cases hand the
	// MDP a protected descriptor for an ordinary buffer). The
	// 64-bit composer is the only caller of setUser and passes 0 or 5, so
	// scenarios 2 and 3 are dead code; the fan-out shapes are refused by the
	// path composer under stock. Skipped unless argv[4] asks for them, because
	// a fault in dead code must not stop the cases that matter.
	int unreachable;
	// 1 when the case is known to HANG and take the display's CMDQ thread with
	// it. ⚠️⚠️ A wedged CMDQ freezes the panel at the kernel level: VSYNC times
	// out on primary_disp and only a reboot clears it. Needs its own opt-in.
	int wedges;
	// Frames to run through ONE stream, and jobs created per frame before the
	// one that gets configured. The composer creates three (two fill-black
	// plus the output) every frame and configures only the last, so a harness
	// that creates one job and invalidates once never sees what it does to
	// the queues. 0 means the plain single-shot behaviour.
	int frames;
	int extraJobsPerFrame;
	// Destination buffers cycled per port, one per frame. The physical mirror
	// draws into a DisplayBufferQueue buffer fetched fresh every frame, so a
	// harness holding one buffer for every frame cannot see what the rotation
	// does to the destination pool. 0 or 1 keeps the single-buffer behaviour.
	int dstRing;
	// DpSecure for the source and every destination port. Non-zero also
	// switches both setters to their handle overloads, which is what
	// BliterNode does.
	int srcSecure;
	int dstSecure;
};

// Every case is something the composer can ask for and dpblit_sweep cannot
// express. The single-port block is deliberately a subset of the sync sweep's
// geometries: those already match stock through DpBlitStream, so a divergence
// here is the async wrapper and nothing else.
static const struct Case cases[] = {
	// -- single port, geometries already green on the sync path --------------
	{ "a_baseline",   USER_PRIMARY, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "a_down2x",     USER_PRIMARY, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "a_rot90",      USER_PRIMARY, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 360, 640, DP_COLOR_YV12, ROT_90, 0,0,0,0 } }, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "a_rot270",     USER_PRIMARY, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 360, 640, DP_COLOR_YV12, ROT_270, 0,0,0,0 } }, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "a_to_rgb565",  USER_PRIMARY, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_RGB565, ROT_0, 0,0,0,0 } }, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "a_rgba_rgba",  USER_PRIMARY, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_RGBA8888, ROT_0, 0,0,0,0 } }, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	// -- setSrcCrop, which only exists on this class -------------------------
	{ "a_crop_centre", USER_PRIMARY, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 320,180,640,360 } }, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "a_crop_scale",  USER_PRIMARY, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 854, 480, DP_COLOR_YV12, ROT_0, 100,100,800,400 } }, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	// -- setUser: the mirror's own scenario ---------------------------------
	{ "a_user5",      USER_MIRROR, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "a_user5_down", USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	// -- the fan-out. This is the mirror's shape: one source, two sinks at
	//    different sizes, and on the device one of them is the panel and the
	//    other the HDMI encoder.
	{ "a_two_same",   USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 2,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_two_sizes",  USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 2,
	  { { 1280, 720, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_two_formats", USER_MIRROR, 640, 360, DP_COLOR_RGBA8888, 2,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 640, 360, DP_COLOR_RGBA8888, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_two_rots",   USER_MIRROR, 640, 360, DP_COLOR_RGBA8888, 2,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 360, 640, DP_COLOR_YV12, ROT_90, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_two_crops",  USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 2,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,640,360 },
	    { 640, 360, DP_COLOR_YV12, ROT_0, 640,360,640,360 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_three_port", USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 3,
	  { { 1280, 720, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 640, 360, DP_COLOR_NV12, ROT_0, 0,0,0,0 },
	    { 320, 180, DP_COLOR_RGBA8888, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_four_port",  USER_MIRROR, 640, 360, DP_COLOR_RGBA8888, 4,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 320, 180, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 640, 360, DP_COLOR_NV21, ROT_0, 0,0,0,0 },
	    { 180, 320, DP_COLOR_YV12, ROT_270, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	// -- the lifecycle corners ----------------------------------------------
	{ "a_pq_sc1",     USER_MIRROR, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "a_pq_sc2",     USER_MIRROR, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 2, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "a_cancel",     USER_MIRROR, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 1, 0, 0, 0, 0, 0, 0, 0 },
	// The physical mirror's own shape: 720p panel scaled 1.5x to a 1080p sink,
	// driven the way AsyncBliterHandler drives it — three jobs created per
	// frame with only the last configured, repeated across frames on ONE
	// stream. A single-shot harness cannot see what that does to the queues.
	{ "a_mirror",     USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 1920, 1080, DP_COLOR_RGBA8888, ROT_0, 0,0,0,0 } }, 0, 0, 0, 0, 3, 2, 0, 0, 0 },
	{ "a_mirror_yuv", USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 1920, 1080, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 0, 0, 3, 2, 0, 0, 0 },
	// The same shape with the destination rotating, which is what
	// getDisplayBufferQueue hands the mirror: a different buffer every frame
	// against the one pool. A fixed destination cannot show what the rotation
	// does to the pool at bringup.
	{ "a_mirror_ring", USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 1920, 1080, DP_COLOR_RGBA8888, ROT_0, 0,0,0,0 } }, 0, 0, 0, 0, 3, 2, 3, 0, 0 },
	{ "a_mirror_ring_yuv", USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 1920, 1080, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 0, 0, 3, 2, 3, 0, 0 },
	// Six frames over a three-deep ring, so every slot is reused rather than
	// merely visited once.
	{ "a_mirror_ring6", USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 1920, 1080, DP_COLOR_RGBA8888, ROT_0, 0,0,0,0 } }, 0, 0, 0, 0, 6, 2, 3, 0, 0 },
	// ⚠️ Secure: opt-in, because these hand the MDP a secure descriptor for a
	// buffer this harness cannot actually protect, and the secure path is not
	// known to fail safely. They drive the handle overloads BliterNode takes.
	{ "a_mirror_sec", USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 1920, 1080, DP_COLOR_RGBA8888, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 3, 2, 0, 1, 1 },
	{ "a_mirror_sec_src", USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 1920, 1080, DP_COLOR_RGBA8888, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 3, 2, 0, 1, 0 },
	{ "a_mirror_ring_sec", USER_MIRROR, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 1920, 1080, DP_COLOR_RGBA8888, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 3, 2, 3, 1, 1 },
	// -- the other scenarios, and the multi-thread path with them ------------
	{ "a_sc2",        USER_SC2, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_sc2_down",   USER_SC2, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_sc2_rot90",  USER_SC2, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 360, 640, DP_COLOR_YV12, ROT_90, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_mt",         USER_MULTI, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	{ "a_mt_down",    USER_MULTI, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	{ "a_mt_rot90",   USER_MULTI, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 360, 640, DP_COLOR_YV12, ROT_90, 0,0,0,0 } }, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	{ "a_mt_crop",    USER_MULTI, 1280, 720, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 320,180,640,360 } }, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	{ "a_mt_rgb565",  USER_MULTI, 640, 360, DP_COLOR_RGBA8888, 1,
	  { { 640, 360, DP_COLOR_RGB565, ROT_0, 0,0,0,0 } }, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	{ "a_mt_two",     USER_MULTI, 1280, 720, DP_COLOR_RGBA8888, 2,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	// -- fan-out shapes that split the output engines differently. The path
	//    composer refuses two plain targets; a rotated target takes a WROT and
	//    a plain one can take the WDMA, so these are the shapes that could
	//    legally coexist.
	{ "a_two_r0_p1",  USER_PRIMARY, 640, 360, DP_COLOR_RGBA8888, 2,
	  { { 360, 640, DP_COLOR_YV12, ROT_90, 0,0,0,0 },
	    { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_two_bothrot",USER_PRIMARY, 640, 360, DP_COLOR_RGBA8888, 2,
	  { { 360, 640, DP_COLOR_YV12, ROT_90, 0,0,0,0 },
	    { 360, 640, DP_COLOR_YV12, ROT_270, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_two_1to1",   USER_PRIMARY, 640, 360, DP_COLOR_RGBA8888, 2,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 640, 360, DP_COLOR_NV12, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_two_sc2",    USER_SC2, 640, 360, DP_COLOR_RGBA8888, 2,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	// Scenario 2 is the only one whose fan-out reaches the tile calculator at
	// all, so the shapes that could split the output engines are tried there.
	{ "a_two_sc2_rot",USER_SC2, 640, 360, DP_COLOR_RGBA8888, 2,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 360, 640, DP_COLOR_YV12, ROT_90, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_two_sc2_sz", USER_SC2, 1280, 720, DP_COLOR_RGBA8888, 2,
	  { { 1280, 720, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_two_sc2_fmt",USER_SC2, 640, 360, DP_COLOR_RGBA8888, 2,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,0,0 },
	    { 640, 360, DP_COLOR_RGBA8888, ROT_0, 0,0,0,0 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
	{ "a_two_sc2_crop",USER_SC2, 1280, 720, DP_COLOR_RGBA8888, 2,
	  { { 640, 360, DP_COLOR_YV12, ROT_0, 0,0,640,360 },
	    { 640, 360, DP_COLOR_YV12, ROT_0, 640,360,640,360 } }, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
};

// queryHWSupport answers before any buffer exists, and a wrong false silently
// drops the composer to GPU composition -- correct pixels, wrong power. Its
// return is the whole measurement.
struct QCase { const char *name; uint32_t sw, sh, dw, dh; int32_t rot; uint32_t sfmt, dfmt; };
static const struct QCase qcases[] = {
	{ "q_1to1",     640, 360,  640, 360, ROT_0,   DP_COLOR_RGBA8888, DP_COLOR_YV12 },
	{ "q_down2x",  1280, 720,  640, 360, ROT_0,   DP_COLOR_RGBA8888, DP_COLOR_YV12 },
	{ "q_down32x", 1280, 720,   40,  22, ROT_0,   DP_COLOR_RGBA8888, DP_COLOR_YV12 },
	{ "q_up8x",     160,  90, 1280, 720, ROT_0,   DP_COLOR_RGBA8888, DP_COLOR_YV12 },
	{ "q_up64x",    160,  90, 1280*8, 720*8, ROT_0, DP_COLOR_RGBA8888, DP_COLOR_YV12 },
	{ "q_rot90",    640, 360,  360, 640, ROT_90,  DP_COLOR_RGBA8888, DP_COLOR_YV12 },
	{ "q_rot270",   640, 360,  360, 640, ROT_270, DP_COLOR_RGBA8888, DP_COLOR_YV12 },
	{ "q_huge_src",4096,4096, 1280, 720, ROT_0,   DP_COLOR_RGBA8888, DP_COLOR_YV12 },
	{ "q_huge_dst", 640, 360, 8192,4096, ROT_0,   DP_COLOR_RGBA8888, DP_COLOR_YV12 },
	{ "q_zero_dst", 640, 360,    0,   0, ROT_0,   DP_COLOR_RGBA8888, DP_COLOR_YV12 },
	{ "q_565_565",  640, 360,  640, 360, ROT_0,   DP_COLOR_RGB565,   DP_COLOR_RGB565 },
	{ "q_yv12_rgba",640, 360,  640, 360, ROT_0,   DP_COLOR_YV12,     DP_COLOR_RGBA8888 },
};

static void run_query(void) {
	// Both classes carry their own copy and the async one allows a taller
	// downscale, so they are asked the same questions side by side.
	for (unsigned i = 0; i < sizeof(qcases) / sizeof(qcases[0]); i++) {
		const struct QCase *q = &qcases[i];
		const char a = _ZN17DpAsyncBlitStream14queryHWSupportEjjjji13DP_COLOR_ENUMS0_(
			q->sw, q->sh, q->dw, q->dh, q->rot, q->sfmt, q->dfmt);
		const char b = _ZN12DpBlitStream14queryHWSupportEjjjji13DP_COLOR_ENUMS0_(
			q->sw, q->sh, q->dw, q->dh, q->rot, q->sfmt, q->dfmt);
		printf("%-15s async=%d sync=%d\n", q->name, (int)a, (int)b);
		fflush(stdout);
	}
}

static void dump_dst(const char *kase, int port, const void *va, unsigned size);

static void run_case(int ionFd, const struct Case *k) {
	struct Planes sp = planes_for(k->sfmt, k->sw, k->sh);
	const unsigned srcSize = total_of(&sp);
	int srcFd = -1;
	void *srcVA = NULL;
	unsigned srcHandle = 0;
	if (alloc_ion(ionFd, srcSize, &srcFd, &srcVA, &srcHandle) != 0) {
		printf("%-15s ALLOC-FAIL\n", k->name);
		return;
	}
	// No constant row and no constant column, so a blit that drops or
	// transposes an axis moves the checksum on its own.
	uint8_t *s = (uint8_t *)srcVA;
	for (unsigned i = 0; i < srcSize; i++)
		s[i] = (uint8_t)((i * 7u + (i / 977u) * 13u) & 0xff);

	const int frames = (k->frames > 0) ? k->frames : 1;
	// One destination buffer per ring slot per port, all filled with the same
	// sentinel so an untouched count means the same thing whichever slot the
	// last frame landed on.
	const int ring = (k->dstRing > 1) ? ((k->dstRing > DST_RING_MAX) ? DST_RING_MAX : k->dstRing) : 1;
	struct Planes dp[4];
	unsigned dstSize[4] = { 0, 0, 0, 0 };
	int dstFd[4][DST_RING_MAX];
	void *dstVA[4][DST_RING_MAX];
	unsigned dstHandle[4][DST_RING_MAX];
	for (int p = 0; p < 4; p++)
		for (int r = 0; r < DST_RING_MAX; r++) {
			dstFd[p][r] = -1; dstVA[p][r] = NULL; dstHandle[p][r] = 0;
		}
	for (int p = 0; p < k->nports; p++) {
		dp[p] = planes_for(k->port[p].dfmt, k->port[p].dw, k->port[p].dh);
		dstSize[p] = total_of(&dp[p]);
		for (int r = 0; r < ring; r++) {
			if (alloc_ion(ionFd, dstSize[p], &dstFd[p][r], &dstVA[p][r], &dstHandle[p][r]) != 0) {
				printf("%-15s ALLOC-FAIL port %d slot %d\n", k->name, p, r);
				return;
			}
			memset(dstVA[p][r], 0xa5, dstSize[p]);
		}
	}

	char obj[OBJ_BYTES];
	memset(obj, 0, sizeof(obj));
	_ZN17DpAsyncBlitStreamC1Ev(obj);
	const int su = _ZN17DpAsyncBlitStream7setUserEj(obj, k->user);

	// A cancelled job's release fence is threaded onto the job that follows,
	// so the cancel has to happen while the real job is still pending.
	uint32_t spareId = 0;
	int32_t spareFence = -1;
	if (k->cancelExtra)
		_ZN17DpAsyncBlitStream9createJobERjRi(obj, &spareId, &spareFence);

	int cj = 0, cx = 0;
	uint32_t jobId = 0;
	int32_t fence = -1;
  for (int f = 0; f < frames; f++) {
	// The composer's shape: several jobs created, only the last configured.
	for (int e = 0; e < k->extraJobsPerFrame; e++) {
		uint32_t xid = 0; int32_t xf = -1;
		_ZN17DpAsyncBlitStream9createJobERjRi(obj, &xid, &xf);
		if (xf >= 0) close(xf);
	}
	if (fence >= 0) { close(fence); fence = -1; }
	cj = _ZN17DpAsyncBlitStream9createJobERjRi(obj, &jobId, &fence);
	if (k->cancelExtra)
		cx = _ZN17DpAsyncBlitStream9cancelJobEj(obj, spareId);

	const int cb = _ZN17DpAsyncBlitStream14setConfigBeginEj(obj, jobId);
	int sb;
	if (k->srcSecure) {
		// A secure source goes in as its ion handle replicated across the
		// plane list, not as a share fd.
		void *h[3] = { (void *)(uintptr_t)srcHandle, (void *)(uintptr_t)srcHandle,
		               (void *)(uintptr_t)srcHandle };
		sb = _ZN17DpAsyncBlitStream12setSrcBufferEPPvPjji(obj, h, sp.size, (uint32_t)sp.n, -1);
	} else {
		sb = _ZN17DpAsyncBlitStream12setSrcBufferEiPjji(obj, srcFd, sp.size, (uint32_t)sp.n, -1);
	}
	const int sc = _ZN17DpAsyncBlitStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormat8DpSecureb(
		obj, k->sw, k->sh, sp.yPitch, sp.uvPitch, k->sfmt, 0, 0, k->srcSecure, 1);

	int db = 0, dc = 0, sk = 0, so = 0, pq = 0;
	for (int p = 0; p < k->nports; p++) {
		const struct Port *o = &k->port[p];
		const int slot = f % ring;
		if (k->dstSecure) {
			void *h[3] = { (void *)(uintptr_t)dstHandle[p][slot],
			               (void *)(uintptr_t)dstHandle[p][slot],
			               (void *)(uintptr_t)dstHandle[p][slot] };
			db |= _ZN17DpAsyncBlitStream12setDstBufferEiPPvPjji(obj, p, h, dp[p].size,
			                                                    (uint32_t)dp[p].n, -1);
		} else {
			db |= _ZN17DpAsyncBlitStream12setDstBufferEiiPjji(obj, p, dstFd[p][slot], dp[p].size,
			                                                  (uint32_t)dp[p].n, -1);
		}
		const int useCrop = (o->cropW != 0);
		struct DpRect srcCrop = { useCrop ? o->cropX : 0, 0, useCrop ? o->cropY : 0, 0,
		                          useCrop ? o->cropW : k->sw, useCrop ? o->cropH : k->sh };
		struct DpRect dstCrop = { 0, 0, 0, 0, o->dw, o->dh };
		sk |= _ZN17DpAsyncBlitStream10setSrcCropEi6DpRect(obj, p, &srcCrop);
		dc |= _ZN17DpAsyncBlitStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRect8DpSecureb(
			obj, p, o->dw, o->dh, dp[p].yPitch, dp[p].uvPitch, o->dfmt, 0, 0, &dstCrop,
			k->dstSecure, 0);
		so |= _ZN17DpAsyncBlitStream14setOrientationEij(obj, p, o->rot);
		if (k->pqScenario != 0) {
			struct DpPqParam param;
			memset(&param, 0, sizeof(param));
			param.scenario = (uint32_t)k->pqScenario;
			param.videoID = 0x1234;
			pq |= _ZN17DpAsyncBlitStream14setPQParameterEiRK9DpPqParam(obj, p, &param);
		}
	}

	const int ce = _ZN17DpAsyncBlitStream12setConfigEndEv(obj);
	printf("%-15s su=%d cj=%d cx=%d cb=%d sb=%d sc=%d db=%d dc=%d sk=%d so=%d pq=%d ce=%d ",
	       k->name, su, cj, cx, cb, sb, sc, db, dc, sk, so, pq, ce);
	fflush(stdout);

	const int inv = _ZN17DpAsyncBlitStream10invalidateEv(obj);
	if (f + 1 < frames) {
		if (fence >= 0) sync_wait(fence, 3000);
		printf("f%d:inv=%d ", f, inv);
		fflush(stdout);
		continue;
	}
	// The blit is asynchronous: the pixels are not there until the job's
	// fence signals, so a checksum taken before this is meaningless.
	const int fw = (fence >= 0) ? sync_wait(fence, 3000) : -2;

	printf("inv=%d fence=%d wait=%d |", inv, fence, fw);
	for (int p = 0; p < k->nports; p++) {
		// Every slot is summed, not just the one the last frame used: a frame
		// landing in the wrong buffer is invisible if only one is read.
		for (int r = 0; r < ring; r++) {
			const uint8_t *d = (const uint8_t *)dstVA[p][r];
			unsigned off = 0, untouched = 0;
			uint32_t sums[3] = { 0, 0, 0 };
			for (int pl = 0; pl < dp[p].n; pl++) {
				uint32_t sum = 0;
				for (unsigned i = 0; i < dp[p].size[pl]; i++) {
					uint8_t b = d[off + i];
					sum = sum * 31u + b;
					if (b == 0xa5) untouched++;
				}
				sums[pl] = sum;
				off += dp[p].size[pl];
			}
			if (ring > 1)
				printf(" P%d/%d:%d %08x %08x %08x u=%u/%u |",
				       p, r, dp[p].n, sums[0], sums[1], sums[2], untouched, dstSize[p]);
			else
				printf(" P%d:%d %08x %08x %08x u=%u/%u |",
				       p, dp[p].n, sums[0], sums[1], sums[2], untouched, dstSize[p]);
			dump_dst(k->name, p, dstVA[p][r], dstSize[p]);
		}
	}
	printf("\n");
	fflush(stdout);
  }

	if (fence >= 0) close(fence);
	if (spareFence >= 0) close(spareFence);
	_ZN17DpAsyncBlitStreamD1Ev(obj);
	ion_munmap(ionFd, srcVA, srcSize);
	close(srcFd);
	for (int p = 0; p < k->nports; p++) {
		for (int r = 0; r < ring; r++) {
			ion_munmap(ionFd, dstVA[p][r], dstSize[p]);
			close(dstFd[p][r]);
		}
	}
}

// Every open descriptor with its target, so a leak can be named rather than
// inferred from a climbing fence number. argv[6] opts in.
// Set from argv[7]; the tag is appended to each dump's name so a stock run and
// a reconstruction run land in different files.
static const char *g_dstDump = NULL;

static void dump_dst(const char *kase, int port, const void *va, unsigned size) {
	if (!g_dstDump) return;
	char path[192];
	snprintf(path, sizeof(path), "/data/local/tmp/dst_%s_p%d.%s.bin", kase, port, g_dstDump);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fwrite(va, 1, size, f);
	fclose(f);
}

static void dump_fds(const char *tag) {
	DIR *d = opendir("/proc/self/fd");
	if (!d) return;
	int n = 0;
	struct dirent *e;
	printf("FDDUMP %s:", tag);
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char path[64], tgt[256];
		snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
		ssize_t k = readlink(path, tgt, sizeof(tgt) - 1);
		if (k < 0) continue;
		tgt[k] = '\0';
		printf(" %s=%s", e->d_name, tgt);
		n++;
	}
	printf(" (total %d)\n", n);
	fflush(stdout);
	closedir(d);
}

int main(int argc, char **argv) {
	const int from = (argc > 1) ? atoi(argv[1]) : 0;
	const int count = (argc > 2) ? atoi(argv[2]) : -1;
	const int doQuery = (argc > 3) ? atoi(argv[3]) : 1;
	const int doUnreachable = (argc > 4) ? atoi(argv[4]) : 0;
	// Separate opt-in from doUnreachable: these do not merely fail, they wedge
	// the display and cost a reboot.
	const int doWedging = (argc > 5) ? atoi(argv[5]) : 0;
	const int doFdDump = (argc > 6) ? atoi(argv[6]) : 0;
	// argv[7]: write each destination to /data/local/tmp/dst_<case>_p<port>.bin
	// so the two arms can be diffed byte for byte instead of by checksum.
	g_dstDump = (argc > 7) ? argv[7] : NULL;

	int ionFd = mt_ion_open("dpasync_sweep");
	if (ionFd < 0) { fprintf(stderr, "mt_ion_open failed\n"); return 1; }

	int ncases = (int)(sizeof(cases) / sizeof(cases[0]));
	if (count >= 0 && from + count < ncases) ncases = from + count;
	for (int c = from; c < ncases; c++) {
		if (cases[c].wedges && !doWedging) {
			printf("%-15s SKIPPED (hangs and wedges CMDQ — reboot to clear)\n", cases[c].name);
			continue;
		}
		if (cases[c].unreachable && !doUnreachable) {
			printf("%-15s SKIPPED (no consumer reaches this)\n", cases[c].name);
			continue;
		}
		run_case(ionFd, &cases[c]);
		if (doFdDump) dump_fds(cases[c].name);
	}

	if (doQuery)
		run_query();
	return 0;
}
