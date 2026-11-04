/*
 * vkpresent.c - Vulkan presenter for the FFE 320x200 framebuffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkpresent.h"

#define FRAMES_IN_FLIGHT 2
#define MAX_SWAP_IMAGES 8

typedef struct {
	VkBuffer staging;
	VkDeviceMemory staging_mem;
	void *staging_ptr;
	VkCommandBuffer cmd;
	VkFence fence;
	VkSemaphore acquired;
} Frame;

static struct {
	int ok;
	VkpConfig cfg;
	VkInstance instance;
	VkSurfaceKHR surface;
	VkPhysicalDevice gpu;
	VkPhysicalDeviceProperties gpu_props;
	VkPhysicalDeviceMemoryProperties memprops;
	uint32_t qfamily;
	VkDevice device;
	VkQueue queue;
	VkCommandPool pool;

	VkSwapchainKHR swapchain;
	VkFormat swap_format;
	VkExtent2D swap_extent;
	uint32_t swap_count;
	VkImage swap_images[MAX_SWAP_IMAGES];
	VkSemaphore rendered[MAX_SWAP_IMAGES];
	int swap_dirty;
	uint32_t win_w, win_h;

	VkImage src_image;
	VkDeviceMemory src_mem;
	int src_w, src_h;
	int src_initialized;

	Frame frames[FRAMES_IN_FLIGHT];
	uint32_t frame_index;
	char error[512];
} vk;

static void set_error(const char *what, VkResult r)
{
	snprintf(vk.error, sizeof vk.error, "Vulkan: %s failed (VkResult %d)", what, (int)r);
	fprintf(stderr, "%s\n", vk.error);
}

const char *VkpGetError(void) { return vk.error; }
const char *VkpGetDeviceName(void) { return vk.gpu ? vk.gpu_props.deviceName : "none"; }

static int find_memory(uint32_t type_bits, VkMemoryPropertyFlags flags, uint32_t *out)
{
	uint32_t i;
	for (i = 0; i < vk.memprops.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) && (vk.memprops.memoryTypes[i].propertyFlags & flags) == flags) {
			*out = i;
			return 1;
		}
	}
	return 0;
}

static int create_instance(const char *const *exts, uint32_t next)
{
	VkApplicationInfo app;
	VkInstanceCreateInfo ci;
	VkResult r;
	const char *layers[1] = { "VK_LAYER_KHRONOS_validation" };
	uint32_t nlayers = 0;

	if (vk.cfg.validation) {
		uint32_t n = 0, i;
		VkLayerProperties *lp;
		vkEnumerateInstanceLayerProperties(&n, NULL);
		lp = (VkLayerProperties *)calloc(n ? n : 1, sizeof *lp);
		vkEnumerateInstanceLayerProperties(&n, lp);
		for (i = 0; i < n; i++) if (!strcmp(lp[i].layerName, layers[0])) nlayers = 1;
		free(lp);
	}

	memset(&app, 0, sizeof app);
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "JJFFE";
	app.applicationVersion = VK_MAKE_VERSION(2, 8, 64);
	app.pEngineName = "FFE-Vulkan";
	app.engineVersion = 1;
	app.apiVersion = VK_API_VERSION_1_0;

	memset(&ci, 0, sizeof ci);
	ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ci.pApplicationInfo = &app;
	ci.enabledExtensionCount = next;
	ci.ppEnabledExtensionNames = exts;
	ci.enabledLayerCount = nlayers;
	ci.ppEnabledLayerNames = layers;
	r = vkCreateInstance(&ci, NULL, &vk.instance);
	if (r != VK_SUCCESS) { set_error("vkCreateInstance", r); return 0; }
	volkLoadInstance(vk.instance);
	return 1;
}

static int pick_gpu(void)
{
	uint32_t n = 0, i, best_score = 0;
	VkPhysicalDevice *devs;
	vkEnumeratePhysicalDevices(vk.instance, &n, NULL);
	if (!n) { snprintf(vk.error, sizeof vk.error, "Vulkan: no GPU with Vulkan support found"); return 0; }
	devs = (VkPhysicalDevice *)calloc(n, sizeof *devs);
	vkEnumeratePhysicalDevices(vk.instance, &n, devs);
	for (i = 0; i < n; i++) {
		VkPhysicalDeviceProperties p;
		uint32_t nq = 0, q, score;
		VkQueueFamilyProperties *qp;
		uint32_t ne = 0, e;
		VkExtensionProperties *ep;
		int has_swapchain = 0;

		vkGetPhysicalDeviceProperties(devs[i], &p);
		vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, NULL);
		ep = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof *ep);
		vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, ep);
		for (e = 0; e < ne; e++) if (!strcmp(ep[e].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) has_swapchain = 1;
		free(ep);
		if (!has_swapchain) continue;

		vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, NULL);
		qp = (VkQueueFamilyProperties *)calloc(nq ? nq : 1, sizeof *qp);
		vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qp);
		for (q = 0; q < nq; q++) {
			VkBool32 present = VK_FALSE;
			if (!(qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
			vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, vk.surface, &present);
			if (!present) continue;
			score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4 :
			        p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3 :
			        p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 2 : 1;
			if (score > best_score) {
				best_score = score;
				vk.gpu = devs[i];
				vk.qfamily = q;
				vk.gpu_props = p;
			}
			break;
		}
		free(qp);
	}
	free(devs);
	if (!vk.gpu) { snprintf(vk.error, sizeof vk.error, "Vulkan: no GPU can present to this window"); return 0; }
	vkGetPhysicalDeviceMemoryProperties(vk.gpu, &vk.memprops);
	return 1;
}

static int create_device(void)
{
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci;
	VkDeviceCreateInfo dci;
	VkCommandPoolCreateInfo pci;
	const char *exts[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkResult r;

	memset(&qci, 0, sizeof qci);
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = vk.qfamily;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;

	memset(&dci, 0, sizeof dci);
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = exts;
	r = vkCreateDevice(vk.gpu, &dci, NULL, &vk.device);
	if (r != VK_SUCCESS) { set_error("vkCreateDevice", r); return 0; }
	volkLoadDevice(vk.device);
	vkGetDeviceQueue(vk.device, vk.qfamily, 0, &vk.queue);

	memset(&pci, 0, sizeof pci);
	pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = vk.qfamily;
	r = vkCreateCommandPool(vk.device, &pci, NULL, &vk.pool);
	if (r != VK_SUCCESS) { set_error("vkCreateCommandPool", r); return 0; }
	return 1;
}

static int create_frames(void)
{
	int i;
	for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
		Frame *f = &vk.frames[i];
		VkCommandBufferAllocateInfo ai;
		VkFenceCreateInfo fci;
		VkSemaphoreCreateInfo sci;
		VkResult r;

		memset(&ai, 0, sizeof ai);
		ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		ai.commandPool = vk.pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		r = vkAllocateCommandBuffers(vk.device, &ai, &f->cmd);
		if (r != VK_SUCCESS) { set_error("vkAllocateCommandBuffers", r); return 0; }

		memset(&fci, 0, sizeof fci);
		fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		r = vkCreateFence(vk.device, &fci, NULL, &f->fence);
		if (r != VK_SUCCESS) { set_error("vkCreateFence", r); return 0; }

		memset(&sci, 0, sizeof sci);
		sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		r = vkCreateSemaphore(vk.device, &sci, NULL, &f->acquired);
		if (r != VK_SUCCESS) { set_error("vkCreateSemaphore", r); return 0; }
	}
	return 1;
}

static void destroy_source(void)
{
	int i;
	for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
		Frame *f = &vk.frames[i];
		if (f->staging) vkDestroyBuffer(vk.device, f->staging, NULL);
		if (f->staging_mem) vkFreeMemory(vk.device, f->staging_mem, NULL);
		f->staging = VK_NULL_HANDLE; f->staging_mem = VK_NULL_HANDLE; f->staging_ptr = NULL;
	}
	if (vk.src_image) vkDestroyImage(vk.device, vk.src_image, NULL);
	if (vk.src_mem) vkFreeMemory(vk.device, vk.src_mem, NULL);
	vk.src_image = VK_NULL_HANDLE; vk.src_mem = VK_NULL_HANDLE;
	vk.src_w = vk.src_h = 0;
}

static int create_source(int w, int h)
{
	VkImageCreateInfo ici;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo mai;
	uint32_t mt;
	VkResult r;
	int i;

	destroy_source();
	memset(&ici, 0, sizeof ici);
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent.width = (uint32_t)w;
	ici.extent.height = (uint32_t)h;
	ici.extent.depth = 1;
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	r = vkCreateImage(vk.device, &ici, NULL, &vk.src_image);
	if (r != VK_SUCCESS) { set_error("vkCreateImage", r); return 0; }
	vkGetImageMemoryRequirements(vk.device, vk.src_image, &req);
	if (!find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mt) &&
	    !find_memory(req.memoryTypeBits, 0, &mt)) { set_error("find image memory", VK_ERROR_OUT_OF_DEVICE_MEMORY); return 0; }
	memset(&mai, 0, sizeof mai);
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = mt;
	r = vkAllocateMemory(vk.device, &mai, NULL, &vk.src_mem);
	if (r != VK_SUCCESS) { set_error("vkAllocateMemory(image)", r); return 0; }
	vkBindImageMemory(vk.device, vk.src_image, vk.src_mem, 0);

	for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
		Frame *f = &vk.frames[i];
		VkBufferCreateInfo bci;
		memset(&bci, 0, sizeof bci);
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = (VkDeviceSize)w * (VkDeviceSize)h * 4;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		r = vkCreateBuffer(vk.device, &bci, NULL, &f->staging);
		if (r != VK_SUCCESS) { set_error("vkCreateBuffer", r); return 0; }
		vkGetBufferMemoryRequirements(vk.device, f->staging, &req);
		if (!find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mt)) {
			set_error("find staging memory", VK_ERROR_OUT_OF_HOST_MEMORY); return 0;
		}
		memset(&mai, 0, sizeof mai);
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = mt;
		r = vkAllocateMemory(vk.device, &mai, NULL, &f->staging_mem);
		if (r != VK_SUCCESS) { set_error("vkAllocateMemory(staging)", r); return 0; }
		vkBindBufferMemory(vk.device, f->staging, f->staging_mem, 0);
		r = vkMapMemory(vk.device, f->staging_mem, 0, VK_WHOLE_SIZE, 0, &f->staging_ptr);
		if (r != VK_SUCCESS) { set_error("vkMapMemory", r); return 0; }
	}
	vk.src_w = w;
	vk.src_h = h;
	vk.src_initialized = 0;
	return 1;
}

static void destroy_swapchain(void)
{
	uint32_t i;
	for (i = 0; i < vk.swap_count; i++) {
		if (vk.rendered[i]) vkDestroySemaphore(vk.device, vk.rendered[i], NULL);
		vk.rendered[i] = VK_NULL_HANDLE;
	}
	if (vk.swapchain) vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);
	vk.swapchain = VK_NULL_HANDLE;
	vk.swap_count = 0;
}

/* returns 1 ok, 0 error, -1 window minimized (try later) */
static int create_swapchain(void)
{
	VkSurfaceCapabilitiesKHR caps;
	VkSurfaceFormatKHR *formats;
	VkPresentModeKHR *modes, mode = VK_PRESENT_MODE_FIFO_KHR;
	VkSwapchainCreateInfoKHR sci;
	VkSwapchainKHR old = vk.swapchain;
	VkFormatProperties fp;
	uint32_t nf = 0, nm = 0, i, count;
	VkResult r;

	vkDeviceWaitIdle(vk.device);
	r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, vk.surface, &caps);
	if (r != VK_SUCCESS) { set_error("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r); return 0; }
	if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
		snprintf(vk.error, sizeof vk.error, "Vulkan: swapchain images cannot be transfer destinations");
		return 0;
	}

	if (caps.currentExtent.width != 0xFFFFFFFFu) vk.swap_extent = caps.currentExtent;
	else {
		vk.swap_extent.width = vk.win_w;
		vk.swap_extent.height = vk.win_h;
		if (vk.swap_extent.width < caps.minImageExtent.width) vk.swap_extent.width = caps.minImageExtent.width;
		if (vk.swap_extent.height < caps.minImageExtent.height) vk.swap_extent.height = caps.minImageExtent.height;
		if (vk.swap_extent.width > caps.maxImageExtent.width) vk.swap_extent.width = caps.maxImageExtent.width;
		if (vk.swap_extent.height > caps.maxImageExtent.height) vk.swap_extent.height = caps.maxImageExtent.height;
	}
	if (vk.swap_extent.width == 0 || vk.swap_extent.height == 0) return -1;

	vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &nf, NULL);
	formats = (VkSurfaceFormatKHR *)calloc(nf ? nf : 1, sizeof *formats);
	vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &nf, formats);
	vk.swap_format = VK_FORMAT_UNDEFINED;
	for (i = 0; i < nf; i++) {
		if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
			vk.swap_format = formats[i].format;
			break;
		}
	}
	if (vk.swap_format == VK_FORMAT_UNDEFINED && nf > 0) vk.swap_format = formats[0].format;
	{
		VkColorSpaceKHR cs = nf ? formats[0].colorSpace : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		for (i = 0; i < nf; i++) if (formats[i].format == vk.swap_format) { cs = formats[i].colorSpace; break; }
		free(formats);

		vkGetPhysicalDeviceFormatProperties(vk.gpu, vk.swap_format, &fp);
		if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
			snprintf(vk.error, sizeof vk.error, "Vulkan: swapchain format %d does not support blits", (int)vk.swap_format);
			return 0;
		}

		vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, vk.surface, &nm, NULL);
		modes = (VkPresentModeKHR *)calloc(nm ? nm : 1, sizeof *modes);
		vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, vk.surface, &nm, modes);
		if (!vk.cfg.vsync) {
			for (i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
			if (mode == VK_PRESENT_MODE_FIFO_KHR)
				for (i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
		}
		free(modes);

		count = caps.minImageCount + 1;
		if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
		if (count > MAX_SWAP_IMAGES) count = MAX_SWAP_IMAGES;

		memset(&sci, 0, sizeof sci);
		sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		sci.surface = vk.surface;
		sci.minImageCount = count;
		sci.imageFormat = vk.swap_format;
		sci.imageColorSpace = cs;
		sci.imageExtent = vk.swap_extent;
		sci.imageArrayLayers = 1;
		sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ?
			VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
		sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
			VkCompositeAlphaFlagBitsKHR opts[3] = { VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
				VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR };
			for (i = 0; i < 3; i++) if (caps.supportedCompositeAlpha & opts[i]) { sci.compositeAlpha = opts[i]; break; }
		}
		sci.presentMode = mode;
		sci.clipped = VK_TRUE;
		sci.oldSwapchain = old;
	}
	r = vkCreateSwapchainKHR(vk.device, &sci, NULL, &vk.swapchain);
	if (old) {
		uint32_t k;
		for (k = 0; k < vk.swap_count; k++) {
			if (vk.rendered[k]) vkDestroySemaphore(vk.device, vk.rendered[k], NULL);
			vk.rendered[k] = VK_NULL_HANDLE;
		}
		vkDestroySwapchainKHR(vk.device, old, NULL);
	}
	if (r != VK_SUCCESS) { vk.swapchain = VK_NULL_HANDLE; vk.swap_count = 0; set_error("vkCreateSwapchainKHR", r); return 0; }

	vk.swap_count = MAX_SWAP_IMAGES;
	vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.swap_count, vk.swap_images);
	for (i = 0; i < vk.swap_count; i++) {
		VkSemaphoreCreateInfo semci;
		memset(&semci, 0, sizeof semci);
		semci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		vkCreateSemaphore(vk.device, &semci, NULL, &vk.rendered[i]);
	}
	vk.swap_dirty = 0;
	return 1;
}

