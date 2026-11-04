/*
 * win64main.c - Windows x64 platform layer for JJFFE.
 *
 * Replaces the 32-bit DirectDraw/DirectInput/DirectSound/DirectMusic code
 * (none of which is usable from a modern 64-bit process) with:
 *   video  : Vulkan (platform/vulkan/vkpresent.c)
 *   input  : Win32 messages + raw input for mouse movement
 *   timer  : QueryPerformanceCounter
 *   sound  : software mixer (platform/common/mixer.c) through waveOut
 *   music  : HMP -> MIDI conversion (hmp.cpp) played through MCI
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "../vulkan/vkpresent.h"

#include "../../ffeapi.h"
#include "../../ffecfg.h"
#include "../common/fbvideo.h"
#include "../common/keyb.h"
#include "../common/mixer.h"

extern int asmmain(int argc, char **argv);
void DirInit(void);
void DirCleanup(void);
void *load_hmp(unsigned char *buf, uint32_t insize, uint32_t *outsize);

#define CFGFILE __CONFIGFILE__

/* ------------------------------------------------------------------ */
/* Configuration */

static int cfg_fullscreen = 0;
static int cfg_winwidth = 960;
static int cfg_winheight = 600;
static int cfg_vsync = 1;
static int cfg_bilinear = 0;
static int cfg_aspect43 = 0;
static int cfg_validation = 0;
static int cfg_sensitivity = 100;
static int cfg_nosound = 0;
static int cfg_nomusic = 0;

static double aspect = 1.6;

/* ------------------------------------------------------------------ */
/* Window */

static HINSTANCE hInst;
static HWND hWnd;
static int active = 1;
static int fullscreen = 0;
static RECT savedrect;
static LONG savedstyle;
static int clientw, clienth;

static int ptrEnabled = 1;
static POINT savedcursor;
static int mousebuttons;
static LONG mickx, micky;

static void Fatal(const char *msg)
{
	MessageBoxA(NULL, msg, "JJFFE x64 error", MB_OK | MB_ICONERROR);
	exit(1);
}

static void UpdateClip(void)
{
	if (!ptrEnabled && active) {
		RECT r;
		GetClientRect(hWnd, &r);
		MapWindowPoints(hWnd, NULL, (POINT *)&r, 2);
		ClipCursor(&r);
	} else ClipCursor(NULL);
}

