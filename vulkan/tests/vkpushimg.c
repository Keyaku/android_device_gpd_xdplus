// Functional maxPushDescriptors test, combined image samplers in a FRAGMENT
// shader -- the half vkpush.c could not reach, since it measured storage
// buffers in compute and maxPushDescriptors is one number across all types and
// stages. Pushes N samplers, renders one pixel summing all N texels.
//
// Image i is 1x1 R8_UNORM holding i+1, so the answer is N*(N+1)/2 exactly.
// R32_SFLOAT attachment: an 8-bit one could not carry the sum.
//
// ⚠️ Headless graphics: 1x1 offscreen, no swapchain. ⚠️ ONE N per process, as
// in vkpush.c -- a device-lost poisons every later result. See pushimg.sh.
//
//   vkpushimg <N> <vert.spv> <frag.spv> [nopush] [destroy] [noidle]
//
// Exit codes: 0 correct, 2 wrong result, 3 timeout/hang, 4 device lost,
// 5 setup failure (the probe's own problem, not the driver's).
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
	VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags sstage, VkPipelineStageFlags dstage) {
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = src, .dstAccessMask = dst,
		.oldLayout = from, .newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	vkCmdPipelineBarrier(cb, sstage, dstage, 0, 0, NULL, 0, NULL, 1, &b);
}

