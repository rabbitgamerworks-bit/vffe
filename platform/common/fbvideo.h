/*
 * fbvideo.h - platform independent 320x200 8-bit framebuffer + palette.
 *
 * Implements the Video*Blit / palette functions of ffeapi.h.  When the game
 * blits a full-width block (once per frame), the frame is converted to
 * 32-bit pixels and handed to the platform through PlatformPresentFrame().
 */
#ifndef FBVIDEO_H
#define FBVIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_WIDTH 320
#define FB_HEIGHT 200

/* Reads RedBase/GreenBase/BlueBase from the [VIDEO] section */
void FbInit(const char *cfgfile);

/* Converted frame: 320x200 pixels, byte order R,G,B,A in memory */
const uint32_t *FbGetRGBA(void);

/* Provided by the platform: show the latest frame */
void PlatformPresentFrame(const uint32_t *rgba, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