static void SetFullscreen(int on)
{
	if (on == fullscreen) return;
	if (on) {
		MONITORINFO mi;
		mi.cbSize = sizeof mi;
		GetWindowRect(hWnd, &savedrect);
		savedstyle = GetWindowLongA(hWnd, GWL_STYLE);
		GetMonitorInfoA(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);
		SetWindowLongA(hWnd, GWL_STYLE, (savedstyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
		SetWindowPos(hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
			mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
			SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
	} else {
		SetWindowLongA(hWnd, GWL_STYLE, savedstyle);
		SetWindowPos(hWnd, NULL, savedrect.left, savedrect.top,
			savedrect.right - savedrect.left, savedrect.bottom - savedrect.top,
			SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOZORDER);
	}
	fullscreen = on;
	UpdateClip();
}

static void HandleKey(WPARAM wp, LPARAM lp, int down)
{
	int sc = (int)((lp >> 16) & 0xFF);
	if (lp & (1 << 24)) sc |= 0x80;          /* extended key */
	if (wp == VK_SHIFT && sc == 0x36) sc = 0x36;
	if (sc == 0x45 || sc == 0xC5) {          /* NumLock / Pause quirks */
		sc = (wp == VK_PAUSE) ? 0 : 0x45;
	}
	if (down && (wp == VK_RETURN) && (GetKeyState(VK_MENU) & 0x8000)) {
		SetFullscreen(!fullscreen);           /* Alt+Enter */
		return;
	}
	if (down && wp == VK_F12 && (GetKeyState(VK_CONTROL) & 0x8000)) {
		SetFullscreen(!fullscreen);           /* Ctrl+F12, like the original */
		return;
	}
	if (sc) KeybSetKey(sc, down);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;     /* Alt+F4 */
		HandleKey(wp, lp, 1);
		return 0;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		HandleKey(wp, lp, 0);
		return 0;
	case WM_SYSCHAR:
		return 0;                                           /* no menu beep */
	case WM_SYSCOMMAND:
		if ((wp & 0xFFF0) == SC_KEYMENU) return 0;          /* F10 / Alt */
		break;
	case WM_LBUTTONDOWN: mousebuttons |= 1; SetCapture(hwnd); return 0;
	case WM_LBUTTONUP:   mousebuttons &= ~1; if (!mousebuttons) ReleaseCapture(); return 0;
	case WM_RBUTTONDOWN: mousebuttons |= 2; SetCapture(hwnd); return 0;
	case WM_RBUTTONUP:   mousebuttons &= ~2; if (!mousebuttons) ReleaseCapture(); return 0;
	case WM_INPUT: {
		RAWINPUT ri;
		UINT size = sizeof ri;
		if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
		    ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
			mickx += ri.data.mouse.lLastX;
			micky += ri.data.mouse.lLastY;
		}
		break;
	}
	case WM_ACTIVATE:
		active = LOWORD(wp) != WA_INACTIVE;
		if (!active) { KeybReleaseAll(); mousebuttons = 0; }
		UpdateClip();
		return 0;
	case WM_SIZE:
		clientw = LOWORD(lp);
		clienth = HIWORD(lp);
		VkpResize((uint32_t)clientw, (uint32_t)clienth);
		UpdateClip();
		return 0;
	case WM_SETCURSOR:
		if (LOWORD(lp) == HTCLIENT && !ptrEnabled) { SetCursor(NULL); return TRUE; }
		break;
	case WM_ERASEBKGND:
		return 1;
	case WM_CLOSE:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wp, lp);
}

static void PumpMessages(void)
{
	MSG msg;
	for (;;) {
		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				SystemCleanup();
				exit(0);
			}
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		if (active || fullscreen) return;
		WaitMessage();       /* paused while minimized / in background */
	}
}

static VkResult CreateSurface(VkInstance instance, void *user, VkSurfaceKHR *surface)
{
	VkWin32SurfaceCreateInfoKHR ci;
	PFN_vkCreateWin32SurfaceKHR fn;
	(void)user;
	fn = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR");
	if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
	memset(&ci, 0, sizeof ci);
	ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	ci.hinstance = hInst;
	ci.hwnd = hWnd;
	return fn(instance, &ci, NULL, surface);
}