int VkpInit(const char *const *instance_exts, uint32_t ext_count,
            VkpCreateSurfaceFn create_surface, void *user,
            const VkpConfig *cfg, uint32_t win_w, uint32_t win_h)
{
	VkResult r;
	memset(&vk, 0, sizeof vk);
	vk.cfg = *cfg;
	if (vk.cfg.aspect <= 0.0) vk.cfg.aspect = 1.6;
	vk.win_w = win_w;
	vk.win_h = win_h;

	r = volkInitialize();
	if (r != VK_SUCCESS) {
		snprintf(vk.error, sizeof vk.error, "Vulkan loader not found - install a GPU driver with Vulkan support");
		return 0;
	}
	if (!create_instance(instance_exts, ext_count)) return 0;
	r = create_surface(vk.instance, user, &vk.surface);
	if (r != VK_SUCCESS) { set_error("surface creation", r); return 0; }
	if (!pick_gpu()) return 0;
	if (!create_device()) return 0;
	if (!create_frames()) return 0;
	if (!create_source(320, 200)) return 0;
	if (create_swapchain() == 0) return 0;
	vk.ok = 1;
	return 1;
}

void VkpResize(uint32_t w, uint32_t h)
{
	vk.win_w = w;
	vk.win_h = h;
	vk.swap_dirty = 1;
}

