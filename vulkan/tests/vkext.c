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

int main(void) {
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

	// ---- VK_KHR_maintenance3 ------------------------------------------------
	PFN_vkGetPhysicalDeviceProperties2KHR gpdp2 =
		(PFN_vkGetPhysicalDeviceProperties2KHR)
		vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2KHR");
	if (gpdp2) {
		VkPhysicalDeviceMaintenance3PropertiesKHR m3 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES_KHR,
		};
		VkPhysicalDeviceProperties2KHR p2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
			.pNext = &m3,
		};
		gpdp2(pd, &p2);
		printf("\nmaintenance3: maxPerSetDescriptors=%u maxMemoryAllocationSize=%llu\n",
			m3.maxPerSetDescriptors,
			(unsigned long long)m3.maxMemoryAllocationSize);
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