static void VideoInit(void)
{
	WNDCLASSA wc;
	RECT r;
	DWORD style = WS_OVERLAPPEDWINDOW;
	VkpConfig vc;
	const char *exts[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
	RAWINPUTDEVICE rid;
	char buf[600];

	FbInit(CFGFILE);
	aspect = cfg_aspect43 ? 4.0 / 3.0 : 1.6;

	memset(&wc, 0, sizeof wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hIcon = LoadIconA(hInst, MAKEINTRESOURCEA(1));
	if (!wc.hIcon) wc.hIcon = LoadIconA(NULL, (LPCSTR)IDI_APPLICATION);
	wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
	wc.lpszClassName = "JJFFE64";
	RegisterClassA(&wc);

	r.left = 0; r.top = 0; r.right = cfg_winwidth; r.bottom = cfg_winheight;
	AdjustWindowRect(&r, style, FALSE);
	hWnd = CreateWindowA("JJFFE64", "Frontier: First Encounters (JJFFE x64 / Vulkan)", style,
		CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, hInst, NULL);
	if (!hWnd) Fatal("Cannot create window");
	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);
	GetClientRect(hWnd, &r);
	clientw = r.right; clienth = r.bottom;

	rid.usUsagePage = 0x01;
	rid.usUsage = 0x02;      /* mouse */
	rid.dwFlags = 0;
	rid.hwndTarget = hWnd;
	RegisterRawInputDevices(&rid, 1, sizeof rid);

	memset(&vc, 0, sizeof vc);
	vc.vsync = cfg_vsync;
	vc.linear_filter = cfg_bilinear;
	vc.aspect = aspect;
	vc.validation = cfg_validation;
	if (!VkpInit(exts, 2, CreateSurface, NULL, &vc, (uint32_t)clientw, (uint32_t)clienth)) {
		snprintf(buf, sizeof buf, "Vulkan initialisation failed.\n\n%s\n\n"
			"A GPU driver with Vulkan 1.0 support is required.", VkpGetError());
		Fatal(buf);
	}
	if (cfg_fullscreen) SetFullscreen(1);
}

static void VideoCleanup(void)
{
	VkpShutdown();
	ClipCursor(NULL);
	if (hWnd) { DestroyWindow(hWnd); hWnd = NULL; }
}

void PlatformPresentFrame(const uint32_t *rgba, int w, int h)
{
	VkpPresent(rgba, w, h);
	PumpMessages();
}

int32_t VideoPointerExclusive(void) { return fullscreen; }

void VideoPointerEnable(void)
{
	if (ptrEnabled) return;
	ptrEnabled = 1;
	SetCursorPos(savedcursor.x, savedcursor.y);
	ShowCursor(TRUE);
	UpdateClip();
}

void VideoPointerDisable(void)
{
	if (!ptrEnabled) return;
	ptrEnabled = 0;
	GetCursorPos(&savedcursor);
	ShowCursor(FALSE);
	mickx = micky = 0;
	UpdateClip();
}

/* ------------------------------------------------------------------ */
/* Input */

int32_t InputMouseReadButtons(void)
{
	return active ? mousebuttons : 0;
}

void InputMouseReadMickeys(int32_t *pX, int32_t *pY)
{
	*pX = (int32_t)((mickx * cfg_sensitivity) / 100);
	*pY = (int32_t)((micky * cfg_sensitivity) / 100);
	mickx = micky = 0;
}

void InputMouseReadPos(int32_t *pX, int32_t *pY)
{
	POINT p;
	int gx, gy;
	GetCursorPos(&p);
	ScreenToClient(hWnd, &p);
	MapWindowToGame(p.x, p.y, clientw, clienth, aspect, &gx, &gy);
	*pX = gx;
	*pY = gy;
}

/* Joystick through the legacy winmm API (works on x64) */
static int joy_enabled = 0;
static int joy_xmax = 512, joy_ymax = 512;
static int joy_xdead = 50, joy_ydead = 50;

void InputJoyReadPos(int32_t *pX, int32_t *pY)
{
	JOYINFOEX ji;
	*pX = *pY = 0;
	if (!joy_enabled) return;
	memset(&ji, 0, sizeof ji);
	ji.dwSize = sizeof ji;
	ji.dwFlags = JOY_RETURNX | JOY_RETURNY;
	if (joyGetPosEx(JOYSTICKID1, &ji) != JOYERR_NOERROR) return;
	{
		int x = (int)ji.dwXpos - 32768, y = (int)ji.dwYpos - 32768;
		x = x * joy_xmax / 32768;
		y = y * joy_ymax / 32768;
		if (x > -joy_xdead && x < joy_xdead) x = 0;
		if (y > -joy_ydead && y < joy_ydead) y = 0;
		*pX = x; *pY = y;
	}
}

int32_t InputJoyReadButtons(void)
{
	JOYINFOEX ji;
	if (!joy_enabled) return 0;
	memset(&ji, 0, sizeof ji);
	ji.dwSize = sizeof ji;
	ji.dwFlags = JOY_RETURNBUTTONS;
	if (joyGetPosEx(JOYSTICKID1, &ji) != JOYERR_NOERROR) return 0;
	return (int32_t)(ji.dwButtons & 3);
}

static void InputInit(void)
{
	CfgStruct cfg;
	KeybInit(CFGFILE);
	if (CfgOpen(&cfg, CFGFILE)) {
		CfgFindSection(&cfg, "JOYSTICK");
		CfgGetKeyVal(&cfg, "enabled", &joy_enabled);
		CfgGetKeyVal(&cfg, "xmax", &joy_xmax);
		CfgGetKeyVal(&cfg, "ymax", &joy_ymax);
		CfgGetKeyVal(&cfg, "xdeadzone", &joy_xdead);
		CfgGetKeyVal(&cfg, "ydeadzone", &joy_ydead);
		CfgClose(&cfg);
	}
}

/* ------------------------------------------------------------------ */
/* Timer: same 5 ms granularity semantics as the original */

static LARGE_INTEGER tm_freq, tm_start;

static void TimerInit(void)
{
	QueryPerformanceFrequency(&tm_freq);
	QueryPerformanceCounter(&tm_start);
	timeBeginPeriod(1);
}

uint32_t TimerGetTimeStamp(void)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (uint32_t)(((now.QuadPart - tm_start.QuadPart) * 1000) / tm_freq.QuadPart);
}

