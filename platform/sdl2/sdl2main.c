/*
 * sdl2main.c - SDL2 + Vulkan platform layer (Linux and other systems).
 *
 * Same structure as the Windows x64 layer: SDL2 provides the window,
 * input and audio; the frame is presented through Vulkan.
 */
#include <SDL.h>
#include <SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vulkan/vkpresent.h"
#include "../../ffeapi.h"
#include "../../ffecfg.h"
#include "../common/fbvideo.h"
#include "../common/keyb.h"
#include "../common/mixer.h"

extern int asmmain(int argc, char **argv);
void DirInit(void);
void DirCleanup(void);

#define CFGFILE __CONFIGFILE__

static int cfg_fullscreen = 0, cfg_winwidth = 960, cfg_winheight = 600;
static int cfg_vsync = 1, cfg_bilinear = 0, cfg_aspect43 = 0, cfg_validation = 0;
static int cfg_sensitivity = 100, cfg_nosound = 0;
static double aspect = 1.6;

static SDL_Window *window;
static SDL_AudioDeviceID audiodev;
static int active = 1;
static int ptrEnabled = 1;
static int mousebuttons;
static int mickx, micky;

/* SDL scancode -> PC set 1 scancode (extended keys | 0x80) */
static int ConvScancode(SDL_Scancode s)
{
	static const struct { SDL_Scancode sdl; int pc; } map[] = {
		{ SDL_SCANCODE_ESCAPE, 0x01 }, { SDL_SCANCODE_1, 0x02 }, { SDL_SCANCODE_2, 0x03 },
		{ SDL_SCANCODE_3, 0x04 }, { SDL_SCANCODE_4, 0x05 }, { SDL_SCANCODE_5, 0x06 },
		{ SDL_SCANCODE_6, 0x07 }, { SDL_SCANCODE_7, 0x08 }, { SDL_SCANCODE_8, 0x09 },
		{ SDL_SCANCODE_9, 0x0A }, { SDL_SCANCODE_0, 0x0B }, { SDL_SCANCODE_MINUS, 0x0C },
		{ SDL_SCANCODE_EQUALS, 0x0D }, { SDL_SCANCODE_BACKSPACE, 0x0E }, { SDL_SCANCODE_TAB, 0x0F },
		{ SDL_SCANCODE_Q, 0x10 }, { SDL_SCANCODE_W, 0x11 }, { SDL_SCANCODE_E, 0x12 },
		{ SDL_SCANCODE_R, 0x13 }, { SDL_SCANCODE_T, 0x14 }, { SDL_SCANCODE_Y, 0x15 },
		{ SDL_SCANCODE_U, 0x16 }, { SDL_SCANCODE_I, 0x17 }, { SDL_SCANCODE_O, 0x18 },
		{ SDL_SCANCODE_P, 0x19 }, { SDL_SCANCODE_LEFTBRACKET, 0x1A }, { SDL_SCANCODE_RIGHTBRACKET, 0x1B },
		{ SDL_SCANCODE_RETURN, 0x1C }, { SDL_SCANCODE_LCTRL, 0x1D }, { SDL_SCANCODE_A, 0x1E },
		{ SDL_SCANCODE_S, 0x1F }, { SDL_SCANCODE_D, 0x20 }, { SDL_SCANCODE_F, 0x21 },
		{ SDL_SCANCODE_G, 0x22 }, { SDL_SCANCODE_H, 0x23 }, { SDL_SCANCODE_J, 0x24 },
		{ SDL_SCANCODE_K, 0x25 }, { SDL_SCANCODE_L, 0x26 }, { SDL_SCANCODE_SEMICOLON, 0x27 },
		{ SDL_SCANCODE_APOSTROPHE, 0x28 }, { SDL_SCANCODE_GRAVE, 0x29 }, { SDL_SCANCODE_LSHIFT, 0x2A },
		{ SDL_SCANCODE_BACKSLASH, 0x2B }, { SDL_SCANCODE_Z, 0x2C }, { SDL_SCANCODE_X, 0x2D },
		{ SDL_SCANCODE_C, 0x2E }, { SDL_SCANCODE_V, 0x2F }, { SDL_SCANCODE_B, 0x30 },
		{ SDL_SCANCODE_N, 0x31 }, { SDL_SCANCODE_M, 0x32 }, { SDL_SCANCODE_COMMA, 0x33 },
		{ SDL_SCANCODE_PERIOD, 0x34 }, { SDL_SCANCODE_SLASH, 0x35 }, { SDL_SCANCODE_RSHIFT, 0x36 },
		{ SDL_SCANCODE_KP_MULTIPLY, 0x37 }, { SDL_SCANCODE_LALT, 0x38 }, { SDL_SCANCODE_SPACE, 0x39 },
		{ SDL_SCANCODE_CAPSLOCK, 0x3A }, { SDL_SCANCODE_F1, 0x3B }, { SDL_SCANCODE_F2, 0x3C },
		{ SDL_SCANCODE_F3, 0x3D }, { SDL_SCANCODE_F4, 0x3E }, { SDL_SCANCODE_F5, 0x3F },
		{ SDL_SCANCODE_F6, 0x40 }, { SDL_SCANCODE_F7, 0x41 }, { SDL_SCANCODE_F8, 0x42 },
		{ SDL_SCANCODE_F9, 0x43 }, { SDL_SCANCODE_F10, 0x44 }, { SDL_SCANCODE_NUMLOCKCLEAR, 0x45 },
		{ SDL_SCANCODE_SCROLLLOCK, 0x46 }, { SDL_SCANCODE_KP_7, 0x47 }, { SDL_SCANCODE_KP_8, 0x48 },
		{ SDL_SCANCODE_KP_9, 0x49 }, { SDL_SCANCODE_KP_MINUS, 0x4A }, { SDL_SCANCODE_KP_4, 0x4B },
		{ SDL_SCANCODE_KP_5, 0x4C }, { SDL_SCANCODE_KP_6, 0x4D }, { SDL_SCANCODE_KP_PLUS, 0x4E },
		{ SDL_SCANCODE_KP_1, 0x4F }, { SDL_SCANCODE_KP_2, 0x50 }, { SDL_SCANCODE_KP_3, 0x51 },
		{ SDL_SCANCODE_KP_0, 0x52 }, { SDL_SCANCODE_KP_PERIOD, 0x53 }, { SDL_SCANCODE_F11, 0x57 },
		{ SDL_SCANCODE_F12, 0x58 }, { SDL_SCANCODE_KP_ENTER, 0x9C }, { SDL_SCANCODE_RCTRL, 0x9D },
		{ SDL_SCANCODE_KP_DIVIDE, 0xB5 }, { SDL_SCANCODE_PRINTSCREEN, 0xB7 }, { SDL_SCANCODE_RALT, 0xB8 },
		{ SDL_SCANCODE_HOME, 0xC7 }, { SDL_SCANCODE_UP, 0xC8 }, { SDL_SCANCODE_PAGEUP, 0xC9 },
		{ SDL_SCANCODE_LEFT, 0xCB }, { SDL_SCANCODE_RIGHT, 0xCD }, { SDL_SCANCODE_END, 0xCF },
		{ SDL_SCANCODE_DOWN, 0xD0 }, { SDL_SCANCODE_PAGEDOWN, 0xD1 }, { SDL_SCANCODE_INSERT, 0xD2 },
		{ SDL_SCANCODE_DELETE, 0xD3 }, { SDL_SCANCODE_LGUI, 0xDB }, { SDL_SCANCODE_RGUI, 0xDC },
		{ SDL_SCANCODE_APPLICATION, 0xDD },
	};
	size_t i;
	for (i = 0; i < sizeof map / sizeof map[0]; i++) if (map[i].sdl == s) return map[i].pc;
	return 0;
}

