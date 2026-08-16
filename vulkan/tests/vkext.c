// Headless probe for the extension set vkshim adds on top of the 1.0.49 blob.
//
// Checks, in order:
//   1. what vkEnumerateDeviceExtensionProperties reports (blob + shim),
//   2. that vkCreateDevice SUCCEEDS with every shim-advertised extension
//      enabled -- the blob fails the whole call on the first name it does not
//      know, so this is the regression test for the ppEnabledExtensionNames
//      filter in shim_CreateDevice,
//   3. that each new entry point resolves and answers sanely:
//      VK_KHR_get_memory_requirements2 (against the 1.0 answer),
//      VK_KHR_dedicated_allocation, VK_KHR_bind_memory2, VK_KHR_maintenance3.
//
// The expected results track what vkshim.c advertises. Adding an extension to
// the shim means adding its check here; a bare pass proves only that what was
// claimed at the time of writing still works.
//
// Build + run: ./run.sh  (needs an NDK; see the script)
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHECK(r, what) do { \
	VkResult _r = (r); \
	if (_r != VK_SUCCESS) { printf("FAIL %s: VkResult %d\n", what, _r); return 1; } \
} while (0)

// ---- maxPushDescriptors search --------------------------------------------
//
// VK_KHR_push_descriptor is genuinely supported by the blob, but its one limit
// was only ever queryable through vkGetPhysicalDeviceProperties2, which the
// 1.0.49 driver does not have -- so an app chaining
// VkPhysicalDevicePushDescriptorPropertiesKHR sees maxPushDescriptors = 0.
// The real value is not in any header on this tree, and both ways of guessing
// are bad: too high can crash an app, too low denies it a working feature.
//
// It is measurable, though. A layout created with
// VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR and more than
// maxPushDescriptors descriptors must be rejected, so the largest accepted
// count is the limit -- PROVIDED two things hold, and the probe checks both
// before believing any number:
//
//  1. The driver actually validates. A 2017 driver with no validation layers
//     may accept anything handed to it, in which case the search "finds" the
//     top of its own range and means nothing. Probed by trying an absurd count
//     first: if that is accepted, the answer is UNBOUNDED and worthless.
//  2. The rejection is about push descriptors specifically. A plain layout
//     with the same count is the control: if it is rejected too, what was
//     found is the general per-set limit, not maxPushDescriptors.
#define ABSURD_PUSH_COUNT 65536u

// Some drivers only diagnose an over-large set at pipeline-layout time, when
// the set has to be assigned real resource slots. Same object-creation cost,
// no GPU work, so it is worth trying before giving up.
static VkResult try_pipeline_layout(VkDevice dev, uint32_t count, int push) {
	VkDescriptorSetLayoutBinding b = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = count,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo lci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.flags = push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
		.bindingCount = 1, .pBindings = &b,
	};
	VkDescriptorSetLayout set = VK_NULL_HANDLE;
	VkResult r = vkCreateDescriptorSetLayout(dev, &lci, NULL, &set);
	if (r != VK_SUCCESS) return r;
	VkPipelineLayoutCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &set,
	};
	VkPipelineLayout pl = VK_NULL_HANDLE;
	r = vkCreatePipelineLayout(dev, &pci, NULL, &pl);
	if (r == VK_SUCCESS) vkDestroyPipelineLayout(dev, pl, NULL);
	vkDestroyDescriptorSetLayout(dev, set, NULL);
	return r;
}

static uint32_t largest_pl_accepted(VkDevice dev, uint32_t hi, int push) {
	if (try_pipeline_layout(dev, 1, push) != VK_SUCCESS) return 0;
	uint32_t lo = 1;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo + 1) / 2;
		if (try_pipeline_layout(dev, mid, push) == VK_SUCCESS) lo = mid;
		else hi = mid - 1;
	}
	return lo;
}

