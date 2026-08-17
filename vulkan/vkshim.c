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
//     keeps its own VkPipelineCache primed from the calling app's own cache
//     directory (/data/user/<user>/<pkg>/cache/vkshim.pcache),
//     substitutes it when the app passes VK_NULL_HANDLE, merges app-provided
//     caches into it after each batch, and persists it back — so only the first
//     run of a given shader set pays the compile cost.
//  2. Compile progress signaling: batches slower than SLOW_NS flag the compile
//     as user-visible and publish "<pkg>:<count>" through the momentary
//     sys.xdplus.vkcompile property. init.xdplus.rc relays that to the Settings
//     VkCompileReceiver (notification/toast per persist.sys.xdplus.vknotify).
//     Property sets from an untrusted app domain only work because this port
//     runs SELinux permissive.
//  3. Driver identity: the 1.0.49 blob predates VK_KHR_driver_properties and
//     the external_*_capabilities trio, so DevCheck-class apps saw "unknown"
//     driver, 0.0.0.0 conformance and all-zero UUIDs. The shim advertises
//     those extensions and answers them from 1.0 entry points — real DDK
//     build tag, blob build-id as driverUUID. See the identity section below.
//  4. The 1.1-promoted memory extensions the blob predates but which cost
//     nothing to implement over its 1.0 entry points:
//     VK_KHR_get_memory_requirements2, VK_KHR_bind_memory2,
//     VK_KHR_dedicated_allocation and VK_KHR_maintenance3 — the set
//     vk_mem_alloc-class allocators probe for. Core 1.0 itself was always
//     fully supported; these close the gap apps actually test. See the
//     memory-query section for what is deliberately NOT advertised.
//
// ⚠️ Anything the shim advertises must be stripped from vkCreateDevice's
// ppEnabledExtensionNames — the blob fails the whole call on the first name it
// does not know, so advertising without filtering costs an app its device.
// shim_owns_ext + the filter in shim_CreateDevice are that contract.
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
#include <stddef.h>
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
#define CACHE_FILE "vkshim.pcache"
#define CACHE_GEN_FILE "vkshim.gen"
// Bumped by the Settings menu's "clear shader caches" action. Each process
// compares it against the generation stamped beside its own cache and deletes
// the cache when they differ -- the only way to clear a cache that lives inside
// an app sandbox, since SELinux lets nothing but the app itself and installd
// unlink app_data_file.
#define CACHE_GEN_PROP "persist.sys.xdplus.vkcachegen"
#define PROGRESS_PROP "sys.xdplus.vkcompile"
// A single batch slower than this marks the whole session as worth notifying.
#define SLOW_NS (2ull * 1000000000ull)
// Floor on how often the cache is flushed to disk mid-session; see save_disk_cache.
#define SAVE_MIN_INTERVAL_NS (30ull * 1000000000ull)

// Hard ceiling on what this shim may leave on /data. The cache only ever grows
// — a Vulkan pipeline cache blob is opaque, so there is no way to evict one
// entry from it — and one RetroArch session took it from 53 MB to 80 MB with no
// sign of converging. This device has ~25 GB of /data and no way to move app
// data to the microSD, so "grows forever" is not a shipping option.
//
// The per-app limit is in MB, read from persist.sys.xdplus.vkcachemax (what the
// GPD XD+ Settings menu writes) with debug.xdplus.vkcachemax as a
// non-persistent override for A/B testing without touching the user's setting.
//
// There is no whole-directory budget any more, and none is needed: each cache
// lives inside its own app's sandbox, so it is counted against that app's
// storage and goes away with the app's data like any other cache file. The
// shared /data/vkshim directory it used to live in could not survive SELinux
// enforcement in either direction — an app may not create or unlink files
// outside its sandbox types (app_neverallows.te), and a root daemon may not
// write or unlink system_data_file (domain.te), so both the writes and the
// budget prune would have been denied.
//
// Over the per-package limit the file is frozen at its last good size rather
// than reset: the alternative is throwing away every compiled pipeline and
// making the user sit through the whole USC compile again, periodically and
// forever. Frozen keeps everything already earned and only forfeits pipelines
// found after the cap — and on this hardware the set converges, so what is
// forfeited is the long tail, not the common path.
#define CACHE_MAX_MB 64u

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
static PFN_vkCmdBeginRenderPass real_cbrp;
static PFN_vkCmdEndRenderPass real_cerp;
static PFN_vkCmdNextSubpass real_cns;
static PFN_vkCmdDraw real_cdraw;
static PFN_vkCmdDrawIndexed real_cdrawi;
static PFN_vkCmdDrawIndirect real_cdrawind;
static PFN_vkCmdDrawIndexedIndirect real_cdrawiind;
static PFN_vkCmdClearAttachments real_cca;
static PFN_vkCreateImage real_ci;
static PFN_vkDestroyImage real_di;
static PFN_vkAllocateMemory real_am;
static PFN_vkFreeMemory real_fm;
static PFN_vkGetImageMemoryRequirements real_gimr;
static PFN_vkGetBufferMemoryRequirements real_gbmr;
static PFN_vkGetImageSparseMemoryRequirements real_gismr;
static PFN_vkBindImageMemory real_bim;
static PFN_vkBindBufferMemory real_bbm;
static PFN_vkCmdCopyImage real_cci;
static PFN_vkCmdPipelineBarrier real_cpb;
static PFN_vkBeginCommandBuffer real_bcb;
static PFN_vkResetCommandBuffer real_rcb;
static PFN_vkFreeCommandBuffers real_fcb;
static PFN_vkCreateCommandPool real_ccpool;
static PFN_vkResetCommandPool real_rcpool;
static PFN_vkDestroyCommandPool real_dcpool;
static PFN_vkAllocateCommandBuffers real_acb;
static PFN_vkGetPhysicalDeviceMemoryProperties real_gpdmp;
static PFN_vkCreateInstance real_cinst;
static PFN_vkEnumerateDeviceExtensionProperties real_edep;
static PFN_vkGetPhysicalDeviceProperties real_gpdp;
static PFN_vkGetPhysicalDeviceFeatures real_gpdf;
static PFN_vkGetPhysicalDeviceFormatProperties real_gpdfp;
static PFN_vkGetPhysicalDeviceImageFormatProperties real_gpdifp;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties real_gpdqfp;
static PFN_vkGetPhysicalDeviceSparseImageFormatProperties real_gpdsifp;
// ⚠️ Single-valued on purpose, and left that way after the per-device cache
// work: these are only ever used to resolve entry points and to read physical
// device properties, both of which are identical for every device on this
// hardware (one GPU, one instance in every app seen). Keying them per device
// would add lookups on hot paths and buy nothing. shim_dev is the exception
// that mattered and it is no longer used as a fallback — see cb_to_device.
static VkInstance shim_instance;
static VkPhysicalDeviceMemoryProperties mem_props;
static int mem_props_loaded;
static VkDevice shim_dev;

// ---- live image metadata table --------------------------------------------

// vkCmdBlitImage does not carry image formats or extents, but the emulation route
// needs them to build correctly-sized scratch images. Track the subset we need
// from vkCreateImage and drop it on vkDestroyImage.
#define IMG_TABLE_MAX 1024
struct img_info {
	VkImage img;
	VkFormat format;
	VkExtent3D extent;
	uint32_t array_layers;
	uint32_t aspect; // guess: color unless depth/stencil format
	int used;
};
static struct img_info img_table[IMG_TABLE_MAX];
static pthread_mutex_t img_mu = PTHREAD_MUTEX_INITIALIZER;

