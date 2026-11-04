/*
 * fbvideo.c - platform independent framebuffer / palette handling.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fbvideo.h"
#include "../../ffeapi.h"
#include "../../ffecfg.h"

static uint8_t backbuf[FB_WIDTH * FB_HEIGHT];
static uint32_t rgba[FB_WIDTH * FB_HEIGHT];
static uint8_t palraw[256][3];       /* game palette, 6 bits per channel */
static uint32_t pal32[256];          /* converted, R,G,B,A byte order */
static uint8_t paltable[3][64];      /* brightness correction */

void FbInit(const char *cfgfile)
{
	CfgStruct cfg;
	int base[3] = { 0, 0, 0 };
	int i, j;

	if (CfgOpen(&cfg, (char *)cfgfile)) {
		CfgFindSection(&cfg, "VIDEO");
		CfgGetKeyVal(&cfg, "RedBase", base);
		CfgGetKeyVal(&cfg, "GreenBase", base + 1);
		CfgGetKeyVal(&cfg, "BlueBase", base + 2);
		CfgClose(&cfg);
	}
	for (i = 0; i < 3; i++) {
		int col = base[i] << 8;
		int step = ((64 << 8) - col) / 63;
		for (j = 0; j < 64; j++) {
			int t = col >> 8;
			if (t < 0) t = 0;
			if (t > 63) t = 63;
			paltable[i][j] = (uint8_t)t;
			col += step;
		}
	}
	memset(backbuf, 0, sizeof backbuf);
	memset(pal32, 0, sizeof pal32);
	memset(palraw, 0, sizeof palraw);
}

static uint32_t conv(const uint8_t *v)
{
	uint32_t r = paltable[0][v[0] & 63], g = paltable[1][v[1] & 63], b = paltable[2][v[2] & 63];
	r = (r << 2) | (r >> 4);
	g = (g << 2) | (g >> 4);
	b = (b << 2) | (b >> 4);
	/* memory order R,G,B,A on little-endian hosts */
	return r | (g << 8) | (b << 16) | 0xFF000000u;
}

/* Test automation: FFE_FRAMES=n exits after n frames, saving the last
 * frame to FFE_SCREENSHOT (PPM) if set. */
static void SavePPM(const char *name)
{
	FILE *f = fopen(name, "wb");
	int i;
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", FB_WIDTH, FB_HEIGHT);
	for (i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
		uint32_t c = pal32[backbuf[i]];
		unsigned char px[3];
		px[0] = (unsigned char)c; px[1] = (unsigned char)(c >> 8); px[2] = (unsigned char)(c >> 16);
		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

static void FbTestHook(void)
{
	static long frames = 0, maxframes = -2;
	if (maxframes == -2) {
		const char *e = getenv("FFE_FRAMES");
		maxframes = e ? atol(e) : -1;
	}
	frames++;
	if (maxframes >= 0 && frames >= maxframes) {
		const char *shot = getenv("FFE_SCREENSHOT");
		if (shot) SavePPM(shot);
		printf("FFE test: %ld frames presented\n", frames);
		fflush(stdout);
		SystemCleanup();
		exit(0);
	}
}

const uint32_t *FbGetRGBA(void)
{
	int i;
	for (i = 0; i < FB_WIDTH * FB_HEIGHT; i++) rgba[i] = pal32[backbuf[i]];
	return rgba;
}

void VideoBlit(uint8_t *pData, int32_t x, int32_t y, int32_t w, int32_t h, int32_t jump)
{
	uint8_t *pBuf = backbuf + x + y * FB_WIDTH;
	for (; h > 0; --h, pData += jump + w, pBuf += FB_WIDTH) memcpy(pBuf, pData, (size_t)w);
	if (w != FB_WIDTH) return;     /* one flip per frame */
	PlatformPresentFrame(FbGetRGBA(), FB_WIDTH, FB_HEIGHT);
	FbTestHook();
}

void VideoMaskedBlit(uint8_t *pData, int32_t x, int32_t y, int32_t w, int32_t h, int32_t jump)
{
	int cw, dJump = FB_WIDTH - w;
	uint8_t *pBuf = backbuf + x + FB_WIDTH * y;
	for (; h > 0; --h, pData += jump, pBuf += dJump)
		for (cw = w; cw; --cw, ++pData, ++pBuf)
			if (*pData) *pBuf = *pData;
}

void VideoReverseBlit(uint8_t *pData, int32_t x, int32_t y, int32_t w, int32_t h, int32_t jump)
{
	uint8_t *pBuf = backbuf + x + y * FB_WIDTH;
	for (; h > 0; --h, pData += jump + w, pBuf += FB_WIDTH) memcpy(pData, pBuf, (size_t)w);
}

void VideoSetPalValue(int32_t palindex, uint8_t *pVal)
{
	palindex &= 255;
	memcpy(palraw[palindex], pVal, 3);
	pal32[palindex] = conv(pVal);
}

void VideoGetPalValue(int32_t palindex, uint8_t *pVal)
{
	memcpy(pVal, palraw[palindex & 255], 3);
}