static VkResult try_layout(VkDevice dev, uint32_t count, int push) {
	VkDescriptorSetLayoutBinding b = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = count,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.flags = push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
		.bindingCount = 1, .pBindings = &b,
	};
	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	VkResult r = vkCreateDescriptorSetLayout(dev, &ci, NULL, &layout);
	if (r == VK_SUCCESS) vkDestroyDescriptorSetLayout(dev, layout, NULL);
	return r;
}

// Largest count accepted in [1, hi], or 0 if even 1 is refused.
static uint32_t largest_accepted(VkDevice dev, uint32_t hi, int push) {
	if (try_layout(dev, 1, push) != VK_SUCCESS) return 0;
	uint32_t lo = 1;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo + 1) / 2;
		if (try_layout(dev, mid, push) == VK_SUCCESS) lo = mid;
		else hi = mid - 1;
	}
	return lo;
}

static int probe_push_descriptors(VkPhysicalDevice pd, VkDevice dev) {
	printf("\n---- maxPushDescriptors search ----\n");

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("control limits: maxDescriptorSetUniformBuffers=%u "
		"maxPerStageDescriptorUniformBuffers=%u\n",
		props.limits.maxDescriptorSetUniformBuffers,
		props.limits.maxPerStageDescriptorUniformBuffers);

	// Gate 1: does this driver validate descriptor-set layout creation at all?
	VkResult absurd = try_layout(dev, ABSURD_PUSH_COUNT, 1);
	printf("absurd push layout (%u descriptors) -> VkResult %d\n",
		ABSURD_PUSH_COUNT, absurd);
	if (absurd == VK_SUCCESS) {
		printf("  set-layout creation does not validate this.\n");
		// Second chance: the pipeline layout, where the set has to be given
		// real slots.
		VkResult pl = try_pipeline_layout(dev, ABSURD_PUSH_COUNT, 1);
		printf("absurd push PIPELINE layout -> VkResult %d\n", pl);
		if (pl == VK_SUCCESS) {
			printf("INCONCLUSIVE: neither set-layout nor pipeline-layout creation\n"
				"  validates an absurd push-descriptor count, so this driver does\n"
				"  not diagnose the limit at object-creation time at all. A search\n"
				"  would only find the top of its own range. maxPushDescriptors\n"
				"  cannot be measured this way and must stay unreported.\n");
			return 0;
		}
		uint32_t p_push = largest_pl_accepted(dev, ABSURD_PUSH_COUNT - 1, 1);
		uint32_t p_plain = largest_pl_accepted(dev, ABSURD_PUSH_COUNT - 1, 0);
		printf("pipeline-layout boundary: push=%u plain=%u\n", p_push, p_plain);
		if (p_push && p_push < p_plain)
			printf("MEASURED (at pipeline-layout creation): maxPushDescriptors = %u\n",
				p_push);
		else
			printf("INCONCLUSIVE: the pipeline-layout boundary is not specific to\n"
				"  the push-descriptor flag, so it is the general per-set limit.\n");
		return 0;
	}

	uint32_t push_max = largest_accepted(dev, ABSURD_PUSH_COUNT - 1, 1);
	uint32_t plain_max = largest_accepted(dev, ABSURD_PUSH_COUNT - 1, 0);
	printf("largest accepted: push=%u plain=%u\n", push_max, plain_max);

	if (!push_max) {
		printf("INCONCLUSIVE: even a single push descriptor is refused.\n");
		return 0;
	}
	// Gate 2: is the boundary about push descriptors, or the general limit?
	if (push_max >= plain_max) {
		printf("INCONCLUSIVE: the push boundary is not below the plain one, so\n"
			"  what was found is the general per-set limit, not maxPushDescriptors.\n");
		return 0;
	}
	printf("MEASURED: maxPushDescriptors = %u\n", push_max);
	printf("  (plain layouts accept %u at the same binding, so the boundary is\n"
		"   specific to the push-descriptor flag)\n", plain_max);
	return 0;
}