void TimerSleep(void)
{
	Sleep(1);
}

void TimerFrameUpdate(void)
{
	PumpMessages();
}

/* ------------------------------------------------------------------ */
/* Audio output: waveOut with a feeder thread */

#define AUDIO_BUFS 4
#define AUDIO_FRAMES 1024

static HWAVEOUT hwo;
static WAVEHDR wavehdr[AUDIO_BUFS];
static int16_t audiobuf[AUDIO_BUFS][AUDIO_FRAMES * 2];
static HANDLE audio_event, audio_thread;
static volatile LONG audio_quit;
static CRITICAL_SECTION audio_cs;
static int audio_cs_init;

void PlatformAudioLock(void)   { if (audio_cs_init) EnterCriticalSection(&audio_cs); }
void PlatformAudioUnlock(void) { if (audio_cs_init) LeaveCriticalSection(&audio_cs); }

static DWORD WINAPI AudioThread(LPVOID arg)
{
	int i;
	(void)arg;
	while (!audio_quit) {
		WaitForSingleObject(audio_event, 50);
		for (i = 0; i < AUDIO_BUFS; i++) {
			if (audio_quit) break;
			if (wavehdr[i].dwFlags & WHDR_DONE) {
				MixerRender(audiobuf[i], AUDIO_FRAMES);
				wavehdr[i].dwFlags &= ~WHDR_DONE;
				waveOutWrite(hwo, &wavehdr[i], sizeof(WAVEHDR));
			}
		}
	}
	return 0;
}

