// Functional maxPushDescriptors test.
//
// The blob supports VK_KHR_push_descriptor but cannot report its one limit
// (see ../README.md), and it validates nothing at object-creation time, so the
// limit cannot be found by probing vkCreateDescriptorSetLayout. This finds it
// by USE instead: push N descriptors, dispatch a compute shader that reads all
// N, and check the arithmetic. Below the limit the sum is exact; above it the
// driver is in undefined behaviour and the observable outcomes are a wrong sum,
// a fence that never signals, or VK_ERROR_DEVICE_LOST.
//
// Deliberately compute, not graphics: no swapchain, no display, nothing to
// leave the screen in a bad state if a dispatch goes wrong.
//
// ⚠️ Runs ONE N per process, by design. A device-lost poisons every later
// result in the same process, and a hang would take the whole run with it, so
// the caller loops and each N gets a clean device. See run.sh pushfunc.
//
//   vkpush <N> <shader.spv>
//
// Exit codes: 0 correct, 2 wrong result, 3 timeout/hang, 4 device lost,
// 5 setup failure (the probe's own problem, not the driver's).
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOT 256u             // per-UBO stride; checked against the real alignment
#define FENCE_TIMEOUT_NS 3000000000ull

#define SETUP(r, what) do { \
	VkResult _r = (r); \
	if (_r != VK_SUCCESS) { printf("SETUP-FAIL %s: %d\n", what, _r); return 5; } \
} while (0)

static uint32_t *load_spv(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *buf = malloc((size_t)n);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
	fclose(f);
	*len = (size_t)n;
	return buf;
}

static uint32_t pick_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) &&
			(mp.memoryTypes[i].propertyFlags & want) == want) return i;
	return UINT32_MAX;
}