// Copies the entry out rather than returning a pointer into the table. The
// table is a fixed array so a slot pointer cannot dangle, but it can be
// recycled: a concurrent vkDestroyImage + vkCreateImage frees the slot and
// refills it with a different image, and a caller still holding the pointer
// would then size its scratch images from the wrong image's format and extent.
static int find_img(VkImage img, struct img_info *out) {
	int found = 0;
	pthread_mutex_lock(&img_mu);
	for (int i = 0; i < IMG_TABLE_MAX; i++) {
		if (img_table[i].used && img_table[i].img == img) {
			*out = img_table[i];
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&img_mu);
	return found;
}

static void remember_img(VkImage img, VkFormat format, VkExtent3D extent,
	uint32_t array_layers) {
	pthread_mutex_lock(&img_mu);
	int slot = -1;
	for (int i = 0; i < IMG_TABLE_MAX; i++) {
		if (!img_table[i].used) { slot = i; break; }
	}
	if (slot < 0) {
		pthread_mutex_unlock(&img_mu);
		LOGE("image metadata table full");
		return;
	}
	img_table[slot].img = img;
	img_table[slot].format = format;
	img_table[slot].extent = extent;
	img_table[slot].array_layers = array_layers;
	img_table[slot].aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	img_table[slot].used = 1;
	pthread_mutex_unlock(&img_mu);
}

static void forget_img(VkImage img) {
	pthread_mutex_lock(&img_mu);
	for (int i = 0; i < IMG_TABLE_MAX; i++) {
		if (img_table[i].used && img_table[i].img == img) {
			img_table[i].used = 0;
			break;
		}
	}
	pthread_mutex_unlock(&img_mu);
}

// ---- persistent pipeline cache --------------------------------------------

// cache_mu covers disk_cache and everything derived from it. It is held for the
// short bookkeeping either side of a batch, never across the compile itself.
//
// pipelineCache is an externally-synchronised parameter on
// vkCreateGraphicsPipelines/vkCreateComputePipelines/vkMergePipelineCaches/
// vkGetPipelineCacheData alike, and substituting one cache for every caller
// manufactures exactly the sharing that rule forbids — so only one batch at a
// time may hold the shared cache (master_in_use). A batch that arrives while it
// is taken does NOT wait: it compiles against a private scratch cache and hands
// that back to be merged afterwards.
//
// Holding the lock across the compile instead was tried and is what made the
// fast-forward ANR: a USC compile runs for seconds, and RetroArch's own
// render/input thread ended up parked on this mutex behind a SwanStation core
// compile, so input dispatch timed out. Never put a compile inside this lock.
static pthread_mutex_t cache_mu = PTHREAD_MUTEX_INITIALIZER;

// Scratch caches from batches that finished while the master was busy, waiting
// to be merged. Bounded: past this many, a scratch is dropped rather than
// queued, which costs one recompile of that batch on a later run and nothing else.
#define PENDING_MAX 8
#define DEV_CACHE_MAX 4

// All of this was one global set until 2026-08-16, which made a second live
// VkDevice evict the first's cache unfreed. Keyed per device now.
// ⚠️ This is NOT about concurrent processes — these are statics in a shared
// library, so every process already gets its own private copy and two games
// running Vulkan side by side never shared any of it.
struct dev_cache {
	VkDevice dev;
	VkPipelineCache pc;
	char path[192];
	size_t last_saved_size;
	unsigned long long last_save_ns;
	int frozen;
	// Non-zero while some batch is compiling with pc; nothing else may hand it
	// out, merge into it or serialise it until that batch is done.
	int master_in_use;
	VkPipelineCache pending[PENDING_MAX];
	unsigned pending_count;
	int warned_pending_full;
};
static struct dev_cache dev_caches[DEV_CACHE_MAX];
static int warned_dev_cache_full;
static unsigned compiled_count;
static int slow_session;

// All three are called with cache_mu held.
static struct dev_cache *cache_lookup(VkDevice dev) {
	if (!dev) return NULL;
	for (int i = 0; i < DEV_CACHE_MAX; i++)
		if (dev_caches[i].dev == dev) return &dev_caches[i];
	return NULL;
}

static struct dev_cache *cache_claim(VkDevice dev) {
	struct dev_cache *c = cache_lookup(dev);
	if (c) return c;
	for (int i = 0; i < DEV_CACHE_MAX; i++) {
		if (!dev_caches[i].dev) {
			memset(&dev_caches[i], 0, sizeof dev_caches[i]);
			dev_caches[i].dev = dev;
			return &dev_caches[i];
		}
	}
	// Full: this device runs with no shim cache rather than evicting someone
	// else's. Costs recompiles, corrupts nothing.
	if (!warned_dev_cache_full) {
		warned_dev_cache_full = 1;
		LOGE("more than %d live devices — the newest runs with no pipeline cache",
			DEV_CACHE_MAX);
	}
	return NULL;
}

// App-supplied caches with a batch currently compiling into them.
//
// vkCreatePipelines externally-synchronises its pipelineCache parameter, but
// that is a contract between the app's own threads: nothing stops two of them
// compiling into the same cache at once, and each is correct on its own terms.
// Harvesting *reads* that cache (vkMergePipelineCaches names pSrcCaches as a
// read), which the spec forbids while it is being written — the reason harvest
// shipped opt-in. Counting in-flight batches per handle closes it: a harvest
// runs only when this batch was the cache's last user, which is precisely the
// state the app has already serialised for us.
//
// The other three ways an app touches a cache — merging into it, serialising
// it, destroying it — are hooked below and take cache_mu, so the shim is the
// serialisation point for all of them. What remains unguarded is an app that
// creates a cache and never routes it through the loader's device dispatch,
// which is not a thing an app can do.
#define APP_CACHE_MAX 16
static struct { VkPipelineCache pc; unsigned n; } app_inflight[APP_CACHE_MAX];
static int warned_app_cache_full;

// cache_mu held. Returns 0 when the handle is tracked, -1 when the table is
// full — in which case the caller must not harvest, since it can no longer see
// who else is using the cache.
static int app_cache_ref(VkPipelineCache pc) {
	for (int i = 0; i < APP_CACHE_MAX; i++)
		if (app_inflight[i].pc == pc) { app_inflight[i].n++; return 0; }
	for (int i = 0; i < APP_CACHE_MAX; i++)
		if (app_inflight[i].pc == VK_NULL_HANDLE) {
			app_inflight[i].pc = pc;
			app_inflight[i].n = 1;
			return 0;
		}
	if (!warned_app_cache_full) {
		warned_app_cache_full = 1;
		LOGI("app pipeline-cache table full, not harvesting while it stays full");
	}
	return -1;
}

// cache_mu held. Non-zero when this batch was the last in-flight user of pc,
// i.e. when reading it is safe.
static int app_cache_unref(VkPipelineCache pc) {
	for (int i = 0; i < APP_CACHE_MAX; i++)
		if (app_inflight[i].pc == pc) {
			if (--app_inflight[i].n == 0) {
				app_inflight[i].pc = VK_NULL_HANDLE;
				return 1;
			}
			return 0;
		}
	return 0;
}

static unsigned long long now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// Where this process's pipeline cache lives: inside its own app sandbox, at
// /data/user/<user>/<pkg>/cache/vkshim.pcache.
//
// It used to be a shared /data/vkshim/<pkg>.pcache, which cannot work under
// SELinux enforcement (see the CACHE_MAX_MB comment). The sandbox path needs no
// policy of its own: an app already owns its cache directory, and the directory
// is guaranteed to exist for any installed app.
//
// Returns 0 when this process has no sandbox to write into — anything below the
// first app uid (SurfaceFlinger, bootanim and friends) and isolated processes,
// which are denied their own data dir. Those keep an in-memory cache for the
// life of the process and simply do not persist it; they compile a handful of
// pipelines, not a game's worth.
#define AID_APP_START 10000
#define AID_USER_OFFSET 100000
#define AID_ISOLATED_START 99000

static const char *pkg_name(void);

static int cache_path_for_self(char *out, size_t len) {
	uid_t uid = getuid();
	uid_t appid = uid % AID_USER_OFFSET;
	if (appid < AID_APP_START || appid >= AID_ISOLATED_START) return 0;
	snprintf(out, len, "/data/user/%u/%s/cache/" CACHE_FILE,
		 (unsigned)(uid / AID_USER_OFFSET), pkg_name());
	return 1;
}

// "Clear shader caches" reaches an app sandbox the only way it can: the menu
// bumps a generation property, and each process deletes its own cache the next
// time it starts if the stamp beside the cache does not match. Root cannot do
// this itself -- domain.te neverallows every domain but appdomain and installd
// from unlinking app_data_file.
//
// Takes effect on the next launch of anything already running, which is what
// the root-side delete did too: an app holds its cache open until it exits.
static void drop_cache_if_stale(const char *path) {
	char gen[PROP_VALUE_MAX] = {0};
	__system_property_get(CACHE_GEN_PROP, gen);
	if (!gen[0]) return;

	char gen_path[256];
	size_t n = strlen(path) - (sizeof(CACHE_FILE) - 1);
	snprintf(gen_path, sizeof(gen_path), "%.*s" CACHE_GEN_FILE, (int)n, path);

	char stamp[PROP_VALUE_MAX] = {0};
	int fd = open(gen_path, O_RDONLY);
	if (fd >= 0) {
		ssize_t r = read(fd, stamp, sizeof(stamp) - 1);
		if (r > 0) stamp[r] = '\0';
		close(fd);
	}
	if (!strcmp(stamp, gen)) return;

	if (unlink(path) == 0)
		LOGI("shader cache cleared (generation %s)", gen);
	fd = open(gen_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0) {
		(void)!write(fd, gen, strlen(gen));
		close(fd);
	}
}

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

// prop_on() below defaults to ON when the property is unset — right for the
// fixes that should always be active. This one is the opposite: OFF unless
// explicitly set to 1, for behaviour that must be opted into rather than
// shipped hot. Defined up here because compile_batch() needs it.
static int real_override = -1;
static int cache_harvest = -1;

static int prop_explicit_on(const char *name, int *cache) {
	if (*cache < 0) {
		char v[PROP_VALUE_MAX] = {0};
		__system_property_get(name, v);
		*cache = v[0] == '1' ? 1 : 0;
	}
	return *cache;
}

static size_t cap_bytes(const char *name, unsigned def_mb) {
	char v[PROP_VALUE_MAX] = {0};
	char prop[96];
	snprintf(prop, sizeof(prop), "debug.xdplus.%s", name);
	__system_property_get(prop, v);
	if (!v[0]) {
		snprintf(prop, sizeof(prop), "persist.sys.xdplus.%s", name);
		__system_property_get(prop, v);
	}
	unsigned mb = v[0] ? (unsigned)strtoul(v, NULL, 10) : def_mb;
	if (!mb) mb = def_mb;   // unset, unparseable or 0 all mean "use the default"
	return (size_t)mb * 1024u * 1024u;
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
	struct dev_cache *c = cache_claim(dev);
	if (!c) return VK_NULL_HANDLE;
	if (c->pc != VK_NULL_HANDLE) return c->pc;
	resolve_cache_fns(dev);
	if (!real_cpc || !real_gpcd) return VK_NULL_HANDLE;

	if (cache_path_for_self(c->path, sizeof c->path))
		drop_cache_if_stale(c->path);
	if (!c->path[0]) {
		// No sandbox to persist into: run the cache in memory only. Every
		// path below that touches the file is guarded on c->path[0].
		c->path[0] = '\0';
		LOGI("no app sandbox for uid %u — pipeline cache stays in memory",
		     (unsigned)getuid());
	}
	c->frozen = 0;
	void *initial = NULL;
	size_t initial_size = 0;
	int fd = c->path[0] ? open(c->path, O_RDONLY) : -1;
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
	c->pc = pc;
	c->last_saved_size = initial_size;
	c->last_save_ns = now_ns();
	LOGI("pipeline cache ready for dev=%p (%s, primed %zu bytes)", (void *)dev,
	     c->path[0] ? c->path : "memory only", initial_size);
	return pc;
}

// Called with cache_mu held. `force` bypasses the rate limit for the one save
// that must not be skipped — the device going away.
//
// Serialising the cache is O(cache), and the cache only grows: a mature
// RetroArch cache is ~55 MB, so a save is a 55 MB serialise into a 55 MB malloc
// followed by a 55 MB write. Doing that after every batch, with cache_mu held
// so no other thread can compile meanwhile, is what ANR'd RetroArch when
// SwanStation was switched to Vulkan — the whole app sat in __memcpy inside
// vkGetPipelineCacheData while input dispatch timed out. Losing the last few
// seconds of compiles on a kill costs one recompile; blocking every compile
// behind a 55 MB flush costs the session.
static void save_disk_cache(struct dev_cache *c, int force) {
	if (!c || c->pc == VK_NULL_HANDLE || !real_gpcd || !c->path[0]) return;
	VkDevice dev = c->dev;
	unsigned long long now = now_ns();
	if (!force && c->last_save_ns && now - c->last_save_ns < SAVE_MIN_INTERVAL_NS) return;
	size_t size = 0;
	if (real_gpcd(dev, c->pc, &size, NULL) != VK_SUCCESS || size == 0) return;
	if (size == c->last_saved_size) return;
	// Size query first, cap second, serialise last — the check costs nothing
	// and skips a multi-megabyte malloc and write when it fails.
	size_t cap = cap_bytes("vkcachemax", CACHE_MAX_MB);
	if (size > cap) {
		if (!c->frozen) {
			c->frozen = 1;
			LOGI("pipeline cache frozen at %zu bytes on disk: next write would be %zu, cap is %zu",
				c->last_saved_size, size, cap);
		}
		return;
	}
	c->last_save_ns = now;
	void *data = malloc(size);
	if (!data) return;
	if (real_gpcd(dev, c->pc, &size, data) == VK_SUCCESS) {
		char tmp[sizeof c->path + 4];
		snprintf(tmp, sizeof(tmp), "%s.tmp", c->path);
		int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			if (write(fd, data, size) == (ssize_t)size && !close(fd)) {
				if (rename(tmp, c->path) == 0) {
					c->last_saved_size = size;
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

static void drain_pending(struct dev_cache *c);

static void publish_progress(void) {
	char v[92];
	snprintf(v, sizeof(v), "%s:%u", pkg_name(), compiled_count);
	__system_property_set(PROGRESS_PROP, v);
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

static int prop_on(const char *name, int *cache);   // defined with the other property helpers

// The big stack is per-CALLER-THREAD and persistent.
//
// ⚠️ This used to pthread_create + pthread_join a fresh 64 MB-stack thread for
// every vkCreate*Pipelines call. That is fine for the compile storm it was
// written for (§134's USC recursion) and ruinous for what apps actually do:
// SwanStation calls vkCreateGraphicsPipelines from its render loop, measured
// at ~667 calls/second — about 28 per frame at 24 fps — so the shim was
// mapping, faulting in, unmapping and tearing down a 64 MB stack 667 times a
// second, for calls that are warm cache hits costing microseconds of real
// work. GL pays none of this, which is why Vulkan was slower here regardless
// of GPU load.
//
// One worker per calling thread keeps the stack guarantee with none of the
// churn, and keeps concurrency: two app threads compiling at once still get
// their own worker rather than serialising on a shared one.
struct big_worker {
	pthread_t thread;
	pthread_mutex_t m;
	pthread_cond_t work_cv, done_cv;
	void *(*fn)(void *);
	void *args;
	int have_work, done, quit, started;
};

static pthread_key_t big_worker_key;
static pthread_once_t big_worker_once = PTHREAD_ONCE_INIT;

static void *big_worker_main(void *p) {
	struct big_worker *w = p;
	pthread_mutex_lock(&w->m);
	for (;;) {
		while (!w->have_work && !w->quit)
			pthread_cond_wait(&w->work_cv, &w->m);
		if (w->quit) break;
		void *(*fn)(void *) = w->fn;
		void *args = w->args;
		w->have_work = 0;
		pthread_mutex_unlock(&w->m);
		fn(args);                       // runs on THIS thread's 64 MB stack
		pthread_mutex_lock(&w->m);
		w->done = 1;
		pthread_cond_signal(&w->done_cv);
	}
	pthread_mutex_unlock(&w->m);
	return NULL;
}

// Runs when a calling thread exits, so a worker never outlives its owner.
static void big_worker_destroy(void *p) {
	struct big_worker *w = p;
	if (!w) return;
	if (w->started) {
		pthread_mutex_lock(&w->m);
		w->quit = 1;
		pthread_cond_signal(&w->work_cv);
		pthread_mutex_unlock(&w->m);
		pthread_join(w->thread, NULL);
	}
	pthread_cond_destroy(&w->work_cv);
	pthread_cond_destroy(&w->done_cv);
	pthread_mutex_destroy(&w->m);
	free(w);
}

static void big_worker_key_init(void) {
	pthread_key_create(&big_worker_key, big_worker_destroy);
}

static struct big_worker *big_worker_get(void) {
	pthread_once(&big_worker_once, big_worker_key_init);
	struct big_worker *w = pthread_getspecific(big_worker_key);
	if (w) return w->started ? w : NULL;
	w = calloc(1, sizeof *w);
	if (!w) return NULL;
	pthread_mutex_init(&w->m, NULL);
	pthread_cond_init(&w->work_cv, NULL);
	pthread_cond_init(&w->done_cv, NULL);
	pthread_setspecific(big_worker_key, w);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, COMPILE_STACK_SIZE);
	int err = pthread_create(&w->thread, &attr, big_worker_main, w);
	pthread_attr_destroy(&attr);
	if (err) {
		LOGE("big-stack worker create failed (%d), calls run on the caller stack", err);
		return NULL;
	}
	w->started = 1;
	LOGI("big-stack worker started for this thread (%u MB)", COMPILE_STACK_SIZE >> 20);
	return w;
}

// debug.xdplus.vkbigworker=0 restores the per-call thread this shipped before,
// for bisection. Not the caller's stack: that overflows on a deep USC compile.
static int big_worker_enabled = -1;

static VkResult run_own_thread(void *(*fn)(void *), void *args, VkResult *slot) {
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, COMPILE_STACK_SIZE);
	pthread_t t;
	int err = pthread_create(&t, &attr, fn, args);
	pthread_attr_destroy(&attr);
	if (err) {
		fn(args);                       // last resort, caller's stack
		return *slot;
	}
	pthread_join(t, NULL);
	return *slot;
}

static VkResult run_big_stack(void *(*fn)(void *), void *args, VkResult *slot) {
	if (!prop_on("debug.xdplus.vkbigworker", &big_worker_enabled))
		return run_own_thread(fn, args, slot);
	struct big_worker *w = big_worker_get();
	// Worker creation failed, now or earlier: keep the stack guarantee.
	if (!w) return run_own_thread(fn, args, slot);
	pthread_mutex_lock(&w->m);
	w->fn = fn;
	w->args = args;
	w->have_work = 1;
	w->done = 0;
	pthread_cond_signal(&w->work_cv);
	while (!w->done)
		pthread_cond_wait(&w->done_cv, &w->m);
	pthread_mutex_unlock(&w->m);
	return *slot;
}

// Called with cache_mu held and the master free. Folds in everything that was
// compiled while the master was busy.
static void drain_pending(struct dev_cache *c) {
	if (!c || !c->pending_count) return;
	if (real_mpc && c->pc != VK_NULL_HANDLE)
		real_mpc(c->dev, c->pc, c->pending_count, c->pending);
	if (real_dpc) {
		for (unsigned i = 0; i < c->pending_count; i++)
			real_dpc(c->dev, c->pending[i], NULL);
	}
	c->pending_count = 0;
}

// Wraps one pipeline-creation batch: cache substitution/merge + persistence +
// progress accounting around the big-stack call. The compile runs with no lock
// held — see cache_mu.
static VkResult compile_batch(void *(*fn)(void *), void *args, VkResult *slot,
	VkDevice dev, VkPipelineCache *cache_slot, uint32_t n) {
	const VkPipelineCache app_cache = *cache_slot;
	VkPipelineCache scratch = VK_NULL_HANDLE;
	int own_master = 0;
	int app_tracked = 0;

	pthread_mutex_lock(&cache_mu);
	VkPipelineCache shim_cache = ensure_disk_cache(dev);
	struct dev_cache *c = cache_lookup(dev);
	if (app_cache != VK_NULL_HANDLE)
		app_tracked = app_cache_ref(app_cache) == 0;
	if (app_cache == VK_NULL_HANDLE && shim_cache != VK_NULL_HANDLE
			&& c && !c->master_in_use) {
		c->master_in_use = 1;
		own_master = 1;
		*cache_slot = shim_cache;
	}
	pthread_mutex_unlock(&cache_mu);

	if (!own_master && app_cache == VK_NULL_HANDLE && real_cpc) {
		// Master taken: compile against our own cache rather than queueing
		// behind a compile that may run for seconds. Empty, so this batch pays
		// full price, but its result is merged back and kept.
		VkPipelineCacheCreateInfo ci = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
		};
		if (real_cpc(dev, &ci, NULL, &scratch) == VK_SUCCESS)
			*cache_slot = scratch;
		else
			scratch = VK_NULL_HANDLE;
	}

	unsigned long long t0 = now_ns();
	VkResult r = run_big_stack(fn, args, slot);
	unsigned long long dt = now_ns() - t0;

	pthread_mutex_lock(&cache_mu);
	compiled_count += n;
	if (dt > SLOW_NS) slow_session = 1;
	if (slow_session) publish_progress();

	// The device could have been destroyed under a long compile; re-look it up
	// rather than trusting the pointer taken before the lock was dropped.
	c = cache_lookup(dev);
	if (own_master) {
		// NOTE: app_cache is VK_NULL_HANDLE here by construction — own_master is
		// only set on that path. An earlier version merged app_cache in right
		// here, which was dead code and hid the gap handled below.
		if (c) c->master_in_use = 0;
		drain_pending(c);
		save_disk_cache(c, 0);
	} else if (app_cache != VK_NULL_HANDLE) {
		// The app brought its own cache, so the batch compiled into that and our
		// persistent cache learned nothing. Measured live: SwanStation supplies a
		// cache, so the persistent cache file stayed at its 36-byte empty header
		// through two full compile storms and every launch paid full price.
		//
		// ⚠️ Still OFF by default (debug.xdplus.vkcacheharvest=1 to enable) —
		// unsoaked, not unsafe. The concurrency hole it was held back for is
		// closed: `last_user` is set only when no other batch is compiling into
		// app_cache, and the app's own merge/serialise/destroy of a cache now
		// go through hooks that take cache_mu. See app_cache_ref.
		int last_user = app_tracked ? app_cache_unref(app_cache) : 0;
		if (last_user
				&& prop_explicit_on("debug.xdplus.vkcacheharvest", &cache_harvest)
				&& real_mpc && c && !c->master_in_use
				&& c->pc != VK_NULL_HANDLE) {
			real_mpc(dev, c->pc, 1, &app_cache);
			drain_pending(c);
			save_disk_cache(c, 0);
		}
	} else if (scratch != VK_NULL_HANDLE) {
		if (c && !c->master_in_use && real_mpc && c->pc != VK_NULL_HANDLE) {
			real_mpc(dev, c->pc, 1, &scratch);
			drain_pending(c);
			save_disk_cache(c, 0);
		} else if (c && c->pending_count < PENDING_MAX) {
			c->pending[c->pending_count++] = scratch;
			scratch = VK_NULL_HANDLE;   // owned by the queue now
		} else if (c && !c->warned_pending_full) {
			c->warned_pending_full = 1;
			LOGI("pipeline cache merge queue full, dropping a scratch cache");
		}
	}
	pthread_mutex_unlock(&cache_mu);

	if (scratch != VK_NULL_HANDLE && real_dpc) real_dpc(dev, scratch, NULL);
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

// The app's own operations on a pipeline cache, serialised against ours. Each
// is a straight pass-through except for taking cache_mu, which is what makes
// the harvest above safe: while the shim is reading an app cache, the app
// cannot be merging into it, serialising it or destroying it. Without these the
// spec's external-synchronisation requirement is unmeetable, because the app
// has no way to know the shim is a second user of its cache.
//
// cache_mu is never held across a compile (see cache_mu), so these add no
// contention to the pipeline-compile path that ANR'd — only to the short
// bookkeeping.
// ⭐ Prime the app's OWN pipeline cache from the shim's persistent one.
//
// Without this the persistent cache is write-only for any app that supplies a
// cache of its own, because the shim only substitutes its cache when the app
// passes VK_NULL_HANDLE — every compile then goes against the app's cache and
// never consults ours. SwanStation supplies one, so it recompiled its entire
// shader set through the USC compiler on every single session, with the
// harvest quietly filling a 64 MB file that nothing ever read. Measured with
// one core pinned at 100% inside libusc while the GPU idled and the game ran
// at 24 fps.
//
// Merging our cache into theirs at creation makes their cache start warm.
// ⚠️ Skipped while a batch is compiling into ours: vkMergePipelineCaches reads
// pSrcCaches, and that is exactly what must not race a write.
static int cache_prime = -1;

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreatePipelineCache(
	VkDevice dev, const VkPipelineCacheCreateInfo *ci,
	const VkAllocationCallbacks *ac, VkPipelineCache *out) {
	VkResult r = real_cpc(dev, ci, ac, out);
	if (r != VK_SUCCESS || !prop_on("debug.xdplus.vkcacheprime", &cache_prime))
		return r;
	pthread_mutex_lock(&cache_mu);
	VkPipelineCache mine = ensure_disk_cache(dev);
	struct dev_cache *c = cache_lookup(dev);
	if (mine != VK_NULL_HANDLE && mine != *out && real_mpc && c && !c->master_in_use) {
		unsigned long long t0 = now_ns();
		VkResult mr = real_mpc(dev, *out, 1, &mine);
		LOGI("primed the app's pipeline cache from ours: result %d in %llu ms",
			mr, (now_ns() - t0) / 1000000ull);
	}
	pthread_mutex_unlock(&cache_mu);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_MergePipelineCaches(
	VkDevice dev, VkPipelineCache dst, uint32_t n, const VkPipelineCache *src) {
	pthread_mutex_lock(&cache_mu);
	VkResult r = real_mpc(dev, dst, n, src);
	pthread_mutex_unlock(&cache_mu);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_GetPipelineCacheData(
	VkDevice dev, VkPipelineCache pc, size_t *size, void *data) {
	pthread_mutex_lock(&cache_mu);
	VkResult r = real_gpcd(dev, pc, size, data);
	pthread_mutex_unlock(&cache_mu);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyPipelineCache(
	VkDevice dev, VkPipelineCache pc, const VkAllocationCallbacks *ac) {
	pthread_mutex_lock(&cache_mu);
	// Drop any tracking entry first: a handle the driver is about to free must
	// never be left where a later batch could match it and harvest through it.
	for (int i = 0; i < APP_CACHE_MAX; i++)
		if (app_inflight[i].pc == pc) {
			app_inflight[i].pc = VK_NULL_HANDLE;
			app_inflight[i].n = 0;
			break;
		}
	real_dpc(dev, pc, ac);
	pthread_mutex_unlock(&cache_mu);
}

// ---- scratch-image helpers for mip blit emulation --------------------------

// Per-command-buffer transient scratch images. The blob's vkCmdBlitImage is fine
// for level-0 source and destination, but broken for any non-zero mip level. The
// emulation route is: copy the real source level into a level-0 scratch, blit
// level-0->level-0 into a second scratch at the destination extent, then copy
// that scratch into the real destination level. Each scratch is created and
// destroyed per command buffer; freeing must wait until GPU execution finishes,
// which is when the command buffer is reset, re-begun or freed.

#define CB_TABLE_MAX 256

struct scratch_pair {
	VkImage src_img;
	VkDeviceMemory src_mem;
	VkImage dst_img;
	VkDeviceMemory dst_mem;
};

struct cb_scratch {
	VkDevice dev;
	VkCommandBuffer cb;
	VkCommandPool pool;
	struct scratch_pair *pairs;
	unsigned count;
	unsigned cap;
};

static struct cb_scratch cb_table[CB_TABLE_MAX];
static pthread_mutex_t scratch_mu = PTHREAD_MUTEX_INITIALIZER;
static VkPhysicalDevice physical_dev;

static int find_memory_type(uint32_t mask, VkMemoryPropertyFlags flags,
	VkPhysicalDeviceMemoryProperties *props) {
	for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
		if ((mask & (1u << i)) && (props->memoryTypes[i].propertyFlags & flags) == flags)
			return (int)i;
	}
	return -1;
}

static int alloc_scratch_image(VkDevice dev, VkFormat format, uint32_t width,
	uint32_t height, uint32_t depth, VkImageUsageFlags usage,
	VkPhysicalDeviceMemoryProperties *props, VkImage *img, VkDeviceMemory *mem) {
	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = depth > 1 ? VK_IMAGE_TYPE_3D : (height > 1 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_1D),
		.format = format,
		.extent = {width, height, depth},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkResult r = real_ci(dev, &ici, NULL, img);
	if (r != VK_SUCCESS) { LOGE("scratch image create failed: %d", r); return -1; }
	VkMemoryRequirements req;
	real_gimr(dev, *img, &req);
	uint32_t type = find_memory_type(req.memoryTypeBits,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, props);
	if (type < 0) { LOGE("no device-local memory for scratch image"); real_di(dev, *img, NULL); return -1; }
	VkMemoryAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	};
	r = real_am(dev, &ai, NULL, mem);
	if (r != VK_SUCCESS) { LOGE("scratch image alloc failed: %d", r); real_di(dev, *img, NULL); return -1; }
	real_bim(dev, *img, *mem, 0);
	return 0;
}

static void free_scratch_pair(VkDevice dev, struct scratch_pair *p) {
	if (p->src_img) { real_di(dev, p->src_img, NULL); p->src_img = VK_NULL_HANDLE; }
	if (p->src_mem) { real_fm(dev, p->src_mem, NULL); p->src_mem = VK_NULL_HANDLE; }
	if (p->dst_img) { real_di(dev, p->dst_img, NULL); p->dst_img = VK_NULL_HANDLE; }
	if (p->dst_mem) { real_fm(dev, p->dst_mem, NULL); p->dst_mem = VK_NULL_HANDLE; }
}

static void resolve_scratch_fns(VkDevice dev);
static void ensure_mem_props(void);

static void record_image_barrier(VkCommandBuffer cb, VkImage img,
	VkImageLayout old_layout, VkImageLayout new_layout,
	VkAccessFlags src_access, VkAccessFlags dst_access) {
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = src_access,
		.dstAccessMask = dst_access,
		.oldLayout = old_layout,
		.newLayout = new_layout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};
	real_cpb(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 1, &b);
}

static uint32_t mip_ext(uint32_t base, uint32_t level) {
	uint32_t v = base >> level;
	return v ? v : 1;
}

// Look up or allocate a scratch slot for this command buffer, and copy the
// resulting handles into *out. Linear search is fine: the number of in-flight
// mip blits in one command buffer is small.
//
// ⚠️ scratch_mu is held across the allocation, and no pointer into the table is
// ever returned. slot->pairs is a realloc'd array that clear_cb()/clear_pool()
// free outright, so a pointer handed back after unlocking could be written
// through after the array had moved or been freed — vkResetCommandPool on
// another thread is enough. The allocation runs under the lock instead: it is a
// handful of driver create/bind calls, nothing like a USC compile, and no
// driver path re-enters scratch_mu.
static int get_scratch_pair(VkDevice dev, VkCommandBuffer cb,
	VkFormat format, uint32_t src_w, uint32_t src_h, uint32_t src_d,
	uint32_t dst_w, uint32_t dst_h, uint32_t dst_d,
	VkPhysicalDeviceMemoryProperties *props, struct scratch_pair *out) {
	struct scratch_pair pair = {0};
	pthread_mutex_lock(&scratch_mu);
	struct cb_scratch *slot = NULL;
	for (int i = 0; i < CB_TABLE_MAX; i++) {
		if (cb_table[i].cb == cb) { slot = &cb_table[i]; break; }
		if (!slot && cb_table[i].cb == VK_NULL_HANDLE) slot = &cb_table[i];
	}
	if (!slot) {
		LOGE("out of command-buffer scratch slots");
		pthread_mutex_unlock(&scratch_mu);
		return -1;
	}
	if (slot->cb == VK_NULL_HANDLE) {
		slot->cb = cb;
		slot->pairs = NULL;
		slot->count = 0;
		slot->cap = 0;
	}
	if (slot->count == slot->cap) {
		unsigned new_cap = slot->cap ? slot->cap * 2 : 4;
		struct scratch_pair *new_pairs = realloc(slot->pairs,
			new_cap * sizeof(*slot->pairs));
		if (!new_pairs) {
			LOGE("realloc scratch pair list failed");
			pthread_mutex_unlock(&scratch_mu);
			return -1;
		}
		memset(new_pairs + slot->cap, 0,
			(new_cap - slot->cap) * sizeof(*slot->pairs));
		slot->pairs = new_pairs;
		slot->cap = new_cap;
	}
	if (alloc_scratch_image(dev, format, src_w, src_h, src_d,
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			props, &pair.src_img, &pair.src_mem) != 0 ||
	    alloc_scratch_image(dev, format, dst_w, dst_h, dst_d,
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			props, &pair.dst_img, &pair.dst_mem) != 0) {
		free_scratch_pair(dev, &pair);
		pthread_mutex_unlock(&scratch_mu);
		return -1;
	}
	// Commit only once both images exist, so a failed allocation leaves no
	// half-built entry for the teardown paths to walk.
	slot->pairs[slot->count++] = pair;
	pthread_mutex_unlock(&scratch_mu);
	*out = pair;
	return 0;
}

static void free_cb_scratch_images(VkDevice dev, struct cb_scratch *slot) {
	VkDevice d = dev ? dev : slot->dev;
	if (d) {
		for (unsigned j = 0; j < slot->count; j++)
			free_scratch_pair(d, &slot->pairs[j]);
	}
	free(slot->pairs);
	slot->pairs = NULL;
	slot->count = 0;
	slot->cap = 0;
}

// Two different things used to be one function, and conflating them is what
// made the device mapping unreliable.
//
// recycle_cb: the command buffer is being re-recorded (begin, or an explicit
// reset). Its scratch images are finished with, but the BUFFER STILL EXISTS
// and still belongs to the same device — so the identity must survive.
// ⚠️ Wiping it here is what forced cb_to_device to guess a device from a
// global, and dropped every mip blit the moment that guess was removed.
//
// clear_cb: the command buffer is going away (freed, or its pool destroyed).
// Identity goes with it.
static void recycle_cb(VkDevice dev, VkCommandBuffer cb) {
	pthread_mutex_lock(&scratch_mu);
	for (int i = 0; i < CB_TABLE_MAX; i++) {
		if (cb_table[i].cb == cb) {
			free_cb_scratch_images(dev, &cb_table[i]);
			break;
		}
	}
	pthread_mutex_unlock(&scratch_mu);
}

static void clear_cb(VkDevice dev, VkCommandBuffer cb) {
	pthread_mutex_lock(&scratch_mu);
	for (int i = 0; i < CB_TABLE_MAX; i++) {
		if (cb_table[i].cb == cb) {
			free_cb_scratch_images(dev, &cb_table[i]);
			cb_table[i].dev = VK_NULL_HANDLE;
			cb_table[i].cb = VK_NULL_HANDLE;
			cb_table[i].pool = VK_NULL_HANDLE;
			break;
		}
	}
	pthread_mutex_unlock(&scratch_mu);
}

// vkResetCommandPool leaves every command buffer in the pool ALIVE, so this
// releases scratch without forgetting which device they belong to.
static void recycle_pool(VkDevice dev, VkCommandPool pool) {
	pthread_mutex_lock(&scratch_mu);
	for (int i = 0; i < CB_TABLE_MAX; i++) {
		if (cb_table[i].pool == pool)
			free_cb_scratch_images(dev, &cb_table[i]);
	}
	pthread_mutex_unlock(&scratch_mu);
}

static void clear_pool(VkDevice dev, VkCommandPool pool) {
	pthread_mutex_lock(&scratch_mu);
	for (int i = 0; i < CB_TABLE_MAX; i++) {
		if (cb_table[i].pool == pool) {
			free_cb_scratch_images(dev, &cb_table[i]);
			cb_table[i].dev = VK_NULL_HANDLE;
			cb_table[i].cb = VK_NULL_HANDLE;
			cb_table[i].pool = VK_NULL_HANDLE;
		}
	}
	pthread_mutex_unlock(&scratch_mu);
}

// Force the scratch table to record a device for slots created before
// vkAllocateCommandBuffers was hooked (e.g. loader-internal command buffers).
static void slot_claim_device(VkCommandBuffer cb, VkDevice dev) {
	pthread_mutex_lock(&scratch_mu);
	for (int i = 0; i < CB_TABLE_MAX; i++) {
		if (cb_table[i].cb == cb) {
			if (!cb_table[i].dev) cb_table[i].dev = dev;
			break;
		}
	}
	pthread_mutex_unlock(&scratch_mu);
}

// vkAllocateCommandBuffers is the only place the shim is told a command
// buffer's device by the API itself, so this CREATES the slot rather than only
// updating one that already exists.
//
// ⚠️ It used to update-only, which meant the device was never recorded here at
// all — a freshly allocated buffer is by definition not in the table yet, so
// the loop matched nothing. That was invisible for as long as cb_to_device
// fell back to the last created device, and it turned into "mip blit: no
// device for command buffer, dropping" on every blit the moment the fallback
// was removed. Measured in RetroArch/SwanStation, not reasoned about.
static void remember_pool(VkDevice dev, VkCommandBuffer cb, VkCommandPool pool) {
	pthread_mutex_lock(&scratch_mu);
	struct cb_scratch *slot = NULL;
	for (int i = 0; i < CB_TABLE_MAX; i++) {
		if (cb_table[i].cb == cb) { slot = &cb_table[i]; break; }
		if (!slot && cb_table[i].cb == VK_NULL_HANDLE) slot = &cb_table[i];
	}
	if (slot) {
		// A reused address must not inherit the previous buffer's scratch.
		if (slot->cb != cb) {
			slot->pairs = NULL;
			slot->count = 0;
			slot->cap = 0;
		}
		slot->cb = cb;
		slot->dev = dev;
		slot->pool = pool;
	}
	pthread_mutex_unlock(&scratch_mu);
}

// ---- broken vkCmdBlitImage, and the mip chains built on it -----------------

// This blob's vkCmdBlitImage mishandles mip levels. It is *not* the scaling that
// is broken, which is what the first round of probes concluded from a chain
// built with blits: scaling and mip addressing were varied together, so the
// blame landed on the wrong one. Measured separately with a gradient source, so
// a 1:1 copy is distinguishable from a real resize (tools/vkmip: vkscale,
// vkchain, vksrcmip):
//
//   both subresources at level 0
//                      correct, and exactly so. 2x/4x/8x down and 2x/4x up all
//                      land within a fraction of a code of the ideal box
//                      filter under LINEAR, and on the right texel under
//                      NEAREST; a 1:1 blit is bit-exact under both filters.
//   dstSubresource.mipLevel > 0
//                      the pixels are scaled correctly and then written at
//                      *level 0's* address. The requested level stays empty and
//                      the base level is destroyed. Both filters do this when
//                      source and destination are different images; when they
//                      are the same image NEAREST instead writes the right
//                      level with the scale dropped (texels copied 1:1). A
//                      *non*-scaling blit into a mip level writes nothing at
//                      all under either filter, so a mip destination is broken
//                      whether or not the blit resizes — which is why the test
//                      below keys on the mip level and ignores the extents.
//   srcSubresource.mipLevel > 0
//                      the blit is a silent no-op — nothing is written, and it
//                      does not fall back to reading level 0 either.
//
// ⚠️ That last row describes the *blob*. Do not re-measure it with vksrcmip and
// conclude the same thing about this shim: with the emulation on, a non-zero
// source level is read correctly (the probe reports slope 8.00, i.e. level 1
// and not a fallback to level 0). The probe reported "NOTHING READ" for a while
// after the emulation landed, which looked like confirmation of the row above
// and was really this file staging the result into a scratch image it then
// never copied out whenever the destination was level 0.
//
// No error in any case, and the format advertises BLIT_SRC and BLIT_DST.
//
// vkCmdCopyImage, by contrast, honours mip levels correctly on both sides
// (verified reading level 1 and writing levels 1..6). So the defect is confined
// to the blit path, and a correct chain is buildable: scale into a level-0
// scratch image, then vkCmdCopyImage the scratch into the real level. Done that
// way all 7 levels of a 64x64 chain come out right with level 0 untouched.
//
// That is why the RetroArch glui menu misbehaved: its icons are loaded
// mipmapped and the chain is generated with blits, so the sampler read levels
// that were never written (icons invisible), and level 0 itself carried a
// half-size copy left behind by the chain (each icon drawn twice, one small,
// one full size).
//
// The scratch-and-copy emulation is implemented below, so mip-level blits are
// served rather than dropped; dropping them is what `vkblitmip=0` reverts to.
// Level-0 blits — including scaling ones — go straight through, since they are
// correct. Samplers are still clamped by default, which is now a policy choice
// rather than a necessity: real chains are generated, and `vkmiplod=0` uses them.
//  - debug.xdplus.vkmiplod=0 stops clamping samplers to level 0.
//  - debug.xdplus.vkblitmip=0 disables the emulation and drops mip-level
//    blits (the old behaviour).
static int miplod_clamp = -1;
static int blit_mip_emulate = -1;
static int warned_mip_blit;

static int prop_on(const char *name, int *cache) {
	if (*cache < 0) {
		char v[PROP_VALUE_MAX] = {0};
		__system_property_get(name, v);
		*cache = v[0] == '0' ? 0 : 1;
	}
	return *cache;
}

// This blob is Vulkan 1.0.49, so it exports the promoted-extension entry points
// only under their KHR names: IMG_vkGetPhysicalDeviceFeatures2KHR exists,
// vkGetPhysicalDeviceFeatures2 does not. An app written against 1.1 asks for the
// unsuffixed name, gets NULL, and — if it does not check — calls it. ARMSX2
// (PCSX2) does exactly that: it logs "Failed to load required instance function
// vkGetPhysicalDeviceFeatures2" for Features2/Properties2/MemoryProperties2,
// then jumps to address 0, which its own signal handler turns into a SIGABRT.
//
// For a promoted extension the KHR alias is defined to be the same function
// with the same signature, so falling back to name+"KHR" when the core lookup
// fails is safe and is what the loader would do for a 1.1 driver. It only ever
// runs after the core name has already failed, so it cannot shadow anything.
//
// ⚠️ This does not make the driver 1.1. An app that gets these pointers may
// still rely on other 1.1 behaviour and fail later; the point is to convert an
// immediate null-pointer call into a real capability query.
// debug.xdplus.vkkhralias=0 disables it.
static int khr_alias = -1;

static int khr_name(const char *name, char *buf, size_t n) {
	size_t l = strlen(name);
	if (l + 4 >= n) return 0;
	// Only promoted-style names end in a version digit; and never re-suffix
	// something that is already an extension entry point.
	if (l < 3 || name[l - 1] != '2') return 0;
	memcpy(buf, name, l);
	memcpy(buf + l, "KHR", 4);
	return 1;
}

// ⚠️ A miss returns VK_NULL_HANDLE and the caller drops the emulation. It used
// to fall back to shim_dev, the most recently created device — which is fine
// with one device and silently wrong with two, allocating this command
// buffer's scratch images on somebody else's device. A miss means the shim
// never saw vkBeginCommandBuffer for this buffer, so there is nothing to
// guess from; failing closed costs one unemulated blit and corrupts nothing.
static VkDevice cb_to_device(VkCommandBuffer cb) {
	VkDevice dev = VK_NULL_HANDLE;
	int found = 0, used = 0;
	pthread_mutex_lock(&scratch_mu);
	for (int i = 0; i < CB_TABLE_MAX; i++) {
		if (cb_table[i].cb != VK_NULL_HANDLE) used++;
		if (cb_table[i].cb == cb) {
			dev = cb_table[i].dev;
			found = 1;
		}
	}
	pthread_mutex_unlock(&scratch_mu);
	if (!dev) {
		static int warned;
		if (!warned) {
			warned = 1;
			LOGE("cb_to_device miss: cb=%p found=%d slots_used=%d/%d",
				(void *)cb, found, used, CB_TABLE_MAX);
		}
	}
	return dev;
}

static void ensure_mem_props(void) {
	if (mem_props_loaded) return;
	if (!real_gpdmp && shim_instance) {
		real_gpdmp = (PFN_vkGetPhysicalDeviceMemoryProperties)
			real_gipa(shim_instance, "vkGetPhysicalDeviceMemoryProperties");
	}
	if (!real_gpdmp && shim_dev) {
		real_gpdmp = (PFN_vkGetPhysicalDeviceMemoryProperties)
			real_gdpa(shim_dev, "vkGetPhysicalDeviceMemoryProperties");
	}
	if (real_gpdmp && physical_dev) {
		real_gpdmp(physical_dev, &mem_props);
		mem_props_loaded = 1;
		LOGI("memory properties loaded in ensure_mem_props");
	}
}

static int format_is_depth_stencil(VkFormat f) {
	switch (f) {
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_S8_UINT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return 1;
	default: return 0;
	}
}

static void emit_emulated_blit(VkCommandBuffer cb, VkDevice dev,
	VkImage src, VkImageLayout src_layout,
	VkImage dst, VkImageLayout dst_layout,
	const VkImageBlit *region, VkFilter filter,
	struct img_info *src_info, struct img_info *dst_info) {
	uint32_t src_mip = region->srcSubresource.mipLevel;
	uint32_t dst_mip = region->dstSubresource.mipLevel;
	VkFormat fmt = src_info ? src_info->format : (dst_info ? dst_info->format : VK_FORMAT_R8G8B8A8_UNORM);
	VkExtent3D src_ext = src_info ? src_info->extent : (VkExtent3D){1,1,1};
	VkExtent3D dst_ext = dst_info ? dst_info->extent : (VkExtent3D){1,1,1};
	uint32_t src_w = mip_ext(src_ext.width, src_mip);
	uint32_t src_h = mip_ext(src_ext.height, src_mip);
	uint32_t src_d = mip_ext(src_ext.depth, src_mip);
	uint32_t dst_w = mip_ext(dst_ext.width, dst_mip);
	uint32_t dst_h = mip_ext(dst_ext.height, dst_mip);
	uint32_t dst_d = mip_ext(dst_ext.depth, dst_mip);
	uint32_t layers = region->srcSubresource.layerCount;
	if (!layers) layers = 1;

	// A copy of the table entry, not a pointer into it — see get_scratch_pair.
	// The images stay owned by the table and are destroyed with the command
	// buffer; this is only the handles used to record the commands below.
	struct scratch_pair pair;
	if (get_scratch_pair(dev, cb, fmt, src_w, src_h, src_d, dst_w, dst_h, dst_d,
			&mem_props, &pair) != 0) {
		LOGE("scratch alloc failed, dropping mip blit");
		return;
	}
	struct scratch_pair *p = &pair;

	// Source mip -> scratch_src level 0, if needed.
	if (src_mip != 0) {
		record_image_barrier(cb, p->src_img, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
		VkImageCopy copy_src = {
			.srcSubresource = {
				.aspectMask = region->srcSubresource.aspectMask,
				.mipLevel = src_mip,
				.baseArrayLayer = region->srcSubresource.baseArrayLayer,
				.layerCount = layers,
			},
			.dstSubresource = {
				.aspectMask = region->srcSubresource.aspectMask,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = layers,
			},
			.srcOffset = region->srcOffsets[0],
			.dstOffset = region->srcOffsets[0],
			.extent = {
				.width = (uint32_t)(region->srcOffsets[1].x - region->srcOffsets[0].x),
				.height = (uint32_t)(region->srcOffsets[1].y - region->srcOffsets[0].y),
				.depth = (uint32_t)(region->srcOffsets[1].z - region->srcOffsets[0].z),
			},
		};
		if (!copy_src.extent.width) copy_src.extent.width = 1;
		if (!copy_src.extent.height) copy_src.extent.height = 1;
		if (!copy_src.extent.depth) copy_src.extent.depth = 1;
		real_cci(cb, src, src_layout, p->src_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_src);
		record_image_barrier(cb, p->src_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	}

	// Intermediate blit: scratch_src (or the real src if src_mip==0) -> scratch_dst.
	//
	// ⚠️ When the destination is already level 0 there is nothing to stage: the
	// blit goes straight into the real image. Routing it through scratch_dst
	// anyway wrote the result into a scratch nothing ever read, because the
	// copy-out below only runs for a non-zero destination level — so a blit that
	// merely read a non-zero *source* mip silently produced nothing. That is what
	// tools/vkmip vksrcmip reports as "NOTHING READ", and it looked like the
	// blob's own defect rather than the emulation dropping the result.
	VkImage mid_dst = (dst_mip == 0) ? dst : p->dst_img;
	VkImageLayout mid_dst_layout = (dst_mip == 0)
		? dst_layout : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	if (dst_mip != 0)
		record_image_barrier(cb, p->dst_img, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
	VkImageBlit mid = *region;
	mid.srcSubresource.mipLevel = 0;
	mid.dstSubresource.mipLevel = 0;
	VkImage mid_src = (src_mip == 0) ? src : p->src_img;
	VkImageLayout mid_src_layout = (src_mip == 0) ? src_layout : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	// Scratch images are single-layer, so an endpoint that was staged addresses
	// layer 0; one that goes straight through keeps the layer the app asked for.
	if (src_mip != 0) mid.srcSubresource.baseArrayLayer = 0;
	if (dst_mip != 0) mid.dstSubresource.baseArrayLayer = 0;
	real_cbi(cb, mid_src, mid_src_layout, mid_dst, mid_dst_layout, 1, &mid, filter);

	// scratch_dst level 0 -> real dst mip, if it was staged.
	if (dst_mip != 0) {
		record_image_barrier(cb, p->dst_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		VkImageCopy copy_dst = {
			.srcSubresource = {
				.aspectMask = region->dstSubresource.aspectMask,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = layers,
			},
			.dstSubresource = {
				.aspectMask = region->dstSubresource.aspectMask,
				.mipLevel = dst_mip,
				.baseArrayLayer = region->dstSubresource.baseArrayLayer,
				.layerCount = layers,
			},
			.srcOffset = region->dstOffsets[0],
			.dstOffset = region->dstOffsets[0],
			.extent = {
				.width = (uint32_t)(region->dstOffsets[1].x - region->dstOffsets[0].x),
				.height = (uint32_t)(region->dstOffsets[1].y - region->dstOffsets[0].y),
				.depth = (uint32_t)(region->dstOffsets[1].z - region->dstOffsets[0].z),
			},
		};
		if (!copy_dst.extent.width) copy_dst.extent.width = 1;
		if (!copy_dst.extent.height) copy_dst.extent.height = 1;
		if (!copy_dst.extent.depth) copy_dst.extent.depth = 1;
		real_cci(cb, p->dst_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, dst_layout, 1, &copy_dst);
	}
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdBlitImage(
	VkCommandBuffer cb, VkImage src, VkImageLayout src_layout,
	VkImage dst, VkImageLayout dst_layout, uint32_t n,
	const VkImageBlit *regions, VkFilter filter) {
	int mips = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (regions[i].srcSubresource.mipLevel ||
			regions[i].dstSubresource.mipLevel) mips = 1;
	}
	// Level-0 blits pass through unchanged. The earlier 1:1 LINEAR->NEAREST
	// downgrade was a workaround for a misdiagnosis; level-0 scaling is exact on
	// this blob, and a 1:1 LINEAR blit is a no-op.
	if (!mips) {
		real_cbi(cb, src, src_layout, dst, dst_layout, n, regions, filter);
		return;
	}

	// Mip-level blit: emulate if enabled, otherwise drop.
	if (!prop_on("debug.xdplus.vkblitmip", &blit_mip_emulate)) {
		if (!warned_mip_blit) {
			warned_mip_blit = 1;
			LOGE("dropping vkCmdBlitImage with a non-zero mip level: emulation disabled");
		}
		return;
	}

	VkDevice dev = cb_to_device(cb);
	if (dev == VK_NULL_HANDLE) {
		LOGE("mip blit: no device for command buffer, dropping");
		return;
	}
	slot_claim_device(cb, dev);
	resolve_scratch_fns(dev);
	ensure_mem_props();
	if (!real_ci || !real_cci || !real_cbi || !mem_props_loaded) {
		LOGE("mip blit: scratch functions unavailable, dropping");
		return;
	}

	struct img_info src_info, dst_info;
	if (!find_img(src, &src_info) || !find_img(dst, &dst_info)) {
		LOGE("mip blit: missing image metadata (src=%p dst=%p), dropping", (void*)(uintptr_t)src, (void*)(uintptr_t)dst);
		return;
	}
	if (format_is_depth_stencil(src_info.format)) {
		LOGE("mip blit: depth/stencil format not emulated, dropping");
		return;
	}

	for (uint32_t i = 0; i < n; i++)
		emit_emulated_blit(cb, dev, src, src_layout, dst, dst_layout,
			&regions[i], filter, &src_info, &dst_info);
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

// The driver dereferences VkRenderPassBeginInfo::framebuffer without checking
// it. Disassembly of the blob: IMG_vkCmdBeginRenderPass (zeus/core/
// framebuffer_state.c, entry +0x1f094) loads renderPass and framebuffer with
// one `ldp x10, x20, [x22, #0x10]`, then at +0x1f1e8 does
// `ldrb w10, [x21, #0x80]!` where x21 = framebuffer + idx*0x38. A null handle
// therefore faults at address 0x80 and kills the process. The only guards on
// that path are on the pNext chain and the attachment count; the handle itself
// is never tested, so VK_NULL_HANDLE is a SIGSEGV rather than a validation
// error. Observed from PPSSPP's PerformBindFramebufferAsRenderTarget at 3x
// internal resolution, alongside "GPU KICKSYNC command buffer usage high".
//
// Dropping the call leaves the render pass unstarted: that frame renders
// wrong, which is strictly better than losing the process, and the log line
// names the caller so the real bug (whoever passes a null framebuffer, most
// likely an unchecked vkCreateFramebuffer failure) stays diagnosable.
// debug.xdplus.vkrpguard=0 restores the blob's behaviour for A/B.
static int rp_guard = -1;
static unsigned null_fb_hits;

// Dropping only the vkCmdBeginRenderPass is not enough, and measuring that cost
// one crash: with the pass suppressed the caller still issued its draws, and
// the driver faulted deeper instead (IMG_vkCmdDraw, fault addr 0x30, four blob
// frames down, from PerformRenderPass). A pass that never began must not
// receive commands, so suppression has to span the whole pass — from the
// dropped Begin to the matching End.
//
// The table is tiny and linear on purpose: suppression is rare, command buffers
// in flight are few, and a miss is harmless (the command goes through, which is
// the pre-guard behaviour). Overflow degrades to that same behaviour rather
// than growing unboundedly.
// ⚠️ supp_active() sits on the hot path — it runs on every draw, and the
// assumption it was originally built on is WRONG for the app that needs it.
//
// The first version kept a mutex here, justified by "suppression is rare (one
// pass per frame, and none at all in a healthy session)", with an atomic count
// as a fast path so the lock was only paid while a suppressed pass was open.
// Measured live on PPSSPP at 3x internal resolution: the guard fires
// **continuously, 7-13 times a second on the same VkRenderPass handle** — 1536
// suppressions inside one short session. A suppressed pass is therefore open
// for much of the time, the count fast-path exits almost never, and every draw
// call in the app's heaviest workload was taking a contended mutex. That is the
// opposite of the design intent, in exactly the case that matters.
//
// So the whole thing is lock-free now: the slots are plain pointers accessed
// with atomics, claimed and released by compare-exchange. A draw costs an
// atomic load of the count, one load of the `last` hint (which hits for the
// single-suppressed-buffer case that PPSSPP actually produces), and at worst a
// scan of 16 atomic loads — no mutex, no futex, no cross-thread contention.
//
// Semantics are unchanged, including the sloppy edges, which were always
// sloppy: a racing reader may miss a set that is in flight, in which case the
// command goes through, which is the pre-guard behaviour. Table overflow
// degrades to that same behaviour rather than growing unboundedly.
#define SUPP_MAX 16
static VkCommandBuffer supp_cb[SUPP_MAX];
static VkCommandBuffer supp_last;   // hint: the most recently suppressed buffer
static int supp_n;

static void supp_set(VkCommandBuffer cb) {
	for (int i = 0; i < SUPP_MAX; i++) {
		VkCommandBuffer expect = VK_NULL_HANDLE;
		if (__atomic_compare_exchange_n(&supp_cb[i], &expect, cb, 0,
				__ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			__atomic_store_n(&supp_last, cb, __ATOMIC_RELEASE);
			__atomic_add_fetch(&supp_n, 1, __ATOMIC_RELEASE);
			return;
		}
	}
	// Table full: leave it unsuppressed rather than evicting someone else's
	// entry — a command that gets through is the pre-guard behaviour.
}

static int supp_active(VkCommandBuffer cb) {
	if (__atomic_load_n(&supp_n, __ATOMIC_ACQUIRE) == 0) return 0;
	// cb is never null in practice; checking keeps a null from matching an
	// empty slot in the scan below.
	if (cb == VK_NULL_HANDLE) return 0;
	if (__atomic_load_n(&supp_last, __ATOMIC_ACQUIRE) == cb) return 1;
	for (int i = 0; i < SUPP_MAX; i++)
		if (__atomic_load_n(&supp_cb[i], __ATOMIC_ACQUIRE) == cb) return 1;
	return 0;
}

static void supp_clear(VkCommandBuffer cb) {
	for (int i = 0; i < SUPP_MAX; i++) {
		VkCommandBuffer expect = cb;
		if (__atomic_compare_exchange_n(&supp_cb[i], &expect, VK_NULL_HANDLE, 0,
				__ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			// Retire the hint with the entry, so a cleared buffer cannot keep
			// matching it after its slot is free.
			VkCommandBuffer h = cb;
			__atomic_compare_exchange_n(&supp_last, &h, VK_NULL_HANDLE, 0,
				__ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
			__atomic_sub_fetch(&supp_n, 1, __ATOMIC_RELEASE);
			return;
		}
	}
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdBeginRenderPass(
	VkCommandBuffer cb, const VkRenderPassBeginInfo *rp,
	VkSubpassContents contents) {
	if (rp && rp->framebuffer == VK_NULL_HANDLE &&
		prop_on("debug.xdplus.vkrpguard", &rp_guard)) {
		// Loud on the first one, then sparse: a caller that does this once
		// usually does it every frame, and the log is the diagnostic.
		null_fb_hits++;
		if (null_fb_hits == 1 || null_fb_hits % 512 == 0)
			LOGE("suppressing render pass with NULL framebuffer "
				 "(hit %u, renderPass %p): this driver would SIGSEGV",
				 null_fb_hits, (void *)(uintptr_t)rp->renderPass);
		supp_set(cb);
		return;
	}
	real_cbrp(cb, rp, contents);
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdEndRenderPass(VkCommandBuffer cb) {
	if (supp_active(cb)) { supp_clear(cb); return; }
	real_cerp(cb);
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdNextSubpass(
	VkCommandBuffer cb, VkSubpassContents contents) {
	if (supp_active(cb)) return;
	real_cns(cb, contents);
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdDraw(
	VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
	if (supp_active(cb)) return;
	real_cdraw(cb, vc, ic, fv, fi);
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdDrawIndexed(
	VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi,
	int32_t vo, uint32_t firstInst) {
	if (supp_active(cb)) return;
	real_cdrawi(cb, ic, inst, fi, vo, firstInst);
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdDrawIndirect(
	VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
	uint32_t count, uint32_t stride) {
	if (supp_active(cb)) return;
	real_cdrawind(cb, buf, off, count, stride);
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdDrawIndexedIndirect(
	VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
	uint32_t count, uint32_t stride) {
	if (supp_active(cb)) return;
	real_cdrawiind(cb, buf, off, count, stride);
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdClearAttachments(
	VkCommandBuffer cb, uint32_t ac, const VkClearAttachment *at,
	uint32_t rc, const VkClearRect *rects) {
	if (supp_active(cb)) return;
	real_cca(cb, ac, at, rc, rects);
}

static void resolve_scratch_fns(VkDevice dev) {
	if (!real_ci) real_ci = (PFN_vkCreateImage)real_gdpa(dev, "vkCreateImage");
	if (!real_di) real_di = (PFN_vkDestroyImage)real_gdpa(dev, "vkDestroyImage");
	if (!real_am) real_am = (PFN_vkAllocateMemory)real_gdpa(dev, "vkAllocateMemory");
	if (!real_fm) real_fm = (PFN_vkFreeMemory)real_gdpa(dev, "vkFreeMemory");
	if (!real_gimr) real_gimr = (PFN_vkGetImageMemoryRequirements)real_gdpa(dev, "vkGetImageMemoryRequirements");
	if (!real_bim) real_bim = (PFN_vkBindImageMemory)real_gdpa(dev, "vkBindImageMemory");
	if (!real_cci) real_cci = (PFN_vkCmdCopyImage)real_gdpa(dev, "vkCmdCopyImage");
	if (!real_cpb) real_cpb = (PFN_vkCmdPipelineBarrier)real_gdpa(dev, "vkCmdPipelineBarrier");
	if (!real_bcb) real_bcb = (PFN_vkBeginCommandBuffer)real_gdpa(dev, "vkBeginCommandBuffer");
	if (!real_rcb) real_rcb = (PFN_vkResetCommandBuffer)real_gdpa(dev, "vkResetCommandBuffer");
	if (!real_fcb) real_fcb = (PFN_vkFreeCommandBuffers)real_gdpa(dev, "vkFreeCommandBuffers");
	if (!real_ccpool) real_ccpool = (PFN_vkCreateCommandPool)real_gdpa(dev, "vkCreateCommandPool");
	if (!real_rcpool) real_rcpool = (PFN_vkResetCommandPool)real_gdpa(dev, "vkResetCommandPool");
	if (!real_dcpool) real_dcpool = (PFN_vkDestroyCommandPool)real_gdpa(dev, "vkDestroyCommandPool");
	if (!real_acb) real_acb = (PFN_vkAllocateCommandBuffers)real_gdpa(dev, "vkAllocateCommandBuffers");
}

// ---- image metadata hooks -------------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateImage(
	VkDevice dev, const VkImageCreateInfo *ci,
	const VkAllocationCallbacks *ac, VkImage *out) {
	VkResult r = real_ci(dev, ci, ac, out);
	if (r == VK_SUCCESS && ci) remember_img(*out, ci->format, ci->extent, ci->arrayLayers);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyImage(
	VkDevice dev, VkImage img, const VkAllocationCallbacks *ac) {
	forget_img(img);
	real_di(dev, img, ac);
}

// ---- command-buffer lifetime hooks ----------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL shim_BeginCommandBuffer(
	VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
	// Implicit reset: any scratch images owned by this command buffer are now
	// safe to free because GPU execution has finished (or will not start).
	recycle_cb(VK_NULL_HANDLE, cb); // frees scratch via the slot's own device
	return real_bcb(cb, bi);
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_ResetCommandBuffer(
	VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
	recycle_cb(VK_NULL_HANDLE, cb);
	return real_rcb(cb, flags);
}

static VKAPI_ATTR void VKAPI_CALL shim_FreeCommandBuffers(
	VkDevice dev, VkCommandPool pool, uint32_t n, const VkCommandBuffer *cbs) {
	for (uint32_t i = 0; i < n; i++) clear_cb(dev, cbs[i]);
	real_fcb(dev, pool, n, cbs);
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_AllocateCommandBuffers(
	VkDevice dev, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *cbs) {
	VkResult r = real_acb(dev, ai, cbs);
	if (r == VK_SUCCESS && ai) {
		for (uint32_t i = 0; i < ai->commandBufferCount; i++)
			remember_pool(dev, cbs[i], ai->commandPool);
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_ResetCommandPool(
	VkDevice dev, VkCommandPool pool, VkCommandPoolResetFlags flags) {
	recycle_pool(dev, pool);
	return real_rcpool(dev, pool, flags);
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyCommandPool(
	VkDevice dev, VkCommandPool pool, const VkAllocationCallbacks *ac) {
	clear_pool(dev, pool);
	real_dcpool(dev, pool, ac);
}

// ---- device lifetime -------------------------------------------------------

// Called with cache_mu held. `dev` is the device the cache belongs to, or NULL
// when it is already dead and the handle must not be touched again.
static void drop_disk_cache(struct dev_cache *c, int dev_alive) {
	if (!c) return;
	// Scratch caches waiting to be merged belong to this device too — fold in
	// what we can and free the rest before the device goes.
	if (dev_alive) {
		drain_pending(c);
		if (real_dpc) {
			for (unsigned i = 0; i < c->pending_count; i++)
				real_dpc(c->dev, c->pending[i], NULL);
		}
		if (c->pc != VK_NULL_HANDLE) {
			save_disk_cache(c, 1);
			if (real_dpc) real_dpc(c->dev, c->pc, NULL);
		}
	}
	memset(c, 0, sizeof *c);
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyDevice(
	VkDevice dev, const VkAllocationCallbacks *ac) {
	pthread_mutex_lock(&cache_mu);
	drop_disk_cache(cache_lookup(dev), 1);
	pthread_mutex_unlock(&cache_mu);
	// Free every scratch image associated with this device. We do not store
	// the device per command buffer; clear all slots because the process only
	// ever has one active VkDevice on this hardware anyway.
	pthread_mutex_lock(&scratch_mu);
	for (int i = 0; i < CB_TABLE_MAX; i++) {
		if (cb_table[i].cb != VK_NULL_HANDLE) {
			for (unsigned j = 0; j < cb_table[i].count; j++)
				free_scratch_pair(dev, &cb_table[i].pairs[j]);
			free(cb_table[i].pairs);
			cb_table[i].dev = VK_NULL_HANDLE;
			cb_table[i].cb = VK_NULL_HANDLE;
			cb_table[i].pool = VK_NULL_HANDLE;
			cb_table[i].pairs = NULL;
			cb_table[i].count = 0;
			cb_table[i].cap = 0;
		}
	}
	pthread_mutex_unlock(&scratch_mu);
	pthread_mutex_lock(&img_mu);
	for (int i = 0; i < IMG_TABLE_MAX; i++) img_table[i].used = 0;
	pthread_mutex_unlock(&img_mu);
	real_dd(dev, ac);
}

// Defined with the extension tables below; the filter in shim_CreateDevice is
// the one caller that needs it before that point.
static int shim_owns_ext(const char *name);

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateDevice(
	VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
	const VkAllocationCallbacks *ac, VkDevice *out) {
	// The blob's vkCreateDevice rejects the WHOLE call with
	// VK_ERROR_EXTENSION_NOT_PRESENT on the first name it does not recognise,
	// and every extension the shim advertises is by definition one of those.
	// Without this filter an app that enables what
	// vkEnumerateDeviceExtensionProperties just told it exists gets no Vulkan
	// device at all — it never reaches a shim entry point to be helped by. So
	// strip our own names out of ppEnabledExtensionNames before the blob sees
	// the list, and answer their entry points from the proc-addr hooks.
	VkDeviceCreateInfo filtered;
	const char **kept = NULL;
	if (ci->enabledExtensionCount) {
		kept = malloc(ci->enabledExtensionCount * sizeof *kept);
		if (kept) {
			uint32_t n = 0;
			for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
				const char *e = ci->ppEnabledExtensionNames[i];
				if (shim_owns_ext(e)) {
					LOGI("vkCreateDevice: keeping %s in the shim (blob has no such extension)", e);
					continue;
				}
				kept[n++] = e;
			}
			if (n != ci->enabledExtensionCount) {
				filtered = *ci;
				filtered.enabledExtensionCount = n;
				filtered.ppEnabledExtensionNames = n ? kept : NULL;
				ci = &filtered;
			}
		} else {
			// Out of memory for the copy: pass the app's list through
			// unchanged rather than silently dropping extensions.
			LOGE("vkCreateDevice: extension filter allocation failed, forwarding unfiltered");
		}
	}
	VkResult r = real_cd(pd, ci, ac, out);
	free(kept);
	if (r != VK_SUCCESS) return r;
	// A cache is per-device now, so a live one belonging to another device is
	// no longer something to clear here. The one case still worth handling is
	// a destroy we never saw: the blob hands back a recycled VkDevice address,
	// which would otherwise inherit the dead device's cache. Its handles may
	// already be dangling, so the slot is abandoned rather than freed.
	pthread_mutex_lock(&cache_mu);
	struct dev_cache *stale = cache_lookup(*out);
	if (stale) {
		LOGE("recycled device handle %p still holds a cache — abandoning it unfreed",
			(void *)*out);
		drop_disk_cache(stale, 0);
	}
	pthread_mutex_unlock(&cache_mu);
	physical_dev = pd;
	shim_dev = *out;
	if (!real_gpdmp) {
		real_gpdmp = (PFN_vkGetPhysicalDeviceMemoryProperties)
			real_gdpa(*out, "vkGetPhysicalDeviceMemoryProperties");
	}
	if (real_gpdmp) real_gpdmp(pd, &mem_props);
	mem_props_loaded = real_gpdmp ? 1 : 0;
	LOGI("vkshim device ready: dev=%p physical=%p gpdmp=%p mem_props_loaded=%d", *out, pd, real_gpdmp, mem_props_loaded);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateInstance(
	const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *ac, VkInstance *out) {
	VkResult r = real_cinst(ci, ac, out);
	if (r != VK_SUCCESS) return r;
	shim_instance = *out;
	real_gpdmp = (PFN_vkGetPhysicalDeviceMemoryProperties)real_gipa(*out, "vkGetPhysicalDeviceMemoryProperties");
	return r;
}

// ---- driver identity: VK_KHR_driver_properties + friends ------------------
//
// The blob is Vulkan 1.0.49 from 2017: it predates VK_KHR_driver_properties
// and the VK_KHR_external_*_capabilities trio, so it never advertises them
// and never fills their property structs. DevCheck-class apps therefore show
// the driver as "unknown", conformance 0.0.0.0 and both UUIDs as all zeros —
// the underlying values exist (the DDK 1.9@4893595 build tag, the BVNC the
// blob itself reports as pipelineCacheUUID, the blob's own GNU build-id) but
// nothing surfaces them.
//
// The shim advertises three information-only extensions and answers their
// queries itself, built purely from 1.0 entry points the blob does have:
//
//  - VK_KHR_driver_properties: driverName "PowerVR Rogue", driverInfo the
//    real DDK build tag, driverID VK_DRIVER_ID_IMAGINATION_PROPRIETARY_KHR.
//    conformanceVersion stays 0.0.0.0 — the spec-mandated "unknown": no
//    Series6XT/GX6250 Vulkan submission exists on the Khronos conformant
//    products list (checked 2026-08-13; every PowerVR Vulkan entry there is
//    Series8XE or newer), so there is no real value to report.
//  - VK_KHR_external_fence_capabilities: carried solely to make
//    VkPhysicalDeviceIDProperties queryable. deviceUUID is a stable
//    name-based value derived from the vendorID/deviceID and the BVNC; the
//    driverUUID is the vendor blob's own .note.gnu.build-id, per ABI — the
//    one identity that genuinely changes when the driver binary changes.
//    deviceLUID stays invalid (no device groups on this hardware). The
//    extension's one entry point, vkGetPhysicalDeviceExternalFenceProperties-
//    KHR, reports no external handle support, which is the truth: the blob
//    has none.
//  - VK_KHR_get_physical_device_properties2: the query transport the two
//    structs above ride on. Advertising it lets apps call any of the *2KHR
//    family, so the whole family is implemented over the 1.0 equivalents.
//
// Nothing here changes rendering behaviour; the additions are read-only
// identity. debug.xdplus.vkdrvinfo=0 disables the whole block.
static int drvinfo = -1;

// {vendorID 0x1010 LE, deviceID 0x6250 LE, BVNC "4 40 2 51" as the blob
// spells it in pipelineCacheUUID, "XD+"}. Bytes 6/8 get RFC 4122 version/
// variant bits at fill time — cosmetic only, so UUID-parsing tools see a
// well-formed name-based UUID.
static const uint8_t shim_device_uuid[VK_UUID_SIZE] = {
	0x10, 0x10, 0x50, 0x62, '4', ' ', '4', '0',
	' ', '2', ' ', '5', '1', 'X', 'D', '+'
};
// .note.gnu.build-id of /vendor/lib{,64}/hw/vulkan.mt8173.so (16 B each).
// Bump these if the vendor blob is ever swapped.
#ifdef __LP64__
static const uint8_t shim_driver_uuid[VK_UUID_SIZE] = {
	0x2b, 0x83, 0x3e, 0x4d, 0x8d, 0x51, 0xf6, 0x16,
	0x51, 0xc1, 0x9d, 0x08, 0xef, 0x96, 0x03, 0x7c
};
#else
static const uint8_t shim_driver_uuid[VK_UUID_SIZE] = {
	0xa2, 0x57, 0x48, 0x6e, 0x2b, 0xac, 0x14, 0x93,
	0x31, 0xdb, 0x48, 0x47, 0xa7, 0xce, 0xe0, 0xe6
};
#endif

static const VkExtensionProperties shim_extra_exts[] = {
	{ VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
		VK_KHR_DRIVER_PROPERTIES_SPEC_VERSION },
	{ VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
		VK_KHR_EXTERNAL_FENCE_CAPABILITIES_SPEC_VERSION },
	{ VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION },
};
#define SHIM_EXTRA_EXTS ((uint32_t)(sizeof shim_extra_exts / sizeof shim_extra_exts[0]))

// ---- memory-query and binding extensions ----------------------------------
//
// Vulkan 1.0 core is fully supported here — the blob is a real 1.0.49 driver.
// What it lacks is the set of extensions later promoted into 1.1, which is
// what modern allocators and engines actually probe for. Four of them are
// pure transport over entry points the blob already has, so the shim can
// implement them honestly rather than merely claiming them:
//
//  - VK_KHR_get_memory_requirements2: struct-wrapper over the 1.0
//    vkGet{Image,Buffer}MemoryRequirements and vkGetImageSparseMemory-
//    Requirements. No behaviour change whatsoever.
//  - VK_KHR_bind_memory2: vkBind{Buffer,Image}Memory2 is a loop over the 1.0
//    single binds. Nothing that may legally chain onto those infos exists on
//    this device (device groups and swapchain binds both need extensions the
//    shim does not advertise), so an unknown pNext cannot arrive.
//  - VK_KHR_dedicated_allocation: reports prefers=false, requires=false, which
//    is the truth for this driver, and lets VkMemoryDedicatedAllocateInfo ride
//    in a pNext the blob ignores. Allocation stays valid either way. This is
//    the pairing vk_mem_alloc looks for.
//  - VK_KHR_maintenance3: two properties and one query, all derivable from 1.0
//    limits and memory properties. See fill_maintenance3_properties.
//
// Deliberately NOT advertised, because a shim cannot fake them and claiming
// one turns a working app into a broken one: every SPIR-V/compiler capability
// (storage_buffer_storage_class, variable_pointers, 16bit_storage,
// relaxed_block_layout, shader_float_controls, EXT_scalar_block_layout, the
// subgroup extensions) — the USC compiler would reject the shaders — and
// everything needing absent hardware or kernel plumbing (multiview,
// sampler_ycbcr_conversion, every external_memory/semaphore/fence *object*
// extension, the AHardwareBuffer import path, GOOGLE_display_timing,
// incremental_present).
//
// debug.xdplus.vkmemext=0 disables the whole block.
static int memext = -1;

// Defined with the rest of the block below; the properties2 chain walker sits
// between the two and is the one earlier caller.
static void fill_maintenance3_properties(VkPhysicalDevice pd,
	VkPhysicalDeviceMaintenance3PropertiesKHR *p);

static const VkExtensionProperties shim_mem_exts[] = {
	{ VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
		VK_KHR_GET_MEMORY_REQUIREMENTS_2_SPEC_VERSION },
	{ VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
		VK_KHR_BIND_MEMORY_2_SPEC_VERSION },
	{ VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
		VK_KHR_DEDICATED_ALLOCATION_SPEC_VERSION },
	{ VK_KHR_MAINTENANCE3_EXTENSION_NAME,
		VK_KHR_MAINTENANCE3_SPEC_VERSION },
};
#define SHIM_MEM_EXTS ((uint32_t)(sizeof shim_mem_exts / sizeof shim_mem_exts[0]))

// The two tables above are appended to whatever the blob enumerates, each
// behind its own property gate. Everything that has to agree on "what does
// the shim own" — the enumeration, and the vkCreateDevice filter — goes
// through these two helpers, so adding a row to a table is the whole change.
static uint32_t shim_added_ext_count(void) {
	uint32_t n = 0;
	if (prop_on("debug.xdplus.vkdrvinfo", &drvinfo)) n += SHIM_EXTRA_EXTS;
	if (prop_on("debug.xdplus.vkmemext", &memext)) n += SHIM_MEM_EXTS;
	return n;
}

static const VkExtensionProperties *shim_added_ext_at(uint32_t i) {
	if (prop_on("debug.xdplus.vkdrvinfo", &drvinfo)) {
		if (i < SHIM_EXTRA_EXTS) return &shim_extra_exts[i];
		i -= SHIM_EXTRA_EXTS;
	}
	if (prop_on("debug.xdplus.vkmemext", &memext) && i < SHIM_MEM_EXTS)
		return &shim_mem_exts[i];
	return NULL;
}

// True for an extension the shim advertises and answers itself, i.e. one the
// blob has never heard of. Gate-aware on purpose: with a gate off the name was
// never advertised, so it must not be stripped either — an app enabling it
// then gets the same VK_ERROR_EXTENSION_NOT_PRESENT a driver without the
// extension would give, which is the honest answer.
static int shim_owns_ext(const char *name) {
	uint32_t n = shim_added_ext_count();
	for (uint32_t i = 0; i < n; i++) {
		const VkExtensionProperties *e = shim_added_ext_at(i);
		if (e && !strcmp(e->extensionName, name)) return 1;
	}
	return 0;
}

static int identity_resolved;

static void resolve_identity_fns(void) {
	if (identity_resolved) return;
	identity_resolved = 1;
	if (!real_gpdp) real_gpdp = (PFN_vkGetPhysicalDeviceProperties)
		real_gipa(shim_instance, "vkGetPhysicalDeviceProperties");
	real_gpdf = (PFN_vkGetPhysicalDeviceFeatures)
		real_gipa(shim_instance, "vkGetPhysicalDeviceFeatures");
	real_gpdfp = (PFN_vkGetPhysicalDeviceFormatProperties)
		real_gipa(shim_instance, "vkGetPhysicalDeviceFormatProperties");
	real_gpdifp = (PFN_vkGetPhysicalDeviceImageFormatProperties)
		real_gipa(shim_instance, "vkGetPhysicalDeviceImageFormatProperties");
	real_gpdqfp = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
		real_gipa(shim_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
	real_gpdsifp = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties)
		real_gipa(shim_instance, "vkGetPhysicalDeviceSparseImageFormatProperties");
	if (!real_gpdmp) real_gpdmp = (PFN_vkGetPhysicalDeviceMemoryProperties)
		real_gipa(shim_instance, "vkGetPhysicalDeviceMemoryProperties");
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_EnumerateDeviceExtensionProperties(
	VkPhysicalDevice pd, const char *layer, uint32_t *count,
	VkExtensionProperties *props) {
	uint32_t added = shim_added_ext_count();
	if (layer || !added)
		return real_edep(pd, layer, count, props);
	if (!props) {
		VkResult r = real_edep(pd, NULL, count, NULL);
		if (r != VK_SUCCESS) return r;
		*count += added;
		return VK_SUCCESS;
	}
	// Two-phase with capacity: save it before the real driver overwrites
	// *count with the number it wrote, then append ours into what is left.
	uint32_t cap = *count;
	VkResult r = real_edep(pd, NULL, count, props);
	if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
	uint32_t filled = *count;
	uint32_t appended = 0;
	while (filled + appended < cap && appended < added) {
		props[filled + appended] = *shim_added_ext_at(appended);
		appended++;
	}
	*count = filled + appended;
	return (r == VK_SUCCESS && appended == added)
		? VK_SUCCESS : VK_INCOMPLETE;
}

static void fill_driver_properties(VkPhysicalDeviceDriverPropertiesKHR *p) {
	p->driverID = VK_DRIVER_ID_IMAGINATION_PROPRIETARY_KHR;
	strncpy(p->driverName, "PowerVR Rogue", VK_MAX_DRIVER_NAME_SIZE_KHR);
	strncpy(p->driverInfo, "DDK 1.9@4893595", VK_MAX_DRIVER_INFO_SIZE_KHR);
	// 0.0.0.0 = "unknown" per spec; no GX6250 Vulkan conformance submission
	// exists, so there is no real version to put here.
	p->conformanceVersion.major = 0;
	p->conformanceVersion.minor = 0;
	p->conformanceVersion.subminor = 0;
	p->conformanceVersion.patch = 0;
}

// VK_KHR_push_descriptor is the blob's own extension; only its limit was
// unreachable, because that is queryable solely through properties2, which a
// 1.0.49 driver lacks. Apps therefore read 0 and gave the feature up.
//
// ⚠️ 32 is a MEASURED FLOOR, not a value read from the driver, and nothing
// here can validate a caller that exceeds it. The driver cannot be asked and
// no boundary is observable in either direction; 32 and 33 were shown to work
// repeatably, with storage buffers in a compute shader only. Method, the two
// confounds and the non-determinism: vulkan/tests/README.md.
//
// debug.xdplus.vkpushlimit=0 restores the old behaviour of reporting nothing.
#define SHIM_MAX_PUSH_DESCRIPTORS 32u
static int pushlimit = -1;

static void fill_push_descriptor_properties(VkPhysicalDevicePushDescriptorPropertiesKHR *p) {
	p->maxPushDescriptors = SHIM_MAX_PUSH_DESCRIPTORS;
}

static void fill_id_properties(VkPhysicalDeviceIDPropertiesKHR *p) {
	memcpy(p->deviceUUID, shim_device_uuid, VK_UUID_SIZE);
	p->deviceUUID[6] = (p->deviceUUID[6] & 0x0f) | 0x50;
	p->deviceUUID[8] = (p->deviceUUID[8] & 0x3f) | 0x80;
	memcpy(p->driverUUID, shim_driver_uuid, VK_UUID_SIZE);
	p->driverUUID[6] = (p->driverUUID[6] & 0x0f) | 0x50;
	p->driverUUID[8] = (p->driverUUID[8] & 0x3f) | 0x80;
	p->deviceNodeMask = 0;
	p->deviceLUIDValid = VK_FALSE;
	memset(p->deviceLUID, 0, VK_LUID_SIZE);
}

// The blob's driverVersion is its Perforce changelist (4893595 = the
// "@4893595" in DDK 1.9@4893595). Apps that bit-decode the field
// VK_MAKE_VERSION-style render it as the nonsense "1.170.2971", and no IMG
// packing is published, so every decoder guesses. Re-encode as the real
// marketing version. The changelist is not lost: it stays in driverInfo.
// ⚠️ If the vendor blob is ever swapped, bump this to the new DDK version —
// 1.0-era apps key their own shader caches on driverVersion and would
// otherwise never notice the driver changed.
#define SHIM_DRIVER_VERSION VK_MAKE_VERSION(1, 9, 0)

static VKAPI_ATTR void VKAPI_CALL shim_GetPhysicalDeviceProperties(
	VkPhysicalDevice pd, VkPhysicalDeviceProperties *out) {
	real_gpdp(pd, out);
	out->driverVersion = SHIM_DRIVER_VERSION;
}

static VKAPI_ATTR void VKAPI_CALL shim_GetPhysicalDeviceProperties2KHR(
	VkPhysicalDevice pd, VkPhysicalDeviceProperties2KHR *out) {
	resolve_identity_fns();
	real_gpdp(pd, &out->properties);
	out->properties.driverVersion = SHIM_DRIVER_VERSION;
	for (VkBaseOutStructure *s = (VkBaseOutStructure *)out->pNext; s; s = s->pNext) {
		switch (s->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR:
			fill_driver_properties((VkPhysicalDeviceDriverPropertiesKHR *)s);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR:
			fill_id_properties((VkPhysicalDeviceIDPropertiesKHR *)s);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR:
			// The extension is the blob's own; only the query transport was
			// missing, so this fills a gap rather than claiming a feature.
			if (prop_on("debug.xdplus.vkpushlimit", &pushlimit))
				fill_push_descriptor_properties(
					(VkPhysicalDevicePushDescriptorPropertiesKHR *)s);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES_KHR:
			if (prop_on("debug.xdplus.vkmemext", &memext))
				fill_maintenance3_properties(pd,
					(VkPhysicalDeviceMaintenance3PropertiesKHR *)s);
			break;
		default:
			break; // not ours to fill; a 1.0 driver ignores unknown sTypes too
		}
	}
}

static VKAPI_ATTR void VKAPI_CALL shim_GetPhysicalDeviceFeatures2KHR(
	VkPhysicalDevice pd, VkPhysicalDeviceFeatures2KHR *out) {
	resolve_identity_fns();
	real_gpdf(pd, &out->features);
}

static VKAPI_ATTR void VKAPI_CALL shim_GetPhysicalDeviceFormatProperties2KHR(
	VkPhysicalDevice pd, VkFormat format, VkFormatProperties2KHR *out) {
	resolve_identity_fns();
	real_gpdfp(pd, format, &out->formatProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_GetPhysicalDeviceImageFormatProperties2KHR(
	VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2KHR *info,
	VkImageFormatProperties2KHR *out) {
	resolve_identity_fns();
	return real_gpdifp(pd, info->format, info->type, info->tiling,
		info->usage, info->flags, &out->imageFormatProperties);
}

static VKAPI_ATTR void VKAPI_CALL shim_GetPhysicalDeviceQueueFamilyProperties2KHR(
	VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties2KHR *out) {
	resolve_identity_fns();
	if (!out) {
		real_gpdqfp(pd, count, NULL);
		return;
	}
	uint32_t cap = *count;
	VkQueueFamilyProperties *tmp = malloc(cap * sizeof *tmp);
	if (!tmp) { *count = 0; return; }
	real_gpdqfp(pd, &cap, tmp);
	for (uint32_t i = 0; i < cap; i++)
		out[i].queueFamilyProperties = tmp[i];
	*count = cap;
	free(tmp);
}

static VKAPI_ATTR void VKAPI_CALL shim_GetPhysicalDeviceMemoryProperties2KHR(
	VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2KHR *out) {
	resolve_identity_fns();
	real_gpdmp(pd, &out->memoryProperties);
}

static VKAPI_ATTR void VKAPI_CALL shim_GetPhysicalDeviceSparseImageFormatProperties2KHR(
	VkPhysicalDevice pd, VkFormat format, VkImageType type,
	VkSampleCountFlagBits samples, VkImageUsageFlags usage,
	VkImageTiling tiling, uint32_t *count,
	VkSparseImageFormatProperties2KHR *out) {
	resolve_identity_fns();
	if (!out) {
		real_gpdsifp(pd, format, type, samples, usage, tiling, count, NULL);
		return;
	}
	uint32_t cap = *count;
	VkSparseImageFormatProperties *tmp = malloc(cap * sizeof *tmp);
	if (!tmp) { *count = 0; return; }
	real_gpdsifp(pd, format, type, samples, usage, tiling, &cap, tmp);
	for (uint32_t i = 0; i < cap; i++)
		out[i].properties = tmp[i];
	*count = cap;
	free(tmp);
}

// VK_KHR_external_fence_capabilities' one query: no external handles exist
// on this blob, so every flag stays zero.
static VKAPI_ATTR void VKAPI_CALL shim_GetPhysicalDeviceExternalFencePropertiesKHR(
	VkPhysicalDevice pd, const VkPhysicalDeviceExternalFenceInfoKHR *info,
	VkExternalFencePropertiesKHR *out) {
	(void)pd; (void)info;
	out->exportFromImportedHandleTypes = 0;
	out->compatibleHandleTypes = 0;
	out->externalFenceFeatures = 0;
}

// ---- get_memory_requirements2 / bind_memory2 / dedicated / maintenance3 ----

static void resolve_memext_fns(VkDevice dev) {
	if (!real_gimr) real_gimr = (PFN_vkGetImageMemoryRequirements)
		real_gdpa(dev, "vkGetImageMemoryRequirements");
	if (!real_gbmr) real_gbmr = (PFN_vkGetBufferMemoryRequirements)
		real_gdpa(dev, "vkGetBufferMemoryRequirements");
	if (!real_gismr) real_gismr = (PFN_vkGetImageSparseMemoryRequirements)
		real_gdpa(dev, "vkGetImageSparseMemoryRequirements");
	if (!real_bim) real_bim = (PFN_vkBindImageMemory)
		real_gdpa(dev, "vkBindImageMemory");
	if (!real_bbm) real_bbm = (PFN_vkBindBufferMemory)
		real_gdpa(dev, "vkBindBufferMemory");
}

// VK_KHR_dedicated_allocation's whole output. Both answers are false, and both
// are true statements about this driver: it has no dedicated-allocation path,
// so nothing is preferred and nothing is required. An app that then chains
// VkMemoryDedicatedAllocateInfo onto vkAllocateMemory is chaining an sType the
// blob ignores, and gets an ordinary — valid — allocation.
static void fill_dedicated_requirements(void *pnext) {
	for (VkBaseOutStructure *s = (VkBaseOutStructure *)pnext; s; s = s->pNext) {
		if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS_KHR) {
			VkMemoryDedicatedRequirementsKHR *d =
				(VkMemoryDedicatedRequirementsKHR *)s;
			d->prefersDedicatedAllocation = VK_FALSE;
			d->requiresDedicatedAllocation = VK_FALSE;
		}
	}
}

static VKAPI_ATTR void VKAPI_CALL shim_GetImageMemoryRequirements2KHR(
	VkDevice dev, const VkImageMemoryRequirementsInfo2KHR *info,
	VkMemoryRequirements2KHR *out) {
	resolve_memext_fns(dev);
	real_gimr(dev, info->image, &out->memoryRequirements);
	fill_dedicated_requirements(out->pNext);
}

static VKAPI_ATTR void VKAPI_CALL shim_GetBufferMemoryRequirements2KHR(
	VkDevice dev, const VkBufferMemoryRequirementsInfo2KHR *info,
	VkMemoryRequirements2KHR *out) {
	resolve_memext_fns(dev);
	real_gbmr(dev, info->buffer, &out->memoryRequirements);
	fill_dedicated_requirements(out->pNext);
}

static VKAPI_ATTR void VKAPI_CALL shim_GetImageSparseMemoryRequirements2KHR(
	VkDevice dev, const VkImageSparseMemoryRequirementsInfo2KHR *info,
	uint32_t *count, VkSparseImageMemoryRequirements2KHR *out) {
	resolve_memext_fns(dev);
	if (!out) {
		real_gismr(dev, info->image, count, NULL);
		return;
	}
	uint32_t cap = *count;
	VkSparseImageMemoryRequirements *tmp = malloc(cap * sizeof *tmp);
	if (!tmp) { *count = 0; return; }
	real_gismr(dev, info->image, &cap, tmp);
	for (uint32_t i = 0; i < cap; i++)
		out[i].memoryRequirements = tmp[i];
	*count = cap;
	free(tmp);
}

// VK_KHR_bind_memory2. The spec does not require these to be atomic, and they
// cannot be: a 1.0 bind is permanent, so an error partway through leaves the
// earlier binds in place. That matches what a driver implementing this
// natively would do on a per-resource failure, and every caller treats a
// failed bind as fatal anyway.
static VKAPI_ATTR VkResult VKAPI_CALL shim_BindBufferMemory2KHR(
	VkDevice dev, uint32_t count, const VkBindBufferMemoryInfoKHR *infos) {
	resolve_memext_fns(dev);
	for (uint32_t i = 0; i < count; i++) {
		VkResult r = real_bbm(dev, infos[i].buffer, infos[i].memory,
			infos[i].memoryOffset);
		if (r != VK_SUCCESS) return r;
	}
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_BindImageMemory2KHR(
	VkDevice dev, uint32_t count, const VkBindImageMemoryInfoKHR *infos) {
	resolve_memext_fns(dev);
	for (uint32_t i = 0; i < count; i++) {
		VkResult r = real_bim(dev, infos[i].image, infos[i].memory,
			infos[i].memoryOffset);
		if (r != VK_SUCCESS) return r;
	}
	return VK_SUCCESS;
}

// VK_KHR_maintenance3's two properties, both derived from 1.0 queries.
//
// maxPerSetDescriptors is defined as a number of descriptors in one set layout
// that is *guaranteed* supported, so the minimum across the per-type
// maxDescriptorSet* limits is the correct conservative answer: any layout
// whose total descriptor count is within it is necessarily within every
// individual per-type limit too. The two *Dynamic limits are deliberately left
// out of the minimum — they are typically 8 on this class of hardware and
// would collapse the figure to something no real layout could satisfy.
//
// maxMemoryAllocationSize has no 1.0 equivalent at all. The largest heap is
// the honest ceiling: nothing bigger can be allocated regardless.
static void fill_maintenance3_properties(VkPhysicalDevice pd,
	VkPhysicalDeviceMaintenance3PropertiesKHR *p) {
	VkPhysicalDeviceProperties props;
	memset(&props, 0, sizeof props);
	if (real_gpdp) real_gpdp(pd, &props);
	const VkPhysicalDeviceLimits *l = &props.limits;
	uint32_t m = l->maxDescriptorSetSamplers;
	if (l->maxDescriptorSetUniformBuffers < m) m = l->maxDescriptorSetUniformBuffers;
	if (l->maxDescriptorSetStorageBuffers < m) m = l->maxDescriptorSetStorageBuffers;
	if (l->maxDescriptorSetSampledImages < m) m = l->maxDescriptorSetSampledImages;
	if (l->maxDescriptorSetStorageImages < m) m = l->maxDescriptorSetStorageImages;
	if (l->maxDescriptorSetInputAttachments < m) m = l->maxDescriptorSetInputAttachments;
	p->maxPerSetDescriptors = m;

	VkDeviceSize biggest = 0;
	VkPhysicalDeviceMemoryProperties mp;
	memset(&mp, 0, sizeof mp);
	if (real_gpdmp) real_gpdmp(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
		if (mp.memoryHeaps[i].size > biggest) biggest = mp.memoryHeaps[i].size;
	p->maxMemoryAllocationSize = biggest;
}

static VKAPI_ATTR void VKAPI_CALL shim_GetDescriptorSetLayoutSupportKHR(
	VkDevice dev, const VkDescriptorSetLayoutCreateInfo *ci,
	VkDescriptorSetLayoutSupportKHR *out) {
	(void)dev;
	resolve_identity_fns();
	VkPhysicalDeviceMaintenance3PropertiesKHR m3;
	memset(&m3, 0, sizeof m3);
	fill_maintenance3_properties(physical_dev, &m3);
	uint64_t total = 0;
	for (uint32_t i = 0; i < ci->bindingCount; i++)
		total += ci->pBindings[i].descriptorCount;
	out->supported = total <= m3.maxPerSetDescriptors ? VK_TRUE : VK_FALSE;
}

// ---- proc-addr interposition ----------------------------------------------

// Shared by both proc-addr tables: the loader takes device entry points from
// vkGetDeviceProcAddr, but apps and layers routinely ask vkGetInstanceProcAddr
// for the same names, and the blob answers NULL for all of them either way.
static PFN_vkVoidFunction shim_memext_proc(const char *name) {
	if (!prop_on("debug.xdplus.vkmemext", &memext)) return NULL;
	if (!strcmp(name, "vkGetImageMemoryRequirements2KHR") ||
		!strcmp(name, "vkGetImageMemoryRequirements2"))
		return (PFN_vkVoidFunction)shim_GetImageMemoryRequirements2KHR;
	if (!strcmp(name, "vkGetBufferMemoryRequirements2KHR") ||
		!strcmp(name, "vkGetBufferMemoryRequirements2"))
		return (PFN_vkVoidFunction)shim_GetBufferMemoryRequirements2KHR;
	if (!strcmp(name, "vkGetImageSparseMemoryRequirements2KHR") ||
		!strcmp(name, "vkGetImageSparseMemoryRequirements2"))
		return (PFN_vkVoidFunction)shim_GetImageSparseMemoryRequirements2KHR;
	if (!strcmp(name, "vkBindBufferMemory2KHR") ||
		!strcmp(name, "vkBindBufferMemory2"))
		return (PFN_vkVoidFunction)shim_BindBufferMemory2KHR;
	if (!strcmp(name, "vkBindImageMemory2KHR") ||
		!strcmp(name, "vkBindImageMemory2"))
		return (PFN_vkVoidFunction)shim_BindImageMemory2KHR;
	if (!strcmp(name, "vkGetDescriptorSetLayoutSupportKHR") ||
		!strcmp(name, "vkGetDescriptorSetLayoutSupport"))
		return (PFN_vkVoidFunction)shim_GetDescriptorSetLayoutSupportKHR;
	return NULL;
}

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
	if (!strcmp(name, "vkMergePipelineCaches")) {
		if (!real_mpc) real_mpc = (PFN_vkMergePipelineCaches)real_gdpa(dev, name);
		return real_mpc ? (PFN_vkVoidFunction)shim_MergePipelineCaches : NULL;
	}
	if (!strcmp(name, "vkCreatePipelineCache")) {
		if (!real_cpc) real_cpc = (PFN_vkCreatePipelineCache)real_gdpa(dev, name);
		return real_cpc ? (PFN_vkVoidFunction)shim_CreatePipelineCache : NULL;
	}
	if (!strcmp(name, "vkGetPipelineCacheData")) {
		if (!real_gpcd) real_gpcd = (PFN_vkGetPipelineCacheData)real_gdpa(dev, name);
		return real_gpcd ? (PFN_vkVoidFunction)shim_GetPipelineCacheData : NULL;
	}
	if (!strcmp(name, "vkDestroyPipelineCache")) {
		if (!real_dpc) real_dpc = (PFN_vkDestroyPipelineCache)real_gdpa(dev, name);
		return real_dpc ? (PFN_vkVoidFunction)shim_DestroyPipelineCache : NULL;
	}
	if (!strcmp(name, "vkCmdBlitImage")) {
		if (!real_cbi) real_cbi = (PFN_vkCmdBlitImage)real_gdpa(dev, name);
		return real_cbi ? (PFN_vkVoidFunction)shim_CmdBlitImage : NULL;
	}
	if (!strcmp(name, "vkCreateSampler")) {
		if (!real_cs) real_cs = (PFN_vkCreateSampler)real_gdpa(dev, name);
		return real_cs ? (PFN_vkVoidFunction)shim_CreateSampler : NULL;
	}
	if (!strcmp(name, "vkCreateImage")) {
		if (!real_ci) real_ci = (PFN_vkCreateImage)real_gdpa(dev, name);
		return real_ci ? (PFN_vkVoidFunction)shim_CreateImage : NULL;
	}
	if (!strcmp(name, "vkDestroyImage")) {
		if (!real_di) real_di = (PFN_vkDestroyImage)real_gdpa(dev, name);
		return real_di ? (PFN_vkVoidFunction)shim_DestroyImage : NULL;
	}
	if (!strcmp(name, "vkBeginCommandBuffer")) {
		if (!real_bcb) real_bcb = (PFN_vkBeginCommandBuffer)real_gdpa(dev, name);
		return real_bcb ? (PFN_vkVoidFunction)shim_BeginCommandBuffer : NULL;
	}
	if (!strcmp(name, "vkResetCommandBuffer")) {
		if (!real_rcb) real_rcb = (PFN_vkResetCommandBuffer)real_gdpa(dev, name);
		return real_rcb ? (PFN_vkVoidFunction)shim_ResetCommandBuffer : NULL;
	}
	if (!strcmp(name, "vkFreeCommandBuffers")) {
		if (!real_fcb) real_fcb = (PFN_vkFreeCommandBuffers)real_gdpa(dev, name);
		return real_fcb ? (PFN_vkVoidFunction)shim_FreeCommandBuffers : NULL;
	}
	if (!strcmp(name, "vkAllocateCommandBuffers")) {
		if (!real_acb) real_acb = (PFN_vkAllocateCommandBuffers)real_gdpa(dev, name);
		return real_acb ? (PFN_vkVoidFunction)shim_AllocateCommandBuffers : NULL;
	}
	if (!strcmp(name, "vkResetCommandPool")) {
		if (!real_rcpool) real_rcpool = (PFN_vkResetCommandPool)real_gdpa(dev, name);
		return real_rcpool ? (PFN_vkVoidFunction)shim_ResetCommandPool : NULL;
	}
	if (!strcmp(name, "vkDestroyCommandPool")) {
		if (!real_dcpool) real_dcpool = (PFN_vkDestroyCommandPool)real_gdpa(dev, name);
		return real_dcpool ? (PFN_vkVoidFunction)shim_DestroyCommandPool : NULL;
	}
	// The whole suppressed-pass set has to be hooked together: hooking Begin
	// alone leaves the draws to reach a driver with no pass state.
	if (!strcmp(name, "vkCmdBeginRenderPass")) {
		if (!real_cbrp) real_cbrp = (PFN_vkCmdBeginRenderPass)real_gdpa(dev, name);
		return real_cbrp ? (PFN_vkVoidFunction)shim_CmdBeginRenderPass : NULL;
	}
	if (!strcmp(name, "vkCmdEndRenderPass")) {
		if (!real_cerp) real_cerp = (PFN_vkCmdEndRenderPass)real_gdpa(dev, name);
		return real_cerp ? (PFN_vkVoidFunction)shim_CmdEndRenderPass : NULL;
	}
	if (!strcmp(name, "vkCmdNextSubpass")) {
		if (!real_cns) real_cns = (PFN_vkCmdNextSubpass)real_gdpa(dev, name);
		return real_cns ? (PFN_vkVoidFunction)shim_CmdNextSubpass : NULL;
	}
	if (!strcmp(name, "vkCmdDraw")) {
		if (!real_cdraw) real_cdraw = (PFN_vkCmdDraw)real_gdpa(dev, name);
		return real_cdraw ? (PFN_vkVoidFunction)shim_CmdDraw : NULL;
	}
	if (!strcmp(name, "vkCmdDrawIndexed")) {
		if (!real_cdrawi) real_cdrawi = (PFN_vkCmdDrawIndexed)real_gdpa(dev, name);
		return real_cdrawi ? (PFN_vkVoidFunction)shim_CmdDrawIndexed : NULL;
	}
	if (!strcmp(name, "vkCmdDrawIndirect")) {
		if (!real_cdrawind) real_cdrawind = (PFN_vkCmdDrawIndirect)real_gdpa(dev, name);
		return real_cdrawind ? (PFN_vkVoidFunction)shim_CmdDrawIndirect : NULL;
	}
	if (!strcmp(name, "vkCmdDrawIndexedIndirect")) {
		if (!real_cdrawiind) real_cdrawiind = (PFN_vkCmdDrawIndexedIndirect)real_gdpa(dev, name);
		return real_cdrawiind ? (PFN_vkVoidFunction)shim_CmdDrawIndexedIndirect : NULL;
	}
	if (!strcmp(name, "vkCmdClearAttachments")) {
		if (!real_cca) real_cca = (PFN_vkCmdClearAttachments)real_gdpa(dev, name);
		return real_cca ? (PFN_vkVoidFunction)shim_CmdClearAttachments : NULL;
	}
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)shim_GetDeviceProcAddr;
	// The 1.1-promoted memory-query and binding extensions the shim implements
	// over 1.0. Both spellings are registered, as in the identity block: the
	// blob exports neither, so nothing here can shadow a real driver entry
	// point. Every one of them resolves its 1.0 backing lazily on first call,
	// so returning the pointer without touching the device is safe.
	{
		PFN_vkVoidFunction g = shim_memext_proc(name);
		if (g) return g;
	}
	PFN_vkVoidFunction f = real_gdpa(dev, name);
	if (!f && prop_on("debug.xdplus.vkkhralias", &khr_alias)) {
		char alias[128];
		if (khr_name(name, alias, sizeof alias) &&
			(f = real_gdpa(dev, alias)) != NULL)
			LOGI("aliased %s -> %s (1.0 driver, promoted extension)", name, alias);
	}
	return f;
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
	if (!strcmp(name, "vkCreateInstance")) {
		if (!real_cinst) real_cinst = (PFN_vkCreateInstance)real_gipa(inst, name);
		return real_cinst ? (PFN_vkVoidFunction)shim_CreateInstance : NULL;
	}
	if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) {
		if (!real_edep) real_edep = (PFN_vkEnumerateDeviceExtensionProperties)real_gipa(inst, name);
		return real_edep ? (PFN_vkVoidFunction)shim_EnumerateDeviceExtensionProperties : NULL;
	}
	// Driver-identity block: physical-device queries, answered by the shim
	// from 1.0 entry points. Both spellings are registered — the unsuffixed
	// names are what the khr_alias fallthrough below would resolve to anyway.
	if (prop_on("debug.xdplus.vkdrvinfo", &drvinfo)) {
		if (!strcmp(name, "vkGetPhysicalDeviceProperties")) {
			if (!real_gpdp) real_gpdp = (PFN_vkGetPhysicalDeviceProperties)real_gipa(inst, name);
			return real_gpdp ? (PFN_vkVoidFunction)shim_GetPhysicalDeviceProperties : NULL;
		}
		if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR") ||
			!strcmp(name, "vkGetPhysicalDeviceProperties2"))
			return (PFN_vkVoidFunction)shim_GetPhysicalDeviceProperties2KHR;
		if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") ||
			!strcmp(name, "vkGetPhysicalDeviceFeatures2"))
			return (PFN_vkVoidFunction)shim_GetPhysicalDeviceFeatures2KHR;
		if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") ||
			!strcmp(name, "vkGetPhysicalDeviceFormatProperties2"))
			return (PFN_vkVoidFunction)shim_GetPhysicalDeviceFormatProperties2KHR;
		if (!strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR") ||
			!strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2"))
			return (PFN_vkVoidFunction)shim_GetPhysicalDeviceImageFormatProperties2KHR;
		if (!strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2KHR") ||
			!strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2"))
			return (PFN_vkVoidFunction)shim_GetPhysicalDeviceQueueFamilyProperties2KHR;
		if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR") ||
			!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2"))
			return (PFN_vkVoidFunction)shim_GetPhysicalDeviceMemoryProperties2KHR;
		if (!strcmp(name, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR") ||
			!strcmp(name, "vkGetPhysicalDeviceSparseImageFormatProperties2"))
			return (PFN_vkVoidFunction)shim_GetPhysicalDeviceSparseImageFormatProperties2KHR;
		if (!strcmp(name, "vkGetPhysicalDeviceExternalFencePropertiesKHR") ||
			!strcmp(name, "vkGetPhysicalDeviceExternalFenceProperties"))
			return (PFN_vkVoidFunction)shim_GetPhysicalDeviceExternalFencePropertiesKHR;
	}
	{
		PFN_vkVoidFunction g = shim_memext_proc(name);
		if (g) return g;
	}
	PFN_vkVoidFunction f = real_gipa(inst, name);
	if (!f && prop_on("debug.xdplus.vkkhralias", &khr_alias)) {
		char alias[128];
		if (khr_name(name, alias, sizeof alias) &&
			(f = real_gipa(inst, alias)) != NULL)
			LOGI("aliased %s -> %s (1.0 driver, promoted extension)", name, alias);
	}
	return f;
}

// ---- hwvulkan HAL module --------------------------------------------------

static int load_real(void) {
	if (real_module) return 0;
	// The vendor blob in the image is the driver. The /data/local/tmp copy is a
	// development override from the prototype era, when installs bind-mounted the
	// shim over the vendor path and the real blob had to be stashed elsewhere.
	//
	// ⚠️ That stash used to be probed FIRST, which meant a shipped build took its
	// Vulkan driver from a directory any adb shell can write (/data/local/tmp is
	// drwxrwx--x shell shell), and a stale A/B copy left there silently won over
	// the image's own blob. The override is still available for A/B work, but it
	// is now opt-in: set debug.xdplus.vkrealoverride=1.
#ifdef __LP64__
	const char *vendor_path = "/vendor/lib64/hw/vulkan.mt8173.so";
	const char *override_path = "/data/local/tmp/vulkan.mt8173.real.so";
#else
	const char *vendor_path = "/vendor/lib/hw/vulkan.mt8173.so";
	const char *override_path = "/data/local/tmp/vulkan.mt8173.real32.so";
#endif
	const char *path = vendor_path;
	void *h = NULL;
	if (prop_explicit_on("debug.xdplus.vkrealoverride", &real_override)) {
		path = override_path;
		h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (h) LOGI("using override driver %s", path);
	}
	if (!h) { path = vendor_path; h = dlopen(path, RTLD_NOW | RTLD_LOCAL); }
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
