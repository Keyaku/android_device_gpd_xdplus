/*
 * O-era libui symbols for the prebuilt MTK OMX codec blobs
 * (libMtkOmxVdecEx.so, libMtkOmxVenc.so). Without these, MtkOmxCore's dlopen
 * of the per-codec libs fails and every OMX.MTK.VIDEO.* component silently
 * drops out of MediaCodecList — apps that require a hardware decoder
 * (Moonlight, etc.) then see no H.264/HEVC support at all.
 *
 * 1. GraphicBufferMapper::lock(buffer_handle_t, uint32_t, const Rect&, void**)
 *    R replaced the 4-arg export with a 6-arg one taking optional
 *    outBytesPerPixel/outBytesPerStride. The header still defaults the two
 *    extra args, so calling lock() here emits the 6-arg symbol; this wrapper
 *    only restores the old 4-arg mangled name.
 *
 * 2. Fence::~Fence()
 *    R declares it `= default` (inline, no export). Layout is unchanged since
 *    O: LightRefBase refcount (int32) followed by the fd (unique_fd = int).
 *    The default dtor's only work is destroying the unique_fd. The fd was
 *    registered with fdsan by R's (still-exported) Fence(int) ctor via
 *    unique_fd, so it must be closed with the matching owner tag — a raw
 *    close() aborts the process ('fdsan: ... actually owned by unique_fd',
 *    seen live as a media.codec SIGABRT mid-decode).
 *
 * Defined with C linkage under the exact mangled names (same pattern as
 * shim_wifi_ipcthreadstate.cpp); `this` arrives as the first argument.
 */

#include <stdint.h>
#include <unistd.h>

#include <android/fdsan.h>

#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>

using android::GraphicBufferMapper;
using android::Rect;
using android::status_t;

extern "C" status_t _ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPv(
		GraphicBufferMapper* thiz, buffer_handle_t handle, uint32_t usage,
		const Rect& bounds, void** vaddr) {
	return thiz->lock(handle, usage, bounds, vaddr);
}

struct FenceLayout {
	int32_t refcount;  // LightRefBase<Fence>::mCount
	int fd;            // base::unique_fd mFenceFd
};

// ARM32 C++ ABI (AAPCS): constructors and destructors return `this` in r0.
// The MTK blob's codegen relies on it (MtkOmxVdec::WaitFence feeds the dtor's
// return value straight into the following call) — returning void leaves
// garbage in r0 and crashed the decoder with free(0xffffffff). Declare the
// shims to return the object pointer.
extern "C" void* _ZN7android5FenceD1Ev(FenceLayout* thiz) {
	// Guard the caller's trailing free()/operator delete. WaitFence (and
	// the HEVC decode path) feed this dtor's return value straight into a
	// delete/free. If the "Fence" was never a real heap object — an
	// uninitialised −1 handle, seen as the HEVC SIGABRT "frees uninit −1 fence"
	// / Scudo "misaligned pointer when deallocating 0xffffffff" — dereferencing
	// or returning it aborts the codec. Return nullptr so the following free is
	// a well-defined no-op. A genuine heap Fence is never null/−1, so the H.264
	// path (valid r5 from operator new) is unchanged.
	if (thiz == nullptr || reinterpret_cast<uintptr_t>(thiz) == UINTPTR_MAX) {
		return nullptr;
	}
	if (thiz->fd >= 0) {
		// Mirror unique_fd::~unique_fd(): the tag is derived from the
		// unique_fd member's own address.
		uint64_t tag = android_fdsan_create_owner_tag(
				ANDROID_FDSAN_OWNER_TYPE_UNIQUE_FD,
				static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&thiz->fd)));
		android_fdsan_close_with_tag(thiz->fd, tag);
		thiz->fd = -1;
	}
	return thiz;
}

extern "C" void* _ZN7android5FenceD2Ev(FenceLayout* thiz) {
	return _ZN7android5FenceD1Ev(thiz);
}