static void ToggleFullscreen(void)
{
	cfg_fullscreen = !cfg_fullscreen;
	SDL_SetWindowFullscreen(window, cfg_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void PumpEvents(void)
{
	SDL_Event ev;
	for (;;) {
		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
			case SDL_QUIT:
				SystemCleanup();
				exit(0);
			case SDL_KEYDOWN:
			case SDL_KEYUP: {
				int down = ev.type == SDL_KEYDOWN;
				if (down && ev.key.keysym.scancode == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT)) { ToggleFullscreen(); break; }
				if (down && ev.key.keysym.scancode == SDL_SCANCODE_F12 && (ev.key.keysym.mod & KMOD_CTRL)) { ToggleFullscreen(); break; }
				KeybSetKey(ConvScancode(ev.key.keysym.scancode), down);
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP: {
				int bit = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
				if (ev.type == SDL_MOUSEBUTTONDOWN) mousebuttons |= bit; else mousebuttons &= ~bit;
				break;
			}
			case SDL_MOUSEMOTION:
				mickx += ev.motion.xrel;
				micky += ev.motion.yrel;
				break;
			case SDL_WINDOWEVENT:
				if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
					int w, h;
					SDL_Vulkan_GetDrawableSize(window, &w, &h);
					VkpResize((uint32_t)w, (uint32_t)h);
				} else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
					KeybReleaseAll(); mousebuttons = 0; active = 0;
				} else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
					active = 1;
				} else if (ev.window.event == SDL_WINDOWEVENT_MINIMIZED) {
					active = 0;
				} else if (ev.window.event == SDL_WINDOWEVENT_RESTORED) {
					active = 1;
				}
				break;
			}
		}
		if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) return;
		SDL_Delay(50);
	}
}

