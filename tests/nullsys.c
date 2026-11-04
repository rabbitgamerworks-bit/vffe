/*
 * nullsys.c - headless platform layer used to test the recompiled core.
 *
 * Renders into a memory framebuffer and writes PPM screenshots; input is
 * scripted with the FFE_KEYS environment variable:
 *    FFE_KEYS="frame:scancode,frame:scancode,..."   (key held for 3 frames)
 * FFE_FRAMES=n quits after n presented frames, FFE_SHOTS=a,b,c saves
 * screenshots of those frames to shot_<n>.ppm.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../ffeapi.h"
#include "../ffecfg.h"

extern int asmmain(int argc, char **argv);
void DirInit(void);
void DirCleanup(void);

static uint8_t fb[320 * 200];
static uint8_t pal[256][3];
static long frame;
static long maxframes = 2000;
static uint32_t timestamp;
static int keys[256];
static char shots[1024];

static int want_shot(long f)
{
	char buf[32];
	const char *p = shots;
	sprintf(buf, "%ld", f);
	while (*p) {
		size_t n = strcspn(p, ",");
		if (n == strlen(buf) && !strncmp(p, buf, n)) return 1;
		p += n;
		if (*p == ',') p++;
	}
	return 0;
}

static void save_ppm(const char *name)
{
	FILE *f = fopen(name, "wb");
	int i;
	if (!f) return;
	fprintf(f, "P6\n320 200\n255\n");
	for (i = 0; i < 320 * 200; i++) {
		uint8_t c[3];
		c[0] = (uint8_t)(pal[fb[i]][0] << 2);
		c[1] = (uint8_t)(pal[fb[i]][1] << 2);
		c[2] = (uint8_t)(pal[fb[i]][2] << 2);
		fwrite(c, 1, 3, f);
	}
	fclose(f);
}

static void update_keys(void)
{
	const char *s = getenv("FFE_KEYS");
	memset(keys, 0, sizeof keys);
	while (s && *s) {
		long fr; int sc; int n = 0;
		if (sscanf(s, "%ld:%i%n", &fr, &sc, &n) != 2) break;
		if (frame >= fr && frame < fr + 3 && sc >= 0 && sc < 256) keys[sc] = 1;
		s += n;
		if (*s == ',') s++;
	}
}

static int lastkey = 0xff;
static int prevkeys[256];

void SystemInit(void)
{
	const char *e = getenv("FFE_FRAMES");
	if (e) maxframes = atol(e);
	e = getenv("FFE_SHOTS");
	if (e) strncpy(shots, e, sizeof shots - 1);
	DirInit();
}

void SystemCleanup(void) { DirCleanup(); }

void VideoBlit(uint8_t *p, int32_t x, int32_t y, int32_t w, int32_t h, int32_t jump)
{
	uint8_t *d = fb + x + y * 320;
	for (; h; --h, p += jump + w, d += 320) memcpy(d, p, (size_t)w);
	if (w != 320) return;
	frame++;
	update_keys();
	if (want_shot(frame)) {
		char name[64];
		sprintf(name, "shot_%ld.ppm", frame);
		save_ppm(name);
	}
	if (frame >= maxframes) {
		save_ppm("shot_last.ppm");
		printf("nullsys: %ld frames rendered, exiting\n", frame);
		exit(0);
	}
}

void VideoMaskedBlit(uint8_t *p, int32_t x, int32_t y, int32_t w, int32_t h, int32_t jump)
{
	uint8_t *d = fb + x + y * 320;
	int cw;
	for (; h; --h, p += jump, d += 320 - w)
		for (cw = w; cw; --cw, ++p, ++d) if (*p) *d = *p;
}

void VideoReverseBlit(uint8_t *p, int32_t x, int32_t y, int32_t w, int32_t h, int32_t jump)
{
	uint8_t *d = fb + x + y * 320;
	for (; h; --h, p += jump + w, d += 320) memcpy(p, d, (size_t)w);
}

void VideoGetPalValue(int32_t i, uint8_t *v) { memcpy(v, pal[i & 255], 3); }
void VideoSetPalValue(int32_t i, uint8_t *v) { memcpy(pal[i & 255], v, 3); }
int32_t VideoPointerExclusive(void) { return 0; }
void VideoPointerEnable(void) {}
void VideoPointerDisable(void) {}

int32_t InputMouseReadButtons(void) { return 0; }
void InputMouseReadMickeys(int32_t *x, int32_t *y) { *x = *y = 0; }
void InputMouseReadPos(int32_t *x, int32_t *y) { *x = 160; *y = 100; }
void InputJoyReadPos(int32_t *x, int32_t *y) { *x = *y = 0; }
int32_t InputJoyReadButtons(void) { return 0; }

void InputKeybReadStates(uint8_t *k)
{
	int i;
	for (i = 0; i < 256; i++) {
		if (keys[i] && !prevkeys[i]) lastkey = i;
		prevkeys[i] = keys[i];
		k[i] = keys[i] ? 0x00 : 0x80;
	}
}
int32_t InputKeybGetLastKey(void) { return lastkey; }
void InputKeybSetLastKey(int32_t k) { lastkey = k; }

/* Deterministic time: advances 20ms per call to keep tests reproducible */
uint32_t TimerGetTimeStamp(void) { timestamp += 5; return timestamp; }
void TimerSleep(void) { timestamp += 5; }
void TimerFrameUpdate(void) {}

void SoundCheckInit(int32_t *a, int32_t *d, int32_t *m) { *a = 0; *d = 0; *m = 0; }
void SoundPlaySong(int32_t i) { (void)i; }
void SoundStopSong(void) {}
int32_t SoundSongDone(void) { return 1; }
void SoundStopAllSamples(void) {}
void SoundPlaySample(int32_t i, int32_t v, int32_t p) { (void)i; (void)v; (void)p; }
int32_t SoundStreamGetUsedBlocks(void) { return 0; }
void SoundStreamReset(void) {}
void SoundStreamAddBlock(void *p, int32_t s) { (void)p; (void)s; }
void SoundStreamStart(void) {}
void SoundStreamWait(void) {}
void SoundStreamStop(void) {}

int main(int argc, char **argv)
{
	return asmmain(argc, argv);
}
