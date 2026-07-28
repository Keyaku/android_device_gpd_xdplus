// vulkan.mt8173.so shim for GPD XD+ (DDK 1.9 PowerVR GX6250).
// The DDK 1.9 USC shader compiler recurses unboundedly deep on complex shaders
// and overflows normal-sized app thread stacks (SwanStation: SIGSEGV in
// libusc.so, frame 0xc8150 x256+). This shim forwards the whole hwvulkan HAL
// module to the real vendor blob but runs pipeline creation (where PowerVR
// compiles shaders) on a dedicated big-stack thread.
//
// On top of the big stack, two quality-of-life layers:
//  1. Persistent pipeline cache: with the overflow fixed, compiles complete but
//     can take minutes (the recursion is merely survivable, not fast). The shim
//     keeps its own VkPipelineCache primed from /data/vkshim/<pkg>.pcache,
//     substitutes it when the app passes VK_NULL_HANDLE, merges app-provided
//     caches into it after each batch, and persists it back — so only the first
//     run of a given shader set pays the compile cost.
//  2. Compile progress signaling: batches slower than SLOW_NS flag the compile
//     as user-visible and publish "<pkg>:<count>" through the momentary
//     sys.xdplus.vkcompile property. init.xdplus.rc relays that to the Settings
//     VkCompileReceiver (notification/toast per persist.sys.xdplus.vknotify).
//     Property sets from an untrusted app domain only work because this port
//     runs SELinux permissive.
//
// The shim cache is owned per VkDevice and torn down from the vkCreateDevice /
// vkDestroyDevice hooks, and every pipeline-creation batch holds cache_mu for
// its whole duration. Both are load-bearing, not tidiness — see ensure_disk_cache
// and cache_mu.
//
// A "fresh-device warm-up" (sacrificial render passes at vkCreateDevice, plus
// hooks on vkCreateSwapchainKHR / vkAcquireImageANDROID / native-buffer
// vkCreateImage / vkDestroyDevice) lived here until 2026-07-20 and was removed:
// the defect it targeted was an artifact of a missing barrier in the vkreinit
// harness, and the warm-up only ever "worked" because its vkQueueWaitIdle
// serialised the queue. Do not reintroduce it without a repro from a
// correctly-synchronised probe.
//
// Install: replaces /system/lib{,64}/vulkan.mt8173.so (previously symlinks to
// the vendor blob). The Android Vulkan loader finds this first on the legacy
// single-namespace search path; the real blob stays at /vendor/lib{,64}/hw/.
#include <vulkan/vulkan.h>
#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>
#include <pthread.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define TAG "vkshim"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define COMPILE_STACK_SIZE (64u * 1024u * 1024u)
#define CACHE_DIR "/data/vkshim"
#define PROGRESS_PROP "sys.xdplus.vkcompile"
// A single batch slower than this marks the whole session as worth notifying.
#define SLOW_NS (2ull * 1000000000ull)

static hwvulkan_module_t *real_module;
static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr real_gdpa;
static PFN_vkCreateGraphicsPipelines real_cgp;
static PFN_vkCreateComputePipelines real_ccp;
static PFN_vkCreatePipelineCache real_cpc;
static PFN_vkMergePipelineCaches real_mpc;
static PFN_vkGetPipelineCacheData real_gpcd;
static PFN_vkDestroyPipelineCache real_dpc;
static PFN_vkDestroyDevice real_dd;
static PFN_vkCreateDevice real_cd;
static PFN_vkCmdBlitImage real_cbi;
static PFN_vkCreateSampler real_cs;

// ---- persistent pipeline cache --------------------------------------------

// cache_mu covers disk_cache and everything derived from it, and is held across
// the whole pipeline-creation batch — not just the bookkeeping around it.
// vkCreateGraphicsPipelines/vkCreateComputePipelines/vkMergePipelineCaches/
// vkGetPipelineCacheData all take pipelineCache as an externally-synchronised
// parameter, and once the shim substitutes one cache for every caller, two app
// threads compiling at once are two threads mutating one cache object.
static pthread_mutex_t cache_mu = PTHREAD_MUTEX_INITIALIZER;
static VkDevice cache_dev;
static VkPipelineCache disk_cache;
static char cache_path[192];
static size_t last_saved_size;
static unsigned compiled_count;
static int slow_session;