static VkResult CreateSurface(VkInstance instance, void *user, VkSurfaceKHR *surface)
{
	(void)user;
	return SDL_Vulkan_CreateSurface(window, instance, surface) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

void PlatformPresentFrame(const uint32_t *rgba, int w, int h)
{
	static int announced = 0;
	VkpPresent(rgba, w, h);
	if (!announced) { printf("Vulkan device: %s\n", VkpGetDeviceName()); announced = 1; }
	PumpEvents();
}

int32_t VideoPointerExclusive(void) { return cfg_fullscreen; }
void VideoPointerEnable(void)
{
	if (ptrEnabled) return;
	ptrEnabled = 1;
	SDL_SetRelativeMouseMode(SDL_FALSE);
}
void VideoPointerDisable(void)
{
	if (!ptrEnabled) return;
	ptrEnabled = 0;
	SDL_SetRelativeMouseMode(SDL_TRUE);
	mickx = micky = 0;
}

int32_t InputMouseReadButtons(void) { return active ? mousebuttons : 0; }
void InputMouseReadMickeys(int32_t *x, int32_t *y)
{
	*x = mickx * cfg_sensitivity / 100;
	*y = micky * cfg_sensitivity / 100;
	mickx = micky = 0;
}
void InputMouseReadPos(int32_t *x, int32_t *y)
{
	int mx, my, ww, wh, gx, gy;
	SDL_GetMouseState(&mx, &my);
	SDL_GetWindowSize(window, &ww, &wh);
	MapWindowToGame(mx, my, ww, wh, aspect, &gx, &gy);
	*x = gx; *y = gy;
}
void InputJoyReadPos(int32_t *x, int32_t *y) { *x = *y = 0; }
int32_t InputJoyReadButtons(void) { return 0; }

static uint32_t tm_start;
uint32_t TimerGetTimeStamp(void) { return SDL_GetTicks() - tm_start; }
void TimerSleep(void) { SDL_Delay(1); }
void TimerFrameUpdate(void) { PumpEvents(); }

void PlatformAudioLock(void) { if (audiodev) SDL_LockAudioDevice(audiodev); }
void PlatformAudioUnlock(void) { if (audiodev) SDL_UnlockAudioDevice(audiodev); }
int PlatformMusicAvailable(void) { return 0; }   /* no MIDI synth in SDL2 */

static void SDLCALL AudioCallback(void *ud, Uint8 *stream, int len)
{
	(void)ud;
	MixerRender((int16_t *)stream, len / 4);
}

void SoundPlaySong(int32_t index) { (void)index; }
void SoundStopSong(void) {}
int32_t SoundSongDone(void) { return 1; }

static void ReadConfig(void)
{
	CfgStruct cfg;
	if (!CfgOpen(&cfg, CFGFILE)) return;
	CfgFindSection(&cfg, "VIDEO");
	CfgGetKeyVal(&cfg, "fullscreen", &cfg_fullscreen);
	CfgGetKeyVal(&cfg, "winwidth", &cfg_winwidth);
	CfgGetKeyVal(&cfg, "winheight", &cfg_winheight);
	CfgGetKeyVal(&cfg, "vsync", &cfg_vsync);
	CfgGetKeyVal(&cfg, "bilinear", &cfg_bilinear);
	CfgGetKeyVal(&cfg, "aspect43", &cfg_aspect43);
	CfgGetKeyVal(&cfg, "validation", &cfg_validation);
	CfgFindSection(&cfg, "MOUSE");
	CfgGetKeyVal(&cfg, "sensitivity", &cfg_sensitivity);
	CfgFindSection(&cfg, "SOUND");
	CfgGetKeyVal(&cfg, "nosound", &cfg_nosound);
	CfgClose(&cfg);
}

void SystemInit(void)
{
	unsigned int n = 0;
	const char **exts;
	VkpConfig vc;
	int w, h;

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		exit(1);
	}
	tm_start = SDL_GetTicks();

	DirInit();
	ReadConfig();
	FbInit(CFGFILE);
	KeybInit(CFGFILE);
	aspect = cfg_aspect43 ? 4.0 / 3.0 : 1.6;

	window = SDL_CreateWindow("Frontier: First Encounters (JJFFE 64-bit / Vulkan)",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cfg_winwidth, cfg_winheight,
		SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
		(cfg_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	if (!window) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); exit(1); }

	SDL_Vulkan_GetInstanceExtensions(window, &n, NULL);
	exts = (const char **)calloc(n ? n : 1, sizeof *exts);
	SDL_Vulkan_GetInstanceExtensions(window, &n, exts);
	SDL_Vulkan_GetDrawableSize(window, &w, &h);
	memset(&vc, 0, sizeof vc);
	vc.vsync = cfg_vsync;
	vc.linear_filter = cfg_bilinear;
	vc.aspect = aspect;
	vc.validation = cfg_validation;
	if (!VkpInit(exts, n, CreateSurface, NULL, &vc, (uint32_t)w, (uint32_t)h)) {
		fprintf(stderr, "%s\n", VkpGetError());
		exit(1);
	}
	free(exts);

	{
		int ok = 0;
		if (!cfg_nosound && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
			SDL_AudioSpec want, have;
			memset(&want, 0, sizeof want);
			want.freq = MIXER_RATE;
			want.format = AUDIO_S16SYS;
			want.channels = 2;
			want.samples = 1024;
			want.callback = AudioCallback;
			audiodev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
			if (audiodev) ok = 1;
		}
		MixerInit(ok);
		if (audiodev) SDL_PauseAudioDevice(audiodev, 0);
	}
}

void SystemCleanup(void)
{
	static int done = 0;
	if (done) return;
	done = 1;
	if (audiodev) { SDL_CloseAudioDevice(audiodev); audiodev = 0; }
	MixerCleanup();
	VkpShutdown();
	if (window) SDL_DestroyWindow(window);
	DirCleanup();
	SDL_Quit();
}

int main(int argc, char **argv)
{
	return asmmain(argc, argv);
}
