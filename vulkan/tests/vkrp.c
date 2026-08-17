// Headless probe for vkshim's NULL-framebuffer render-pass guard.
//
// The blob dereferences VkRenderPassBeginInfo::framebuffer without testing it,
// so a null handle is a SIGSEGV. The guard drops the Begin and suppresses every
// command to the matching End. What this pins is that span and its END: a stale
// entry would swallow the next pass on the same buffer, which looks like a
// black frame rather than a failure.
//
//   ./run.sh rp            guard on: must survive and still render.
//   ./run.sh rp control    guard off: must SIGSEGV. ⚠️ Deliberate crash of this
//                          process; nothing is submitted, so no GPU work.
//
// Build + run: ./run.sh rp  (needs an NDK; see the script)
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHECK(r, what) do { \
	VkResult _r = (r); \
	if (_r != VK_SUCCESS) { printf("FAIL %s: VkResult %d\n", what, _r); return 1; } \
} while (0)

#define FB_W 64u
#define FB_H 64u

static int fails;

static void report(const char *what, int ok) {
	printf("  %-52s %s\n", what, ok ? "PASS" : "FAIL");
	if (!ok) fails++;
}

// Single-subpass pass over one colour attachment.
static VkResult make_render_pass(VkDevice dev, VkFormat fmt, VkRenderPass *out) {
	VkAttachmentDescription att = {
		.format = fmt,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference ref = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription sub = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &ref,
	};
	VkRenderPassCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &att,
		.subpassCount = 1, .pSubpasses = &sub,
	};
	return vkCreateRenderPass(dev, &ci, NULL, out);
}

static int memory_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return (int)i;
	return -1;
}

// vkCmdClearAttachments stands in for drawing: it needs an active pass but no
// pipeline, and the guard must suppress it when fb is VK_NULL_HANDLE.
static void record_pass(VkCommandBuffer cb, VkRenderPass rp, VkFramebuffer fb) {
	VkClearValue clear = { .color = { .float32 = { 0.f, 0.f, 0.f, 1.f } } };
	VkRenderPassBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp,
		.framebuffer = fb,
		.renderArea = { .offset = {0, 0}, .extent = { FB_W, FB_H } },
		.clearValueCount = 1, .pClearValues = &clear,
	};
	vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	VkClearAttachment ca = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.colorAttachment = 0,
		.clearValue = { .color = { .float32 = { 1.f, 0.f, 0.f, 1.f } } },
	};
	VkClearRect cr = {
		.rect = { .offset = {0, 0}, .extent = { FB_W, FB_H } },
		.baseArrayLayer = 0, .layerCount = 1,
	};
	vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
	vkCmdEndRenderPass(cb);
}