static int AudioOpen(void)
{
	WAVEFORMATEX wf;
	int i;
	memset(&wf, 0, sizeof wf);
	wf.wFormatTag = WAVE_FORMAT_PCM;
	wf.nChannels = 2;
	wf.nSamplesPerSec = MIXER_RATE;
	wf.wBitsPerSample = 16;
	wf.nBlockAlign = 4;
	wf.nAvgBytesPerSec = MIXER_RATE * 4;
	audio_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (waveOutOpen(&hwo, WAVE_MAPPER, &wf, (DWORD_PTR)audio_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
		CloseHandle(audio_event);
		return 0;
	}
	InitializeCriticalSection(&audio_cs);
	audio_cs_init = 1;
	for (i = 0; i < AUDIO_BUFS; i++) {
		memset(&wavehdr[i], 0, sizeof(WAVEHDR));
		wavehdr[i].lpData = (LPSTR)audiobuf[i];
		wavehdr[i].dwBufferLength = sizeof audiobuf[i];
		waveOutPrepareHeader(hwo, &wavehdr[i], sizeof(WAVEHDR));
		wavehdr[i].dwFlags |= WHDR_DONE;
	}
	audio_quit = 0;
	audio_thread = CreateThread(NULL, 0, AudioThread, NULL, 0, NULL);
	SetThreadPriority(audio_thread, THREAD_PRIORITY_TIME_CRITICAL);
	SetEvent(audio_event);
	return 1;
}

static void AudioClose(void)
{
	int i;
	if (!hwo) return;
	InterlockedExchange(&audio_quit, 1);
	SetEvent(audio_event);
	WaitForSingleObject(audio_thread, 2000);
	CloseHandle(audio_thread);
	waveOutReset(hwo);
	for (i = 0; i < AUDIO_BUFS; i++) waveOutUnprepareHeader(hwo, &wavehdr[i], sizeof(WAVEHDR));
	waveOutClose(hwo);
	hwo = NULL;
	CloseHandle(audio_event);
}

/* ------------------------------------------------------------------ */
/* Music: HMP files converted to MIDI, played with MCI */

static const char *songnames[17] = {
	"qqfront", "qqatmosp", "qqtravel", "qqdrama", "qqsuspen", "qqrock",
	"qqescape", "qqparadi", "qqfront2", "hallking", "intro", "babayaga",
	"barem", "jupalt", "valkries", "bluedan", "ggofkiev"
};
static MCIDEVICEID midiDevice = 0;

int PlatformMusicAvailable(void)
{
	return !cfg_nomusic && midiOutGetNumDevs() > 0;
}

static int ConvertHMP(const char *hmpname, const char *midname)
{
	FILE *f;
	long size;
	unsigned char *in;
	void *out;
	uint32_t outsize = 0;
	f = fopen(hmpname, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	in = (unsigned char *)malloc((size_t)size);
	if (!in || fread(in, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(in); return 0; }
	fclose(f);
	out = load_hmp(in, (uint32_t)size, &outsize);
	free(in);
	if (!out) return 0;
	f = fopen(midname, "wb");
	if (f) { fwrite(out, 1, outsize, f); fclose(f); }
	free(out);
	return f != NULL;
}

void SoundPlaySong(int32_t index)
{
	char mid[512], hmp[512];
	MCI_OPEN_PARMSA op;
	MCI_PLAY_PARMS pp;
	if (!PlatformMusicAvailable() || index < 0 || index >= 17) return;
	SoundStopSong();
	DirMakeSongName(mid, (char *)songnames[index]);
	strcat(mid, ".mid");
	if (_access(mid, 4) != 0) {
		DirMakeSongName(hmp, (char *)songnames[index]);
		strcat(hmp, ".hmp");
		if (!ConvertHMP(hmp, mid)) return;
	}
	memset(&op, 0, sizeof op);
	op.lpstrDeviceType = (LPCSTR)(DWORD_PTR)MCI_DEVTYPE_SEQUENCER;
	op.lpstrElementName = mid;
	if (mciSendCommandA(0, MCI_OPEN, MCI_WAIT | MCI_OPEN_ELEMENT | MCI_OPEN_TYPE | MCI_OPEN_TYPE_ID,
	                    (DWORD_PTR)&op) != 0) return;
	midiDevice = op.wDeviceID;
	memset(&pp, 0, sizeof pp);
	if (mciSendCommandA(midiDevice, MCI_PLAY, 0, (DWORD_PTR)&pp) != 0) {
		mciSendCommandA(midiDevice, MCI_CLOSE, MCI_WAIT, 0);
		midiDevice = 0;
	}
}

void SoundStopSong(void)
{
	if (!midiDevice) return;
	mciSendCommandA(midiDevice, MCI_CLOSE, MCI_WAIT, 0);
	midiDevice = 0;
}

int32_t SoundSongDone(void)
{
	MCI_STATUS_PARMS sp;
	if (!midiDevice) return 1;
	memset(&sp, 0, sizeof sp);
	sp.dwItem = MCI_STATUS_MODE;
	if (mciSendCommandA(midiDevice, MCI_STATUS, MCI_STATUS_ITEM, (DWORD_PTR)&sp) != 0 ||
	    sp.dwReturn == MCI_MODE_STOP) {
		SoundStopSong();
		return 1;
	}
	return 0;
}

static void SoundInit(void)
{
	int ok = 0;
	if (!cfg_nosound) ok = AudioOpen();
	MixerInit(ok);
}

static void SoundCleanup(void)
{
	SoundStopSong();
	AudioClose();
	MixerCleanup();
}

/* ------------------------------------------------------------------ */
/* System */

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
	CfgGetKeyVal(&cfg, "nomusic", &cfg_nomusic);
	CfgClose(&cfg);
	if (cfg_winwidth < 320) cfg_winwidth = 320;
	if (cfg_winheight < 200) cfg_winheight = 200;
}

void SystemInit(void)
{
	TimerInit();
	DirInit();          /* may chdir into the game directory */
	ReadConfig();
	VideoInit();
	InputInit();
	SoundInit();
}

void SystemCleanup(void)
{
	static int done = 0;
	if (done) return;
	done = 1;
	SoundCleanup();
	VideoCleanup();
	DirCleanup();
	timeEndPeriod(1);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR cmdline, int show)
{
	(void)hPrev; (void)cmdline; (void)show;
	hInst = hInstance;
	SetProcessDPIAware();
	return asmmain(__argc, __argv);
}