int main(int argc, char **argv) {
	if (argc < 4) { printf("usage: vkpushimg <N> <vert.spv> <frag.spv> [nopush] [destroy] [noidle]\n"); return 5; }
	uint32_t N = (uint32_t)strtoul(argv[1], NULL, 10);
	// Control: same N through an ordinary set. A failure in BOTH modes is not
	// about push descriptors.
	int use_push = 1, wait_idle = 1, destroy = 0;
	for (int i = 4; i < argc; i++) {
		if (!strcmp(argv[i], "nopush")) use_push = 0;
		else if (!strcmp(argv[i], "noidle")) wait_idle = 0;
		else if (!strcmp(argv[i], "destroy")) destroy = 1;
	}
	// Unbuffered: a killed probe must still have delivered its result.
	setvbuf(stdout, NULL, _IONBF, 0);

	size_t vlen = 0, flen = 0;
	uint32_t *vspv = load_spv(argv[2], &vlen);
	uint32_t *fspv = load_spv(argv[3], &flen);
	if (!vspv || !fspv) { printf("SETUP-FAIL cannot read shaders\n"); return 5; }

	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkpushimg", .apiVersion = VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	VkInstance inst;
	SETUP(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

	uint32_t one = 1;
	VkPhysicalDevice pd;
	SETUP(vkEnumeratePhysicalDevices(inst, &one, &pd), "vkEnumeratePhysicalDevices");

	// Checked, not assumed: an unsupported format fails far from its cause.
	const VkFormat tex_fmt = VK_FORMAT_R8_UNORM;
	const VkFormat att_fmt = VK_FORMAT_R32_SFLOAT;
	VkFormatProperties fp;
	vkGetPhysicalDeviceFormatProperties(pd, tex_fmt, &fp);
	if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
		printf("SETUP-FAIL R8_UNORM not sampleable\n"); return 5;
	}
	vkGetPhysicalDeviceFormatProperties(pd, att_fmt, &fp);
	if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
		printf("SETUP-FAIL R32_SFLOAT not a colour attachment\n"); return 5;
	}

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
	VkQueueFamilyProperties *qf = calloc(qn, sizeof *qf);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
	uint32_t qi = UINT32_MAX;
	for (uint32_t i = 0; i < qn; i++)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qi = i; break; }
	if (qi == UINT32_MAX) { printf("SETUP-FAIL no graphics queue\n"); return 5; }

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

	// ---- N texture images, all out of ONE allocation --------------------------
	// One allocation, not N: the vendor suballocator null-derefs around 36-40 of
	// them, which is a confound unrelated to descriptors.
	VkImage *imgs = calloc(N, sizeof *imgs);
	VkImageView *views = calloc(N, sizeof *views);
	VkImageCreateInfo tci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = tex_fmt,
		.extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	for (uint32_t i = 0; i < N; i++)
		SETUP(vkCreateImage(dev, &tci, NULL, &imgs[i]), "vkCreateImage(texture)");

	VkMemoryRequirements tr;
	vkGetImageMemoryRequirements(dev, imgs[0], &tr);
	VkDeviceSize stride = (tr.size + tr.alignment - 1) / tr.alignment * tr.alignment;
	uint32_t tt = pick_mem(pd, tr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (tt == UINT32_MAX) tt = pick_mem(pd, tr.memoryTypeBits, 0);
	if (tt == UINT32_MAX) { printf("SETUP-FAIL no memory for textures\n"); return 5; }
	VkMemoryAllocateInfo tai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = stride * N, .memoryTypeIndex = tt };
	VkDeviceMemory tmem;
	SETUP(vkAllocateMemory(dev, &tai, NULL, &tmem), "vkAllocateMemory(textures)");
	for (uint32_t i = 0; i < N; i++) {
		SETUP(vkBindImageMemory(dev, imgs[i], tmem, stride * i), "vkBindImageMemory");
		VkImageViewCreateInfo vci = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = imgs[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = tex_fmt,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		SETUP(vkCreateImageView(dev, &vci, NULL, &views[i]), "vkCreateImageView");
	}

	// Texel i carries i+1, so the rendered sum must be N*(N+1)/2.
	VkBufferCreateInfo sbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = N, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	VkBuffer staging;
	SETUP(vkCreateBuffer(dev, &sbci, NULL, &staging), "vkCreateBuffer(staging)");
	VkMemoryRequirements sreq;
	vkGetBufferMemoryRequirements(dev, staging, &sreq);
	VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t st = pick_mem(pd, sreq.memoryTypeBits, host);
	if (st == UINT32_MAX) { printf("SETUP-FAIL no host-visible memory\n"); return 5; }
	VkMemoryAllocateInfo sai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = sreq.size, .memoryTypeIndex = st };
	VkDeviceMemory smem;
	SETUP(vkAllocateMemory(dev, &sai, NULL, &smem), "vkAllocateMemory(staging)");
	SETUP(vkBindBufferMemory(dev, staging, smem, 0), "vkBindBufferMemory(staging)");
	void *p;
	SETUP(vkMapMemory(dev, smem, 0, N, 0, &p), "vkMapMemory(staging)");
	for (uint32_t i = 0; i < N; i++) ((unsigned char *)p)[i] = (unsigned char)(i + 1);
	vkUnmapMemory(dev, smem);

	// ---- the 1x1 attachment and its readback buffer ---------------------------
	VkImageCreateInfo aci = tci;
	aci.format = att_fmt;
	aci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VkImage att;
	SETUP(vkCreateImage(dev, &aci, NULL, &att), "vkCreateImage(attachment)");
	VkMemoryRequirements ar;
	vkGetImageMemoryRequirements(dev, att, &ar);
	uint32_t at = pick_mem(pd, ar.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (at == UINT32_MAX) at = pick_mem(pd, ar.memoryTypeBits, 0);
	VkMemoryAllocateInfo aai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = ar.size, .memoryTypeIndex = at };
	VkDeviceMemory amem;
	SETUP(vkAllocateMemory(dev, &aai, NULL, &amem), "vkAllocateMemory(attachment)");
	SETUP(vkBindImageMemory(dev, att, amem, 0), "vkBindImageMemory(attachment)");
	VkImageViewCreateInfo avci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = att, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = att_fmt,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView aview;
	SETUP(vkCreateImageView(dev, &avci, NULL, &aview), "vkCreateImageView(attachment)");

	VkBufferCreateInfo rbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = sizeof(float), .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	VkBuffer rbuf;
	SETUP(vkCreateBuffer(dev, &rbci, NULL, &rbuf), "vkCreateBuffer(result)");
	VkMemoryRequirements rr;
	vkGetBufferMemoryRequirements(dev, rbuf, &rr);
	uint32_t rt = pick_mem(pd, rr.memoryTypeBits, host);
	VkMemoryAllocateInfo rai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = rr.size, .memoryTypeIndex = rt };
	VkDeviceMemory rmem;
	SETUP(vkAllocateMemory(dev, &rai, NULL, &rmem), "vkAllocateMemory(result)");
	SETUP(vkBindBufferMemory(dev, rbuf, rmem, 0), "vkBindBufferMemory(result)");

	// ---- descriptors, pass, pipeline -----------------------------------------
	VkSamplerCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxLod = 0.0f };
	VkSampler sampler;
	SETUP(vkCreateSampler(dev, &sci, NULL, &sampler), "vkCreateSampler");

	// One binding, array of N: N separate bindings measures the compiler.
	VkDescriptorSetLayoutBinding pb = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = N,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
	VkDescriptorSetLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.flags = use_push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
		.bindingCount = 1, .pBindings = &pb };
	VkDescriptorSetLayout pset;
	SETUP(vkCreateDescriptorSetLayout(dev, &plci, NULL, &pset), "push set layout");

	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet pds = VK_NULL_HANDLE;
	if (!use_push) {
		VkDescriptorPoolSize ps = {
			.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = N };
		VkDescriptorPoolCreateInfo pool_ci = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
		SETUP(vkCreateDescriptorPool(dev, &pool_ci, NULL, &pool), "descriptor pool");
		VkDescriptorSetAllocateInfo dsai = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &pset };
		SETUP(vkAllocateDescriptorSets(dev, &dsai, &pds), "allocate control set");
	}

	VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &pset };
	VkPipelineLayout layout;
	SETUP(vkCreatePipelineLayout(dev, &plc, NULL, &layout), "pipeline layout");

	VkAttachmentDescription ad = {
		.format = att_fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference aref = { .attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &aref };
	VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sub };
	VkRenderPass rp;
	SETUP(vkCreateRenderPass(dev, &rpci, NULL, &rp), "vkCreateRenderPass");
	VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rp, .attachmentCount = 1, .pAttachments = &aview,
		.width = 1, .height = 1, .layers = 1 };
	VkFramebuffer fb;
	SETUP(vkCreateFramebuffer(dev, &fbci, NULL, &fb), "vkCreateFramebuffer");

	VkShaderModuleCreateInfo vsm = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = vlen, .pCode = vspv };
	VkShaderModuleCreateInfo fsm = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = flen, .pCode = fspv };
	VkShaderModule vmod, fmod;
	SETUP(vkCreateShaderModule(dev, &vsm, NULL, &vmod), "vertex module");
	SETUP(vkCreateShaderModule(dev, &fsm, NULL, &fmod), "fragment module");

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vmod, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fmod, .pName = "main" } };
	// No vertex buffers: the shader builds a covering triangle from its index.
	VkPipelineVertexInputStateCreateInfo vin = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { .x = 0, .y = 0, .width = 1, .height = 1, .minDepth = 0, .maxDepth = 1 };
	VkRect2D sc = { .offset = {0, 0}, .extent = {1, 1} };
	VkPipelineViewportStateCreateInfo vps = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = VK_COLOR_COMPONENT_R_BIT };
	VkPipelineColorBlendStateCreateInfo cb_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkGraphicsPipelineCreateInfo gpci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = stages, .pVertexInputState = &vin,
		.pInputAssemblyState = &ia, .pViewportState = &vps,
		.pRasterizationState = &rs, .pMultisampleState = &ms,
		.pColorBlendState = &cb_state, .layout = layout, .renderPass = rp, .subpass = 0 };
	VkPipeline pipe;
	VkResult pr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
	if (pr != VK_SUCCESS) {
		// Not a SETUP failure: this is the USC compiler answering, i.e. a result.
		printf("N=%u PIPELINE-FAIL %d (shader compiler refused it)\n", N, pr);
		return 2;
	}

	VkDescriptorImageInfo *dii = calloc(N, sizeof *dii);
	for (uint32_t i = 0; i < N; i++) {
		dii[i].sampler = sampler;
		dii[i].imageView = views[i];
		dii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
	VkWriteDescriptorSet ws = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstBinding = 0, .dstArrayElement = 0, .descriptorCount = N,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = dii };
	if (!use_push) {
		ws.dstSet = pds;
		vkUpdateDescriptorSets(dev, 1, &ws, 0, NULL);
	}

	// ---- one command buffer: upload, render, read back ------------------------
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
	for (uint32_t i = 0; i < N; i++) {
		barrier(cb, imgs[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy bic = {
			.bufferOffset = i,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = {1, 1, 1} };
		vkCmdCopyBufferToImage(cb, staging, imgs[i],
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
		barrier(cb, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	VkClearValue clear = { .color = { .float32 = { -1.f, 0.f, 0.f, 0.f } } };
	VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp, .framebuffer = fb,
		.renderArea = { .offset = {0, 0}, .extent = {1, 1} },
		.clearValueCount = 1, .pClearValues = &clear };
	vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	if (use_push)
		push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &ws);
	else
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &pds, 0, NULL);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);

	VkBufferImageCopy out = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = {1, 1, 1} };
	vkCmdCopyImageToBuffer(cb, att, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &out);
	SETUP(vkEndCommandBuffer(cb), "end");

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	SETUP(vkCreateFence(dev, &fci, NULL, &fence), "fence");
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cb };
	VkResult sr = vkQueueSubmit(queue, 1, &si, fence);
	if (sr == VK_ERROR_DEVICE_LOST) { printf("N=%u DEVICE-LOST at submit\n", N); return 4; }
	if (sr != VK_SUCCESS) { printf("N=%u SUBMIT-FAIL %d\n", N, sr); return 2; }

	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS);
	if (wr == VK_TIMEOUT) { printf("N=%u HANG (fence unsignalled after 3s)\n", N); return 3; }
	if (wr == VK_ERROR_DEVICE_LOST) { printf("N=%u DEVICE-LOST while waiting\n", N); return 4; }
	if (wr != VK_SUCCESS) { printf("N=%u WAIT-FAIL %d\n", N, wr); return 2; }

	SETUP(vkMapMemory(dev, rmem, 0, sizeof(float), 0, &p), "map result");
	float got = *(float *)p;
	vkUnmapMemory(dev, rmem);
	double want = (double)N * (N + 1) / 2;
	// Slack of half a unit: rounding must not read as a descriptor failure.
	int ok = got > want - 0.5 && got < want + 0.5;
	if (ok) printf("N=%u OK (sum %.0f)\n", N, (double)got);
	else printf("N=%u WRONG (sum %.3f, expected %.0f)\n", N, (double)got, want);

	// ⚠️ Result first, teardown opt-in: after a real draw the blob's
	// vkDestroyDevice parks in RGXDestroyGlobalPB and does not return.
	// `destroy` reproduces that; the default _exits and lets the kernel reclaim.
	if (!destroy) {
		fflush(stdout);
		_exit(ok ? 0 : 2);
	}
	if (wait_idle) vkDeviceWaitIdle(dev);
	printf("  tearing down (%s)...\n", wait_idle ? "after vkDeviceWaitIdle" : "no device-idle");
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	printf("  teardown returned\n");
	return ok ? 0 : 2;
}