static const char *pkg_name(void) {
	static char pkg[96];
	if (!pkg[0]) {
		int fd = open("/proc/self/cmdline", O_RDONLY);
		ssize_t n = fd >= 0 ? read(fd, pkg, sizeof(pkg) - 1) : 0;
		if (fd >= 0) close(fd);
		if (n <= 0) strcpy(pkg, "unknown");
		pkg[sizeof(pkg) - 1] = '\0';
		char *c = strchr(pkg, ':');  // strip :remote_process suffixes
		if (c) *c = '\0';
	}
	return pkg;
}

static void resolve_cache_fns(VkDevice dev) {
	if (!real_cpc) real_cpc = (PFN_vkCreatePipelineCache)real_gdpa(dev, "vkCreatePipelineCache");
	if (!real_mpc) real_mpc = (PFN_vkMergePipelineCaches)real_gdpa(dev, "vkMergePipelineCaches");
	if (!real_gpcd) real_gpcd = (PFN_vkGetPipelineCacheData)real_gdpa(dev, "vkGetPipelineCacheData");
	if (!real_dpc) real_dpc = (PFN_vkDestroyPipelineCache)real_gdpa(dev, "vkDestroyPipelineCache");
}

// Called with cache_mu held. Creates the shim cache for this device, primed
// from the on-disk blob if one exists (the driver validates the header UUID
// itself and falls back to empty on mismatch).
//
// The device is tracked by the vkCreateDevice/vkDestroyDevice hooks below, NOT
// by comparing handles here: the blob hands out the same VkDevice address again
// after a destroy/recreate, so a handle compare cannot tell "same device" from
// "new device at the old address" and would keep a cache belonging to the dead
// one. That is exactly what killed RetroArch on a renderer restart —
// vkMergePipelineCaches dereferenced the freed cache object.
static VkPipelineCache ensure_disk_cache(VkDevice dev) {
	if (cache_dev == dev && disk_cache != VK_NULL_HANDLE) return disk_cache;
	resolve_cache_fns(dev);
	if (!real_cpc || !real_gpcd) return VK_NULL_HANDLE;

	snprintf(cache_path, sizeof(cache_path), CACHE_DIR "/%s.pcache", pkg_name());
	void *initial = NULL;
	size_t initial_size = 0;
	int fd = open(cache_path, O_RDONLY);
	if (fd >= 0) {
		struct stat st;
		if (fstat(fd, &st) == 0 && st.st_size > 0) {
			initial = malloc(st.st_size);
			if (initial && read(fd, initial, st.st_size) == st.st_size)
				initial_size = st.st_size;
		}
		close(fd);
	}

	VkPipelineCacheCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
		.initialDataSize = initial_size,
		.pInitialData = initial_size ? initial : NULL,
	};
	VkPipelineCache pc = VK_NULL_HANDLE;
	VkResult r = real_cpc(dev, &ci, NULL, &pc);
	if (r != VK_SUCCESS && initial_size) {
		// Corrupt/stale blob the driver refused outright — retry empty.
		ci.initialDataSize = 0;
		ci.pInitialData = NULL;
		r = real_cpc(dev, &ci, NULL, &pc);
	}
	free(initial);
	if (r != VK_SUCCESS) return VK_NULL_HANDLE;
	cache_dev = dev;
	disk_cache = pc;
	last_saved_size = initial_size;
	LOGI("pipeline cache ready (%s, primed %zu bytes)", cache_path, initial_size);
	return pc;
}

