/*
 * keyb.c - keyboard state + key repeat, shared by all platforms.
 * Logic follows the original SDL/Win32 input code of JJFFE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keyb.h"
#include "../../ffeapi.h"
#include "../../ffecfg.h"

static unsigned char rawdown[256];
static unsigned char states[256];
static uint32_t keytimes[256];
static int lastkey = 0xff;
static int repeatrate = 30;
static int repeatdelay = 300;

void KeybInit(const char *cfgfile)
{
	CfgStruct cfg;
	if (CfgOpen(&cfg, (char *)cfgfile)) {
		CfgFindSection(&cfg, "KEYBOARD");
		CfgGetKeyVal(&cfg, "repeatrate", &repeatrate);
		CfgGetKeyVal(&cfg, "repeatdelay", &repeatdelay);
		CfgClose(&cfg);
	}
	memset(rawdown, 0, sizeof rawdown);
	memset(states, 0x80, sizeof states);
	memset(keytimes, 0, sizeof keytimes);
	lastkey = 0xff;
}

void KeybSetKey(int sc, int down)
{
	if (sc <= 0 || sc > 255) return;
	rawdown[sc] = (unsigned char)(down ? 1 : 0);
}

void KeybReleaseAll(void)
{
	memset(rawdown, 0, sizeof rawdown);
}

int KeybIsDown(int sc)
{
	return sc > 0 && sc < 256 && rawdown[sc];
}

/* Test automation: FFE_KEYS="ms:scancode,ms:scancode,..." presses each key
 * for 150 ms at the given time (milliseconds since start). */
static void ScriptedKeys(uint32_t now)
{
	static int held[256];
	const char *s = getenv("FFE_KEYS");
	int want[256], i;
	if (!s) return;
	memset(want, 0, sizeof want);
	while (*s) {
		long t; int sc, n = 0;
		if (sscanf(s, "%ld:%i%n", &t, &sc, &n) != 2) break;
		if (sc > 0 && sc < 256 && now >= (uint32_t)t && now < (uint32_t)t + 150) want[sc] = 1;
		s += n;
		if (*s == ',') s++;
	}
	for (i = 1; i < 256; i++) {
		if (want[i] != held[i]) { rawdown[i] = (unsigned char)want[i]; held[i] = want[i]; }
	}
}

void InputKeybReadStates(uint8_t *pKeys)
{
	int i;
	uint32_t now = TimerGetTimeStamp();
	ScriptedKeys(now);
	for (i = 0; i < 256; i++) {
		if (rawdown[i]) {
			if (states[i] == 0x80) {            /* initial press */
				lastkey = i;
				states[i] = 0;
				keytimes[i] = now + (uint32_t)repeatdelay;
			} else if (states[i] == 0) {         /* hold */
				if (keytimes[i] < now) states[i] = 0xff;
			}
			if (states[i] == 0xff) {             /* repeat */
				if (keytimes[i] < now) {
					lastkey = i;
					keytimes[i] += (uint32_t)repeatrate;
				}
			}
		} else states[i] = 0x80;
	}
	for (i = 0; i < 256; i++) pKeys[i] = (uint8_t)(states[i] == 0x80 ? 0x80 : 0x00);
}

int32_t InputKeybGetLastKey(void) { return lastkey; }
void InputKeybSetLastKey(int32_t key) { lastkey = key; }

void MapWindowToGame(int wx, int wy, int ww, int wh, double aspect, int *gx, int *gy)
{
	int dw, dh, dx, dy;
	if (ww <= 0 || wh <= 0) { *gx = 160; *gy = 100; return; }
	dw = ww;
	dh = (int)((double)dw / aspect + 0.5);
	if (dh > wh) { dh = wh; dw = (int)((double)dh * aspect + 0.5); }
	dx = (ww - dw) / 2;
	dy = (wh - dh) / 2;
	*gx = (wx - dx) * 320 / (dw ? dw : 1);
	*gy = (wy - dy) * 200 / (dh ? dh : 1);
	if (*gx < 0) *gx = 0;
	if (*gx > 319) *gx = 319;
	if (*gy < 0) *gy = 0;
	if (*gy > 199) *gy = 199;
}