int main(int argc, char **argv) {
	int push_mode = (argc > 1 && !strcmp(argv[1], "push"));
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkext",
		.apiVersion = VK_API_VERSION_1_0,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

	uint32_t n = 1;
	VkPhysicalDevice pd;
	CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd), "vkEnumeratePhysicalDevices");

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s  api %u.%u.%u  driverVersion 0x%x\n", props.deviceName,
		VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
		VK_VERSION_PATCH(props.apiVersion), props.driverVersion);

	uint32_t ec = 0;
	vkEnumerateDeviceExtensionProperties(pd, NULL, &ec, NULL);
	VkExtensionProperties *exts = calloc(ec, sizeof *exts);
	vkEnumerateDeviceExtensionProperties(pd, NULL, &ec, exts);
	printf("\n%u device extensions:\n", ec);
	for (uint32_t i = 0; i < ec; i++)
		printf("  %s (rev %u)\n", exts[i].extensionName, exts[i].specVersion);

	// Enable every extension that was just reported. If the shim's
	// vkCreateDevice filter is missing or wrong, this is where it shows: the
	// blob answers VK_ERROR_EXTENSION_NOT_PRESENT (-7) for the whole call.
	const char **names = calloc(ec, sizeof *names);
	for (uint32_t i = 0; i < ec; i++) names[i] = exts[i].extensionName;

	float prio = 1.0f;
	VkDeviceQueueCreateInfo q = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio,
	};
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &q,
		.enabledExtensionCount = ec, .ppEnabledExtensionNames = names,
	};
	VkDevice dev;
	printf("\nvkCreateDevice with all %u extensions enabled...\n", ec);
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice(all extensions)");
	printf("  PASS: device created\n");

	if (push_mode) {
		int r = probe_push_descriptors(pd, dev);
		vkDestroyDevice(dev, NULL);
		vkDestroyInstance(inst, NULL);
		return r;
	}

	// ---- VK_KHR_maintenance3 ------------------------------------------------
	PFN_vkGetPhysicalDeviceProperties2KHR gpdp2 =
		(PFN_vkGetPhysicalDeviceProperties2KHR)
		vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2KHR");
	if (gpdp2) {
		VkPhysicalDeviceMaintenance3PropertiesKHR m3 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES_KHR,
		};
		VkPhysicalDevicePushDescriptorPropertiesKHR pdp = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR,
			.pNext = &m3,
		};
		VkPhysicalDeviceProperties2KHR p2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
			.pNext = &pdp,
		};
		gpdp2(pd, &p2);
		printf("\nmaintenance3: maxPerSetDescriptors=%u maxMemoryAllocationSize=%llu\n",
			m3.maxPerSetDescriptors,
			(unsigned long long)m3.maxMemoryAllocationSize);
		// 0 means the shim is not filling it: debug.xdplus.vkpushlimit=0, or a
		// shim predating the measured floor.
		printf("push_descriptor: maxPushDescriptors=%u -> %s\n",
			pdp.maxPushDescriptors,
			pdp.maxPushDescriptors ? "PASS" : "not reported");
		if (!m3.maxPerSetDescriptors || !m3.maxMemoryAllocationSize)
			printf("  FAIL: zero -- properties not filled\n");
		else
			printf("  PASS\n");
	} else {
		printf("\nFAIL: no vkGetPhysicalDeviceProperties2KHR\n");
	}

	// ---- VK_KHR_get_memory_requirements2 + dedicated_allocation -------------
	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = 64 * 1024,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &buf), "vkCreateBuffer");

	VkMemoryRequirements req1;
	vkGetBufferMemoryRequirements(dev, buf, &req1);

	PFN_vkGetBufferMemoryRequirements2KHR gbmr2 =
		(PFN_vkGetBufferMemoryRequirements2KHR)
		vkGetDeviceProcAddr(dev, "vkGetBufferMemoryRequirements2KHR");
	printf("\nget_memory_requirements2: entry point %s\n", gbmr2 ? "resolved" : "MISSING");
	if (gbmr2) {
		VkMemoryDedicatedRequirementsKHR ded = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS_KHR,
			.prefersDedicatedAllocation = VK_TRUE,   // poisoned; must come back false
			.requiresDedicatedAllocation = VK_TRUE,
		};
		VkMemoryRequirements2KHR req2 = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2_KHR,
			.pNext = &ded,
		};
		VkBufferMemoryRequirementsInfo2KHR info = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2_KHR,
			.buffer = buf,
		};
		gbmr2(dev, &info, &req2);
		int same = req2.memoryRequirements.size == req1.size &&
			req2.memoryRequirements.alignment == req1.alignment &&
			req2.memoryRequirements.memoryTypeBits == req1.memoryTypeBits;
		printf("  1.0: size=%llu align=%llu bits=0x%x\n",
			(unsigned long long)req1.size, (unsigned long long)req1.alignment,
			req1.memoryTypeBits);
		printf("  2:   size=%llu align=%llu bits=0x%x -> %s\n",
			(unsigned long long)req2.memoryRequirements.size,
			(unsigned long long)req2.memoryRequirements.alignment,
			req2.memoryRequirements.memoryTypeBits, same ? "PASS" : "FAIL (mismatch)");
		printf("  dedicated: prefers=%u requires=%u -> %s\n",
			ded.prefersDedicatedAllocation, ded.requiresDedicatedAllocation,
			(!ded.prefersDedicatedAllocation && !ded.requiresDedicatedAllocation)
				? "PASS" : "FAIL (not written)");
	}

	// ---- VK_KHR_bind_memory2 ------------------------------------------------
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if (req1.memoryTypeBits & (1u << i)) { type = i; break; }
	if (type == UINT32_MAX) { printf("\nFAIL: no memory type for the buffer\n"); return 1; }

	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req1.size, .memoryTypeIndex = type,
	};
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &mem), "vkAllocateMemory");

	PFN_vkBindBufferMemory2KHR bbm2 = (PFN_vkBindBufferMemory2KHR)
		vkGetDeviceProcAddr(dev, "vkBindBufferMemory2KHR");
	printf("\nbind_memory2: entry point %s\n", bbm2 ? "resolved" : "MISSING");
	if (bbm2) {
		VkBindBufferMemoryInfoKHR bind = {
			.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO_KHR,
			.buffer = buf, .memory = mem, .memoryOffset = 0,
		};
		VkResult r = bbm2(dev, 1, &bind);
		printf("  vkBindBufferMemory2KHR -> %d %s\n", r,
			r == VK_SUCCESS ? "PASS" : "FAIL");
	}

	// ---- VK_KHR_maintenance3's query ---------------------------------------
	PFN_vkGetDescriptorSetLayoutSupportKHR gdsls =
		(PFN_vkGetDescriptorSetLayoutSupportKHR)
		vkGetDeviceProcAddr(dev, "vkGetDescriptorSetLayoutSupportKHR");
	printf("\nmaintenance3 query: entry point %s\n", gdsls ? "resolved" : "MISSING");
	if (gdsls) {
		VkDescriptorSetLayoutBinding b = {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
		VkDescriptorSetLayoutCreateInfo lci = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 1, .pBindings = &b,
		};
		VkDescriptorSetLayoutSupportKHR sup = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT_KHR,
		};
		gdsls(dev, &lci, &sup);
		printf("  1 uniform buffer supported=%u -> %s\n", sup.supported,
			sup.supported ? "PASS" : "FAIL");

		// And a layout that must NOT fit, to prove the answer is computed.
		b.descriptorCount = 1u << 24;
		gdsls(dev, &lci, &sup);
		printf("  16M descriptors supported=%u -> %s\n", sup.supported,
			sup.supported ? "FAIL (should be unsupported)" : "PASS");
	}

	vkFreeMemory(dev, mem, NULL);
	vkDestroyBuffer(dev, buf, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	printf("\ndone\n");
	return 0;
}