// Called with cache_mu held.
static void save_disk_cache(VkDevice dev) {
	if (disk_cache == VK_NULL_HANDLE || !real_gpcd || !cache_path[0]) return;
	size_t size = 0;
	if (real_gpcd(dev, disk_cache, &size, NULL) != VK_SUCCESS || size == 0) return;
	if (size == last_saved_size) return;
	void *data = malloc(size);
	if (!data) return;
	if (real_gpcd(dev, disk_cache, &size, data) == VK_SUCCESS) {
		char tmp[sizeof(cache_path) + 4];
		snprintf(tmp, sizeof(tmp), "%s.tmp", cache_path);
		int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			if (write(fd, data, size) == (ssize_t)size && !close(fd)) {
				if (rename(tmp, cache_path) == 0) {
					last_saved_size = size;
					LOGI("pipeline cache saved (%zu bytes)", size);
				}
			} else {
				close(fd);
				unlink(tmp);
			}
		}
	}
	free(data);
}

static void publish_progress(void) {
	char v[92];
	snprintf(v, sizeof(v), "%s:%u", pkg_name(), compiled_count);
	__system_property_set(PROGRESS_PROP, v);
}

static unsigned long long now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// ---- big-stack trampoline -------------------------------------------------

struct gp_args {
	VkDevice dev; VkPipelineCache cache; uint32_t n;
	const VkGraphicsPipelineCreateInfo *ci;
	const VkAllocationCallbacks *ac; VkPipeline *out; VkResult r;
};
struct cp_args {
	VkDevice dev; VkPipelineCache cache; uint32_t n;
	const VkComputePipelineCreateInfo *ci;
	const VkAllocationCallbacks *ac; VkPipeline *out; VkResult r;
};

static void *gp_thread(void *p) {
	struct gp_args *a = p;
	a->r = real_cgp(a->dev, a->cache, a->n, a->ci, a->ac, a->out);
	return NULL;
}
static void *cp_thread(void *p) {
	struct cp_args *a = p;
	a->r = real_ccp(a->dev, a->cache, a->n, a->ci, a->ac, a->out);
	return NULL;
}

static VkResult run_big_stack(void *(*fn)(void *), void *args, VkResult *slot) {
	pthread_attr_t attr;
	pthread_t t;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, COMPILE_STACK_SIZE);
	int err = pthread_create(&t, &attr, fn, args);
	pthread_attr_destroy(&attr);
	if (err) {
		LOGE("pthread_create failed (%d), calling on caller stack", err);
		fn(args);
	} else {
		pthread_join(t, NULL);
	}
	return *slot;
}

