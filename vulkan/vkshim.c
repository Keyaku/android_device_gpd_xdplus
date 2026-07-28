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

// ---- persistent pipeline cache --------------------------------------------

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
}

// Called with cache_mu held. Creates the shim cache for this device, primed
// from the on-disk blob if one exists (the driver validates the header UUID
// itself and falls back to empty on mismatch).
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
	pthread_mutex_unlock(&cache_mu);

	unsigned long long t0 = now_ns();
	VkResult r = run_big_stack(fn, args, slot);
	unsigned long long dt = now_ns() - t0;

	pthread_mutex_lock(&cache_mu);
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