int main(int argc, char **argv) {
	if (argc < 3) { printf("usage: vkpush <N> <shader.spv> [nopush]\n"); return 5; }
	uint32_t N = (uint32_t)strtoul(argv[1], NULL, 10);
	// The control. Same shader, same N bindings, ordinary descriptor set
	// instead of a pushed one -- so anything that fails in BOTH modes is not
	// about push descriptors. The USC compiler is the confound this exists
	// for: it fails "UF to HW" on enough separate UBO bindings regardless of
	// how they are bound.
	int use_push = 1, ssbo_mode = 0;
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "nopush")) use_push = 0;
		// Storage buffers instead of uniform buffers. Push descriptors accept
		// both, and the USC compiler's uniform-buffer ceiling (15, measured)
		// sits below any plausible maxPushDescriptors -- so the UBO form
		// cannot reach the limit being looked for, and this form can.
		else if (!strcmp(argv[i], "ssbo")) ssbo_mode = 1;
	}
	VkDescriptorType push_type = ssbo_mode
		? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	size_t spv_len = 0;
	uint32_t *spv = load_spv(argv[2], &spv_len);
	if (!spv) { printf("SETUP-FAIL cannot read %s\n", argv[2]); return 5; }

	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkpush", .apiVersion = VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	VkInstance inst;
	SETUP(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

	uint32_t one = 1;
	VkPhysicalDevice pd;
	SETUP(vkEnumeratePhysicalDevices(inst, &one, &pd), "vkEnumeratePhysicalDevices");

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	VkDeviceSize align = ssbo_mode ? props.limits.minStorageBufferOffsetAlignment
		: props.limits.minUniformBufferOffsetAlignment;
	if (align > SLOT) {
		printf("SETUP-FAIL min offset alignment %llu > slot %u\n",
			(unsigned long long)align, SLOT);
		return 5;
	}

	// One compute queue.
	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
	VkQueueFamilyProperties *qf = calloc(qn, sizeof *qf);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
	uint32_t qi = UINT32_MAX;
	for (uint32_t i = 0; i < qn; i++)
		if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qi = i; break; }
	if (qi == UINT32_MAX) { printf("SETUP-FAIL no compute queue\n"); return 5; }

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &prio };
	const char *dev_exts[] = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
		.enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_exts };
	VkDevice dev;
	SETUP(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice");

	PFN_vkCmdPushDescriptorSetKHR push = (PFN_vkCmdPushDescriptorSetKHR)
		vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
	if (!push) { printf("SETUP-FAIL no vkCmdPushDescriptorSetKHR\n"); return 5; }

	VkQueue queue;
	vkGetDeviceQueue(dev, qi, 0, &queue);

	// One uniform buffer holding N slots, plus one storage buffer for the sum.
	VkDeviceSize ubo_size = (VkDeviceSize)SLOT * N;
	VkBufferCreateInfo ubci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = ubo_size, .usage = ssbo_mode ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
			: VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	VkBuffer ubo;
	SETUP(vkCreateBuffer(dev, &ubci, NULL, &ubo), "vkCreateBuffer(ubo)");
	VkBufferCreateInfo sbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = sizeof(uint32_t), .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	VkBuffer ssbo;
	SETUP(vkCreateBuffer(dev, &sbci, NULL, &ssbo), "vkCreateBuffer(ssbo)");

	VkMemoryRequirements ur, sr;
	vkGetBufferMemoryRequirements(dev, ubo, &ur);
	vkGetBufferMemoryRequirements(dev, ssbo, &sr);
	VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t ut = pick_mem(pd, ur.memoryTypeBits, host);
	uint32_t st = pick_mem(pd, sr.memoryTypeBits, host);
	if (ut == UINT32_MAX || st == UINT32_MAX) { printf("SETUP-FAIL no host-visible memory\n"); return 5; }

	VkMemoryAllocateInfo uai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = ur.size, .memoryTypeIndex = ut };
	VkDeviceMemory umem;
	SETUP(vkAllocateMemory(dev, &uai, NULL, &umem), "vkAllocateMemory(ubo)");
	SETUP(vkBindBufferMemory(dev, ubo, umem, 0), "vkBindBufferMemory(ubo)");
	VkMemoryAllocateInfo sai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = sr.size, .memoryTypeIndex = st };
	VkDeviceMemory smem;
	SETUP(vkAllocateMemory(dev, &sai, NULL, &smem), "vkAllocateMemory(ssbo)");
	SETUP(vkBindBufferMemory(dev, ssbo, smem, 0), "vkBindBufferMemory(ssbo)");

	// Slot i carries i+1, so the shader's sum must be N*(N+1)/2.
	void *p;
	SETUP(vkMapMemory(dev, umem, 0, ubo_size, 0, &p), "vkMapMemory(ubo)");
	memset(p, 0, (size_t)ubo_size);
	for (uint32_t i = 0; i < N; i++)
		*(uint32_t *)((char *)p + (size_t)SLOT * i) = i + 1;
	vkUnmapMemory(dev, umem);
	SETUP(vkMapMemory(dev, smem, 0, sizeof(uint32_t), 0, &p), "vkMapMemory(ssbo)");
	*(uint32_t *)p = 0xdeadbeef;
	vkUnmapMemory(dev, smem);

	// set 0 = the push set: N uniform buffers. set 1 = the result, an ordinary set.
	// ONE binding carrying an array of N descriptors -- the shape apps actually
	// use with push descriptors, and the shape the USC compiler survives.
	// ⚠️ N separate bindings was tried first and is a dead end: the compiler
	// fails "UF to HW (UF_ERR_INTERNAL 0x8)" from 16 bindings up, in the
	// control path too, so it measures the compiler and not this limit.
	VkDescriptorSetLayoutBinding pb = {
		.binding = 0,
		.descriptorType = push_type,
		.descriptorCount = N,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	VkDescriptorSetLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.flags = use_push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
		.bindingCount = 1, .pBindings = &pb };
	VkDescriptorSetLayout pset;
	SETUP(vkCreateDescriptorSetLayout(dev, &plci, NULL, &pset), "push set layout");

	VkDescriptorSetLayoutBinding rb = { .binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo rlci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &rb };
	VkDescriptorSetLayout rset;
	SETUP(vkCreateDescriptorSetLayout(dev, &rlci, NULL, &rset), "result set layout");

	VkDescriptorPoolSize ps[2] = {
		{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 },
		{ .type = push_type, .descriptorCount = N },
	};
	VkDescriptorPoolCreateInfo pool_ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 2, .poolSizeCount = use_push ? 1 : 2, .pPoolSizes = ps };
	VkDescriptorPool pool;
	SETUP(vkCreateDescriptorPool(dev, &pool_ci, NULL, &pool), "descriptor pool");
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &rset };
	VkDescriptorSet rds;
	SETUP(vkAllocateDescriptorSets(dev, &dsai, &rds), "allocate result set");
	VkDescriptorBufferInfo rbi = { .buffer = ssbo, .offset = 0, .range = sizeof(uint32_t) };
	VkWriteDescriptorSet rw = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = rds, .dstBinding = 0, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &rbi };
	vkUpdateDescriptorSets(dev, 1, &rw, 0, NULL);

	VkDescriptorSetLayout sets[2] = { pset, rset };
	VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 2, .pSetLayouts = sets };
	VkPipelineLayout layout;
	SETUP(vkCreatePipelineLayout(dev, &plc, NULL, &layout), "pipeline layout");

	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = spv_len, .pCode = spv };
	VkShaderModule sm;
	SETUP(vkCreateShaderModule(dev, &smci, NULL, &sm), "shader module");
	VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
		.layout = layout };
	VkPipeline pipe;
	SETUP(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe),
		"compute pipeline");

	VkCommandPoolCreateInfo cpc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = qi };
	VkCommandPool cpool;
	SETUP(vkCreateCommandPool(dev, &cpc, NULL, &cpool), "command pool");
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	SETUP(vkAllocateCommandBuffers(dev, &cbai, &cb), "command buffer");

	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	SETUP(vkBeginCommandBuffer(cb, &bi), "begin");
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 1, 1, &rds, 0, NULL);

	// The measurement itself: N descriptors pushed in one call.
	VkDescriptorBufferInfo *bufs = calloc(N, sizeof *bufs);
	for (uint32_t i = 0; i < N; i++) {
		bufs[i].buffer = ubo;
		bufs[i].offset = (VkDeviceSize)SLOT * i;
		bufs[i].range = sizeof(uint32_t);
	}
	VkWriteDescriptorSet ws[1] = {{
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstBinding = 0, .dstArrayElement = 0,
		.descriptorCount = N,
		.descriptorType = push_type,
		.pBufferInfo = bufs,
	}};
	if (use_push) {
		push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, ws);
	} else {
		// Control path: the same N writes, into a real set, bound normally.
		VkDescriptorSetAllocateInfo pai = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &pset };
		VkDescriptorSet pds;
		SETUP(vkAllocateDescriptorSets(dev, &pai, &pds), "allocate control set");
		ws[0].dstSet = pds;
		vkUpdateDescriptorSets(dev, 1, ws, 0, NULL);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &pds, 0, NULL);
	}

	vkCmdDispatch(cb, 1, 1, 1);
	SETUP(vkEndCommandBuffer(cb), "end");

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	SETUP(vkCreateFence(dev, &fci, NULL, &fence), "fence");
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cb };
	VkResult sr2 = vkQueueSubmit(queue, 1, &si, fence);
	if (sr2 == VK_ERROR_DEVICE_LOST) { printf("N=%u DEVICE-LOST at submit\n", N); return 4; }
	if (sr2 != VK_SUCCESS) { printf("N=%u SUBMIT-FAIL %d\n", N, sr2); return 2; }

	// Bounded wait: an unbounded one would hang the probe along with the GPU.
	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS);
	if (wr == VK_TIMEOUT) { printf("N=%u HANG (fence unsignalled after 3s)\n", N); return 3; }
	if (wr == VK_ERROR_DEVICE_LOST) { printf("N=%u DEVICE-LOST while waiting\n", N); return 4; }
	if (wr != VK_SUCCESS) { printf("N=%u WAIT-FAIL %d\n", N, wr); return 2; }

	SETUP(vkMapMemory(dev, smem, 0, sizeof(uint32_t), 0, &p), "map result");
	uint32_t got = *(uint32_t *)p;
	vkUnmapMemory(dev, smem);
	uint64_t want = (uint64_t)N * (N + 1) / 2;
	if (got == want) {
		printf("N=%u OK (sum %u)\n", N, got);
		vkDestroyDevice(dev, NULL);
		vkDestroyInstance(inst, NULL);
		return 0;
	}
	printf("N=%u WRONG (sum %u, expected %llu)\n", N, got, (unsigned long long)want);
	return 2;
}