// Wraps one pipeline-creation batch: cache substitution/merge + persistence +
// progress accounting around the big-stack call.
static VkResult compile_batch(void *(*fn)(void *), void *args, VkResult *slot,
	VkDevice dev, VkPipelineCache *cache_slot, uint32_t n) {
	pthread_mutex_lock(&cache_mu);
	VkPipelineCache shim_cache = ensure_disk_cache(dev);
	VkPipelineCache app_cache = *cache_slot;
	if (app_cache == VK_NULL_HANDLE && shim_cache != VK_NULL_HANDLE)
		*cache_slot = shim_cache;

	unsigned long long t0 = now_ns();
	VkResult r = run_big_stack(fn, args, slot);
	unsigned long long dt = now_ns() - t0;

	compiled_count += n;
	if (dt > SLOW_NS) slow_session = 1;
	if (slow_session) publish_progress();
	if (shim_cache != VK_NULL_HANDLE) {
		if (app_cache != VK_NULL_HANDLE && real_mpc)
			real_mpc(dev, shim_cache, 1, &app_cache);
		save_disk_cache(dev);
	}
	pthread_mutex_unlock(&cache_mu);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateGraphicsPipelines(
	VkDevice dev, VkPipelineCache cache, uint32_t n,
	const VkGraphicsPipelineCreateInfo *ci,
	const VkAllocationCallbacks *ac, VkPipeline *out) {
	struct gp_args a = {dev, cache, n, ci, ac, out, VK_ERROR_INITIALIZATION_FAILED};
	return compile_batch(gp_thread, &a, &a.r, dev, &a.cache, n);
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateComputePipelines(
	VkDevice dev, VkPipelineCache cache, uint32_t n,
	const VkComputePipelineCreateInfo *ci,
	const VkAllocationCallbacks *ac, VkPipeline *out) {
	struct cp_args a = {dev, cache, n, ci, ac, out, VK_ERROR_INITIALIZATION_FAILED};
	return compile_batch(cp_thread, &a, &a.r, dev, &a.cache, n);
}

// ---- broken vkCmdBlitImage, and the mip chains built on it -----------------

// This blob's vkCmdBlitImage is broken for anything that resizes, in a
// different way per filter. tools/vkmip fills level 0 with a blue top-left
// quadrant on a red field, generates a chain, and reads levels back:
//
//   VK_FILTER_LINEAR   the requested levels stay empty and the *base* level
//                      comes back holding the downscaled result — the blit
//                      scales correctly but ignores dstSubresource.mipLevel,
//                      so it overwrites the image it was reading from
//   VK_FILTER_NEAREST  the requested level is written, but with the source
//                      texels copied 1:1 — the scale factor is ignored
//
// No error in either case, and the format advertises BLIT_SRC and BLIT_DST. So
// a blit is trustworthy only when source and destination extents are equal, and
// then only with NEAREST.
//
// Consequence: no mipmap chain generated the standard way (blit level n-1 into
// level n) is usable here, and generating one with LINEAR actively destroys the
// texture. That is the RetroArch glui menu: its icons are loaded mipmapped, so
// the sampler read levels that were never written (icons invisible), and once
// they were made to draw, level 0 itself carried a half-size copy of the icon
// left behind by the chain (each icon drawn twice, one small, one full size).
//
// So: never hand the driver a scaling blit, and never sample above level 0.
// Both are toggleable:
//  - debug.xdplus.vkblitnearest=0 stops the 1:1 filter downgrade.
//  - debug.xdplus.vkmiplod=0 stops clamping samplers to level 0.
//  - debug.xdplus.vkblitskip=0 lets scaling blits through to the driver.
static int blit_downgrade = -1;
static int miplod_clamp = -1;
static int blit_skip = -1;
static int warned_scaling_blit;

static int prop_on(const char *name, int *cache) {
	if (*cache < 0) {
		char v[PROP_VALUE_MAX] = {0};
		__system_property_get(name, v);
		*cache = v[0] == '0' ? 0 : 1;
	}
	return *cache;
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdBlitImage(
	VkCommandBuffer cb, VkImage src, VkImageLayout src_layout,
	VkImage dst, VkImageLayout dst_layout, uint32_t n,
	const VkImageBlit *regions, VkFilter filter) {
	int scales = 0;
	for (uint32_t i = 0; i < n; i++) {
		const VkOffset3D *s = regions[i].srcOffsets, *d = regions[i].dstOffsets;
		if (s[1].x - s[0].x != d[1].x - d[0].x ||
			s[1].y - s[0].y != d[1].y - d[0].y ||
			s[1].z - s[0].z != d[1].z - d[0].z) { scales = 1; break; }
	}
	if (scales && prop_on("debug.xdplus.vkblitskip", &blit_skip)) {
		// Dropping it leaves the destination level unwritten, which the sampler
		// clamp below makes harmless. Letting it through under LINEAR would
		// corrupt the source image instead — strictly worse than doing nothing.
		if (!warned_scaling_blit) {
			warned_scaling_blit = 1;
			LOGE("dropping scaling vkCmdBlitImage: this driver writes it to the wrong mip level");
		}
		return;
	}
	// A 1:1 blit is correct under NEAREST and does nothing under LINEAR, so
	// the downgrade is a straight win.
	if (!scales && filter == VK_FILTER_LINEAR &&
		prop_on("debug.xdplus.vkblitnearest", &blit_downgrade))
		filter = VK_FILTER_NEAREST;
	real_cbi(cb, src, src_layout, dst, dst_layout, n, regions, filter);
}

// Since no generated mip chain can be trusted, keep samplers on level 0. The
// cost is point-sampled minification instead of a mip pyramid; the alternative
// is sampling levels that hold a magnified corner of the image, or nothing.
static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateSampler(
	VkDevice dev, const VkSamplerCreateInfo *ci,
	const VkAllocationCallbacks *ac, VkSampler *out) {
	if (ci && ci->maxLod > 0.0f && prop_on("debug.xdplus.vkmiplod", &miplod_clamp)) {
		VkSamplerCreateInfo fixed = *ci;
		fixed.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		fixed.minLod = 0.0f;
		fixed.maxLod = 0.0f;
		return real_cs(dev, &fixed, ac, out);
	}
	return real_cs(dev, ci, ac, out);
}

// ---- device lifetime -------------------------------------------------------

// Called with cache_mu held. `dev` is the device the cache belongs to, or NULL
// when it is already dead and the handle must not be touched again.
static void drop_disk_cache(VkDevice dev) {
	if (disk_cache != VK_NULL_HANDLE && dev) {
		save_disk_cache(dev);
		if (real_dpc) real_dpc(dev, disk_cache, NULL);
	}
	disk_cache = VK_NULL_HANDLE;
	cache_dev = NULL;
	last_saved_size = 0;
	cache_path[0] = '\0';
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyDevice(
	VkDevice dev, const VkAllocationCallbacks *ac) {
	pthread_mutex_lock(&cache_mu);
	if (cache_dev == dev) drop_disk_cache(dev);
	pthread_mutex_unlock(&cache_mu);
	real_dd(dev, ac);
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateDevice(
	VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
	const VkAllocationCallbacks *ac, VkDevice *out) {
	VkResult r = real_cd(pd, ci, ac, out);
	if (r != VK_SUCCESS) return r;
	// Belt and braces for a destroy we never saw: the old cache cannot be
	// destroyed (its device may or may not still exist, and the handle may
	// already be dangling), so drop the reference without touching it.
	pthread_mutex_lock(&cache_mu);
	if (disk_cache != VK_NULL_HANDLE) {
		LOGE("device created with a live cache from %p — dropping it unfreed", cache_dev);
		drop_disk_cache(NULL);
	}
	pthread_mutex_unlock(&cache_mu);
	return r;
}

// ---- proc-addr interposition ----------------------------------------------

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_GetDeviceProcAddr(
	VkDevice dev, const char *name) {
	if (!strcmp(name, "vkCreateGraphicsPipelines")) {
		if (!real_cgp) real_cgp = (PFN_vkCreateGraphicsPipelines)real_gdpa(dev, name);
		return real_cgp ? (PFN_vkVoidFunction)shim_CreateGraphicsPipelines : NULL;
	}
	if (!strcmp(name, "vkCreateComputePipelines")) {
		if (!real_ccp) real_ccp = (PFN_vkCreateComputePipelines)real_gdpa(dev, name);
		return real_ccp ? (PFN_vkVoidFunction)shim_CreateComputePipelines : NULL;
	}
	if (!strcmp(name, "vkDestroyDevice")) {
		if (!real_dd) real_dd = (PFN_vkDestroyDevice)real_gdpa(dev, name);
		return real_dd ? (PFN_vkVoidFunction)shim_DestroyDevice : NULL;
	}
	if (!strcmp(name, "vkCmdBlitImage")) {
		if (!real_cbi) real_cbi = (PFN_vkCmdBlitImage)real_gdpa(dev, name);
		return real_cbi ? (PFN_vkVoidFunction)shim_CmdBlitImage : NULL;
	}
	if (!strcmp(name, "vkCreateSampler")) {
		if (!real_cs) real_cs = (PFN_vkCreateSampler)real_gdpa(dev, name);
		return real_cs ? (PFN_vkVoidFunction)shim_CreateSampler : NULL;
	}
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)shim_GetDeviceProcAddr;
	return real_gdpa(dev, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_GetInstanceProcAddr(
	VkInstance inst, const char *name) {
	if (!strcmp(name, "vkGetDeviceProcAddr")) {
		if (!real_gdpa) real_gdpa = (PFN_vkGetDeviceProcAddr)real_gipa(inst, name);
		return real_gdpa ? (PFN_vkVoidFunction)shim_GetDeviceProcAddr : NULL;
	}
	if (!strcmp(name, "vkCreateGraphicsPipelines")) {
		if (!real_cgp) real_cgp = (PFN_vkCreateGraphicsPipelines)real_gipa(inst, name);
		return real_cgp ? (PFN_vkVoidFunction)shim_CreateGraphicsPipelines : NULL;
	}
	if (!strcmp(name, "vkCreateComputePipelines")) {
		if (!real_ccp) real_ccp = (PFN_vkCreateComputePipelines)real_gipa(inst, name);
		return real_ccp ? (PFN_vkVoidFunction)shim_CreateComputePipelines : NULL;
	}
	// Device lifetime: the loader takes vkCreateDevice from the instance and
	// vkDestroyDevice from the device, so both tables need a hook.
	if (!strcmp(name, "vkCreateDevice")) {
		if (!real_cd) real_cd = (PFN_vkCreateDevice)real_gipa(inst, name);
		return real_cd ? (PFN_vkVoidFunction)shim_CreateDevice : NULL;
	}
	if (!strcmp(name, "vkDestroyDevice")) {
		if (!real_dd) real_dd = (PFN_vkDestroyDevice)real_gipa(inst, name);
		return real_dd ? (PFN_vkVoidFunction)shim_DestroyDevice : NULL;
	}
	return real_gipa(inst, name);
}

// ---- hwvulkan HAL module --------------------------------------------------

static int load_real(void) {
	if (real_module) return 0;
	// Prototype installs bind-mounted the shim over the vendor blob path, so a
	// stashed copy is probed first; the tree install leaves the vendor path real.
#ifdef __LP64__
	const char *paths[] = {"/data/local/tmp/vulkan.mt8173.real.so",
		"/vendor/lib64/hw/vulkan.mt8173.so"};
#else
	const char *paths[] = {"/data/local/tmp/vulkan.mt8173.real32.so",
		"/vendor/lib/hw/vulkan.mt8173.so"};
#endif
	const char *path = paths[0];
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) { path = paths[1]; h = dlopen(path, RTLD_NOW | RTLD_LOCAL); }
	if (!h) { LOGE("dlopen %s failed: %s", path, dlerror()); return -1; }
	real_module = (hwvulkan_module_t *)dlsym(h, HAL_MODULE_INFO_SYM_AS_STR);
	if (!real_module) { LOGE("no HMI in real blob"); return -1; }
	return 0;
}

static hwvulkan_device_t shim_device;

static int shim_close(struct hw_device_t *dev) {
	(void)dev;
	return 0;
}

static int shim_open(const struct hw_module_t *module, const char *id,
	struct hw_device_t **device) {
	(void)module;
	if (load_real()) return -1;
	hw_device_t *real_dev;
	int err = real_module->common.methods->open(&real_module->common, id, &real_dev);
	if (err) { LOGE("real open(%s) failed: %d", id, err); return err; }
	hwvulkan_device_t *rd = (hwvulkan_device_t *)real_dev;
	real_gipa = rd->GetInstanceProcAddr;
	shim_device = *rd;
	shim_device.common.close = shim_close;
	shim_device.GetInstanceProcAddr = shim_GetInstanceProcAddr;
	*device = &shim_device.common;
	LOGI("vkshim active: pipeline creation on %u MB stack", COMPILE_STACK_SIZE >> 20);
	return 0;
}

static struct hw_module_methods_t shim_methods = { .open = shim_open };

__attribute__((visibility("default")))
hwvulkan_module_t HAL_MODULE_INFO_SYM = {
	.common = {
		.tag = HARDWARE_MODULE_TAG,
		.module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
		.hal_api_version = HARDWARE_HAL_API_VERSION,
		.id = HWVULKAN_HARDWARE_MODULE_ID,
		.name = "GPD XD+ Vulkan big-stack shim",
		.author = "xdplus port",
		.methods = &shim_methods,
	},
};
