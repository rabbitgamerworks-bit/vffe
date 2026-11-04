/*
 * vkpresent.h - Vulkan presenter for the 320x200 FFE framebuffer.
 *
 * Uploads each frame into a texture and scales it into the swapchain with
 * vkCmdBlitImage (aspect-correct, letterboxed).  The Vulkan loader is
 * resolved at runtime through volk, so no vulkan-1.lib is needed at link
 * time; the game only needs a Vulkan driver installed.
 */
#ifndef VKPRESENT_H
#define VKPRESENT_H

#include <stdint.h>
#include "volk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef VkResult (*VkpCreateSurfaceFn)(VkInstance instance, void *user, VkSurfaceKHR *surface);

typedef struct {
	int vsync;           /* 1 = FIFO, 0 = MAILBOX/IMMEDIATE if available */
	int linear_filter;   /* bilinear scaling instead of nearest */
	double aspect;       /* displayed aspect of the 320x200 image (1.6 or 4/3) */
	int validation;      /* enable VK_LAYER_KHRONOS_validation if present */
} VkpConfig;

/* Returns 1 on success.  instance_exts: surface extensions required by the
 * window system (e.g. VK_KHR_surface + VK_KHR_win32_surface). */
int VkpInit(const char *const *instance_exts, uint32_t ext_count,
            VkpCreateSurfaceFn create_surface, void *user,
            const VkpConfig *cfg, uint32_t win_w, uint32_t win_h);

/* Present one frame.  rgba: w*h pixels in R,G,B,A byte order. */
void VkpPresent(const uint32_t *rgba, int w, int h);

/* Notify the presenter of a window size change (0x0 = minimized). */
void VkpResize(uint32_t win_w, uint32_t win_h);

void VkpShutdown(void);

/* Last error message (for message boxes / logs) */
const char *VkpGetError(void);

/* Name of the selected GPU */
const char *VkpGetDeviceName(void);

#ifdef __cplusplus
}
#endif

#endif