int main(int argc, char **argv) {
	int control = (argc > 1 && !strcmp(argv[1], "control"));
	if (control)
		printf("⚠️ control run: the guard must be OFF (debug.xdplus.vkrpguard=0)\n"
		       "   and this process is EXPECTED to SIGSEGV at the first null Begin.\n");

	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkrp",
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

	float prio = 1.0f;
	VkDeviceQueueCreateInfo q = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio,
	};
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &q,
	};
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice");
	VkQueue queue;
	vkGetDeviceQueue(dev, 0, 0, &queue);

	// ---- a real framebuffer, for the control half of every check -------------
	const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
	VkRenderPass rp;
	CHECK(make_render_pass(dev, fmt, &rp), "vkCreateRenderPass");

	VkImageCreateInfo ici2 = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = fmt,
		.extent = { FB_W, FB_H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkImage img;
	CHECK(vkCreateImage(dev, &ici2, NULL, &img), "vkCreateImage");
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	int mt = memory_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mt < 0) mt = memory_type(pd, mr.memoryTypeBits, 0);
	if (mt < 0) { printf("FAIL: no memory type for the attachment\n"); return 1; }
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt,
	};
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &mem), "vkAllocateMemory");
	CHECK(vkBindImageMemory(dev, img, mem, 0), "vkBindImageMemory");

	VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkImageView view;
	CHECK(vkCreateImageView(dev, &vci, NULL, &view), "vkCreateImageView");

	VkFramebufferCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rp, .attachmentCount = 1, .pAttachments = &view,
		.width = FB_W, .height = FB_H, .layers = 1,
	};
	VkFramebuffer fb;
	CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fb), "vkCreateFramebuffer");

	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = 0,
	};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool), "vkCreateCommandPool");
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 2,
	};
	VkCommandBuffer cbs[2];
	CHECK(vkAllocateCommandBuffers(dev, &cai, cbs), "vkAllocateCommandBuffers");

	VkCommandBufferBeginInfo cbbi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	printf("\n---- NULL-framebuffer render pass ----\n");

	// 1. Without the guard the process dies here, so reaching line 2 is the result.
	CHECK(vkBeginCommandBuffer(cbs[0], &cbbi), "vkBeginCommandBuffer(null pass)");
	record_pass(cbs[0], rp, VK_NULL_HANDLE);
	CHECK(vkEndCommandBuffer(cbs[0]), "vkEndCommandBuffer(null pass)");
	report("recorded a pass with a NULL framebuffer, survived", 1);
	if (control) {
		printf("  ⚠️ CONTROL DID NOT CRASH -- the guard is still on. Nothing was\n"
		       "     measured; set debug.xdplus.vkrpguard=0 and use a fresh process\n"
		       "     (the shim caches the property on first use).\n");
		return 1;
	}

	// 2. The pass was dropped, so this buffer is empty and must complete cleanly.
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cbs[0],
	};
	VkResult sr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
	report("submitting the suppressed pass returns VK_SUCCESS", sr == VK_SUCCESS);
	VkResult wr = vkQueueWaitIdle(queue);
	report("queue goes idle after it (no device-lost)", wr == VK_SUCCESS);

	// 3. Suppression must end at the matching End, learned from a crash.
	CHECK(vkBeginCommandBuffer(cbs[1], &cbbi), "vkBeginCommandBuffer(valid pass)");
	record_pass(cbs[1], rp, fb);
	CHECK(vkEndCommandBuffer(cbs[1]), "vkEndCommandBuffer(valid pass)");
	si.pCommandBuffers = &cbs[1];
	sr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
	wr = sr == VK_SUCCESS ? vkQueueWaitIdle(queue) : sr;
	report("a valid pass on another buffer still renders", wr == VK_SUCCESS);

	// 4. Same handle reused: a stale table entry would swallow this pass.
	CHECK(vkResetCommandBuffer(cbs[0], 0), "vkResetCommandBuffer");
	CHECK(vkBeginCommandBuffer(cbs[0], &cbbi), "vkBeginCommandBuffer(reused)");
	record_pass(cbs[0], rp, fb);
	CHECK(vkEndCommandBuffer(cbs[0]), "vkEndCommandBuffer(reused)");
	si.pCommandBuffers = &cbs[0];
	sr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
	wr = sr == VK_SUCCESS ? vkQueueWaitIdle(queue) : sr;
	report("the same buffer renders a valid pass after suppression", wr == VK_SUCCESS);

	// 5. Null then valid in one buffer -- the ordering PPSSPP produces.
	CHECK(vkResetCommandBuffer(cbs[0], 0), "vkResetCommandBuffer(interleaved)");
	CHECK(vkBeginCommandBuffer(cbs[0], &cbbi), "vkBeginCommandBuffer(interleaved)");
	record_pass(cbs[0], rp, VK_NULL_HANDLE);
	record_pass(cbs[0], rp, fb);
	CHECK(vkEndCommandBuffer(cbs[0]), "vkEndCommandBuffer(interleaved)");
	si.pCommandBuffers = &cbs[0];
	sr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
	wr = sr == VK_SUCCESS ? vkQueueWaitIdle(queue) : sr;
	report("null pass then valid pass on one buffer", wr == VK_SUCCESS);

	printf("\n⚠️ logcat should carry one 'suppressing render pass with NULL\n"
	       "   framebuffer' line -- the guard logs the first hit and then every\n"
	       "   512th, so two suppressions here produce exactly one line.\n");

	vkDestroyCommandPool(dev, pool, NULL);
	vkDestroyFramebuffer(dev, fb, NULL);
	vkDestroyImageView(dev, view, NULL);
	vkDestroyImage(dev, img, NULL);
	vkFreeMemory(dev, mem, NULL);
	vkDestroyRenderPass(dev, rp, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);

	printf("\n%s\n", fails ? "FAILURES ABOVE" : "all render-pass guard checks passed");
	return fails ? 1 : 0;
}
