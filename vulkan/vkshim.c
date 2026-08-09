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
static PFN_vkBindImageMemory real_bim;
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
static VkDevice cache_dev;
static VkPipelineCache disk_cache;
static char cache_path[192];
static size_t last_saved_size;
static unsigned long long last_save_ns;
static int cache_frozen;
// Non-zero while some batch is compiling with disk_cache; nothing else may hand
// it out, merge into it or serialise it until that batch is done.
static int master_in_use;
// Scratch caches from batches that finished while the master was busy, waiting
// to be merged. Bounded: past this many, a scratch is dropped rather than
// queued, which costs one recompile of that batch on a later run and nothing else.
#define PENDING_MAX 8
static VkPipelineCache pending_merge[PENDING_MAX];
static unsigned pending_count;
static int warned_pending_full;
static unsigned compiled_count;
static int slow_session;

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
static void drop_cache_if_stale(void) {
	char gen[PROP_VALUE_MAX] = {0};
	__system_property_get(CACHE_GEN_PROP, gen);
	if (!gen[0]) return;

	char gen_path[sizeof(cache_path)];
	size_t n = strlen(cache_path) - (sizeof(CACHE_FILE) - 1);
	snprintf(gen_path, sizeof(gen_path), "%.*s" CACHE_GEN_FILE, (int)n, cache_path);

	char stamp[PROP_VALUE_MAX] = {0};
	int fd = open(gen_path, O_RDONLY);
	if (fd >= 0) {
		ssize_t r = read(fd, stamp, sizeof(stamp) - 1);
		if (r > 0) stamp[r] = '\0';
		close(fd);
	}
	if (!strcmp(stamp, gen)) return;

	if (unlink(cache_path) == 0)
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
	if (cache_dev == dev && disk_cache != VK_NULL_HANDLE) return disk_cache;
	resolve_cache_fns(dev);
	if (!real_cpc || !real_gpcd) return VK_NULL_HANDLE;

	if (cache_path_for_self(cache_path, sizeof(cache_path)))
		drop_cache_if_stale();
	if (!cache_path[0]) {
		// No sandbox to persist into: run the cache in memory only. Every
		// path below that touches the file is guarded on cache_path[0].
		cache_path[0] = '\0';
		LOGI("no app sandbox for uid %u — pipeline cache stays in memory",
		     (unsigned)getuid());
	}
	cache_frozen = 0;
	void *initial = NULL;
	size_t initial_size = 0;
	int fd = cache_path[0] ? open(cache_path, O_RDONLY) : -1;
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
	last_save_ns = now_ns();
	LOGI("pipeline cache ready (%s, primed %zu bytes)",
	     cache_path[0] ? cache_path : "memory only", initial_size);
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
static void save_disk_cache(VkDevice dev, int force) {
	if (disk_cache == VK_NULL_HANDLE || !real_gpcd || !cache_path[0]) return;
	unsigned long long now = now_ns();
	if (!force && last_save_ns && now - last_save_ns < SAVE_MIN_INTERVAL_NS) return;
	size_t size = 0;
	if (real_gpcd(dev, disk_cache, &size, NULL) != VK_SUCCESS || size == 0) return;
	if (size == last_saved_size) return;
	// Size query first, cap second, serialise last — the check costs nothing
	// and skips a multi-megabyte malloc and write when it fails.
	size_t cap = cap_bytes("vkcachemax", CACHE_MAX_MB);
	if (size > cap) {
		if (!cache_frozen) {
			cache_frozen = 1;
			LOGI("pipeline cache frozen at %zu bytes on disk: next write would be %zu, cap is %zu",
				last_saved_size, size, cap);
		}
		return;
	}
	last_save_ns = now;
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

static void drain_pending(VkDevice dev);

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

// Called with cache_mu held and the master free. Folds in everything that was
// compiled while the master was busy.
static void drain_pending(VkDevice dev) {
	if (!pending_count) return;
	if (real_mpc && disk_cache != VK_NULL_HANDLE && cache_dev == dev)
		real_mpc(dev, disk_cache, pending_count, pending_merge);
	if (real_dpc) {
		for (unsigned i = 0; i < pending_count; i++)
			real_dpc(dev, pending_merge[i], NULL);
	}
	pending_count = 0;
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
	if (app_cache != VK_NULL_HANDLE)
		app_tracked = app_cache_ref(app_cache) == 0;
	if (app_cache == VK_NULL_HANDLE && shim_cache != VK_NULL_HANDLE && !master_in_use) {
		master_in_use = 1;
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

	if (own_master) {
		// NOTE: app_cache is VK_NULL_HANDLE here by construction — own_master is
		// only set on that path. An earlier version merged app_cache in right
		// here, which was dead code and hid the gap handled below.
		master_in_use = 0;
		drain_pending(dev);
		save_disk_cache(dev, 0);
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
				&& real_mpc && !master_in_use
				&& disk_cache != VK_NULL_HANDLE && cache_dev == dev) {
			real_mpc(dev, disk_cache, 1, &app_cache);
			drain_pending(dev);
			save_disk_cache(dev, 0);
		}
	} else if (scratch != VK_NULL_HANDLE) {
		if (!master_in_use && real_mpc && disk_cache != VK_NULL_HANDLE
				&& cache_dev == dev) {
			real_mpc(dev, disk_cache, 1, &scratch);
			drain_pending(dev);
			save_disk_cache(dev, 0);
		} else if (pending_count < PENDING_MAX) {
			pending_merge[pending_count++] = scratch;
			scratch = VK_NULL_HANDLE;   // owned by the queue now
		} else if (!warned_pending_full) {
			warned_pending_full = 1;
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
// contention to the path that ANR'd in §139 — only to the short bookkeeping.
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

static void remember_pool(VkDevice dev, VkCommandBuffer cb, VkCommandPool pool) {
	pthread_mutex_lock(&scratch_mu);
	for (int i = 0; i < CB_TABLE_MAX; i++) {
		if (cb_table[i].cb == cb) {
			cb_table[i].dev = dev;
			cb_table[i].pool = pool;
			break;
		}
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

static VkDevice cb_to_device(VkCommandBuffer cb) {
	VkDevice dev = shim_dev;
	pthread_mutex_lock(&scratch_mu);
	for (int i = 0; i < CB_TABLE_MAX; i++) {
		if (cb_table[i].cb == cb) {
			if (cb_table[i].dev) dev = cb_table[i].dev;
			break;
		}
	}
	pthread_mutex_unlock(&scratch_mu);
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
	clear_cb(VK_NULL_HANDLE, cb); // device not needed for metadata clear
	return real_bcb(cb, bi);
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_ResetCommandBuffer(
	VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
	clear_cb(VK_NULL_HANDLE, cb);
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
	clear_pool(dev, pool);
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
static void drop_disk_cache(VkDevice dev) {
	// Scratch caches waiting to be merged belong to this device too — fold in
	// what we can and free the rest before the device goes.
	if (dev) drain_pending(dev);
	if (real_dpc && dev) {
		for (unsigned i = 0; i < pending_count; i++)
			real_dpc(dev, pending_merge[i], NULL);
	}
	pending_count = 0;
	master_in_use = 0;
	if (disk_cache != VK_NULL_HANDLE && dev) {
		save_disk_cache(dev, 1);
		if (real_dpc) real_dpc(dev, disk_cache, NULL);
	}
	disk_cache = VK_NULL_HANDLE;
	cache_dev = NULL;
	last_saved_size = 0;
	last_save_ns = 0;
	cache_frozen = 0;
	cache_path[0] = '\0';
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyDevice(
	VkDevice dev, const VkAllocationCallbacks *ac) {
	pthread_mutex_lock(&cache_mu);
	if (cache_dev == dev) drop_disk_cache(dev);
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
	if (!strcmp(name, "vkMergePipelineCaches")) {
		if (!real_mpc) real_mpc = (PFN_vkMergePipelineCaches)real_gdpa(dev, name);
		return real_mpc ? (PFN_vkVoidFunction)shim_MergePipelineCaches : NULL;
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