static void image_barrier(VkCommandBuffer cmd, VkImage img,
                          VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b;
	memset(&b, 0, sizeof b);
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

void VkpPresent(const uint32_t *rgba, int w, int h)
{
	Frame *f;
	uint32_t img;
	VkResult r;
	VkCommandBufferBeginInfo bi;
	VkBufferImageCopy copy;
	VkClearColorValue black;
	VkImageSubresourceRange range;
	VkImageBlit blit;
	VkSubmitInfo si;
	VkPresentInfoKHR pi;
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	int32_t dw, dh, dx, dy;

	if (!vk.ok) return;
	if (vk.swap_dirty || !vk.swapchain) {
		int s = create_swapchain();
		if (s <= 0) return;   /* minimized or failed: drop frame */
	}
	if (w != vk.src_w || h != vk.src_h) {
		vkDeviceWaitIdle(vk.device);
		if (!create_source(w, h)) { vk.ok = 0; return; }
	}

	f = &vk.frames[vk.frame_index];
	vkWaitForFences(vk.device, 1, &f->fence, VK_TRUE, UINT64_MAX);

	r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &img);
	if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk.swap_dirty = 1; return; }
	if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { set_error("vkAcquireNextImageKHR", r); return; }
	vkResetFences(vk.device, 1, &f->fence);

	memcpy(f->staging_ptr, rgba, (size_t)w * (size_t)h * 4);

	vkResetCommandBuffer(f->cmd, 0);
	memset(&bi, 0, sizeof bi);
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(f->cmd, &bi);

	/* upload the frame */
	image_barrier(f->cmd, vk.src_image,
		vk.src_initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	memset(&copy, 0, sizeof copy);
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = (uint32_t)w;
	copy.imageExtent.height = (uint32_t)h;
	copy.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(f->cmd, f->staging, vk.src_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	image_barrier(f->cmd, vk.src_image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vk.src_initialized = 1;

	/* clear swapchain image and scale the frame into it */
	image_barrier(f->cmd, vk.swap_images[img],
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		0, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	memset(&black, 0, sizeof black);
	black.float32[3] = 1.0f;
	memset(&range, 0, sizeof range);
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	vkCmdClearColorImage(f->cmd, vk.swap_images[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	image_barrier(f->cmd, vk.swap_images[img],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	/* aspect-correct destination rectangle */
	dw = (int32_t)vk.swap_extent.width;
	dh = (int32_t)((double)dw / vk.cfg.aspect + 0.5);
	if (dh > (int32_t)vk.swap_extent.height) {
		dh = (int32_t)vk.swap_extent.height;
		dw = (int32_t)((double)dh * vk.cfg.aspect + 0.5);
	}
	dx = ((int32_t)vk.swap_extent.width - dw) / 2;
	dy = ((int32_t)vk.swap_extent.height - dh) / 2;

	memset(&blit, 0, sizeof blit);
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[1].x = w;
	blit.srcOffsets[1].y = h;
	blit.srcOffsets[1].z = 1;
	blit.dstSubresource = blit.srcSubresource;
	blit.dstOffsets[0].x = dx;
	blit.dstOffsets[0].y = dy;
	blit.dstOffsets[1].x = dx + dw;
	blit.dstOffsets[1].y = dy + dh;
	blit.dstOffsets[1].z = 1;
	vkCmdBlitImage(f->cmd, vk.src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		vk.swap_images[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
		vk.cfg.linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

	image_barrier(f->cmd, vk.swap_images[img],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_ACCESS_TRANSFER_WRITE_BIT, 0,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	vkEndCommandBuffer(f->cmd);

	memset(&si, 0, sizeof si);
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.waitSemaphoreCount = 1;
	si.pWaitSemaphores = &f->acquired;
	si.pWaitDstStageMask = &wait_stage;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &f->cmd;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &vk.rendered[img];
	r = vkQueueSubmit(vk.queue, 1, &si, f->fence);
	if (r != VK_SUCCESS) { set_error("vkQueueSubmit", r); vk.ok = 0; return; }

	memset(&pi, 0, sizeof pi);
	pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &vk.rendered[img];
	pi.swapchainCount = 1;
	pi.pSwapchains = &vk.swapchain;
	pi.pImageIndices = &img;
	r = vkQueuePresentKHR(vk.queue, &pi);
	if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk.swap_dirty = 1;
	else if (r != VK_SUCCESS) set_error("vkQueuePresentKHR", r);

	vk.frame_index = (vk.frame_index + 1) % FRAMES_IN_FLIGHT;
}

void VkpShutdown(void)
{
	int i;
	if (!vk.instance) return;
	if (vk.device) {
		vkDeviceWaitIdle(vk.device);
		destroy_source();
		destroy_swapchain();
		for (i = 0; i < FRAMES_IN_FLIGHT; i++) {
			if (vk.frames[i].fence) vkDestroyFence(vk.device, vk.frames[i].fence, NULL);
			if (vk.frames[i].acquired) vkDestroySemaphore(vk.device, vk.frames[i].acquired, NULL);
		}
		if (vk.pool) vkDestroyCommandPool(vk.device, vk.pool, NULL);
		vkDestroyDevice(vk.device, NULL);
	}
	if (vk.surface) vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
	vkDestroyInstance(vk.instance, NULL);
	memset(&vk, 0, sizeof vk);
}
