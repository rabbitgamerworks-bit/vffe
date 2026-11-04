/*
 * keyb.h - shared keyboard state handling (DOS scancodes, key repeat).
 *
 * Platforms translate their key events to PC set-1 scancodes (extended
 * keys get bit 7 set, e.g. cursor up = 0xC8) and call KeybSetKey().
 */
#ifndef KEYB_H
#define KEYB_H

#ifdef __cplusplus
extern "C" {
#endif

void KeybInit(const char *cfgfile);
void KeybSetKey(int scancode, int down);
void KeybReleaseAll(void);
int KeybIsDown(int scancode);

/* Map a window position to game coordinates (320x200), honouring the
 * letterboxing of the presenter. */
void MapWindowToGame(int wx, int wy, int ww, int wh, double aspect, int *gx, int *gy);

#ifdef __cplusplus
}
#endif

#endif
