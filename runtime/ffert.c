/*
 * ffert.c - runtime support for the recompiled JJFFE core.
 *
 * Guest memory, guest heap, control transfer helpers and the thunks that
 * connect the recompiled code with the C library and the platform layer
 * (ffeapi.h).  Everything here is 64-bit clean.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "ffert.h"
#include "ffe_syms.h"
#include "../ffeapi.h"
#include "../ffecfg.h"

extern const uint32_t ffe_data_size;
extern const uint32_t ffe_data_image[];

uint8_t *g_mem;
uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
int lf_op = LF_OTHER, lf_sz = 4;
uint32_t lf_x, lf_y, lf_res, lf_cf, lf_of, lf_zf, lf_sf, lf_pf;
double fpu_st[8];
int fpu_top;
uint16_t fpu_cw = 0x037F, fpu_sw;
uint32_t guest_longjmp_val;

uint32_t GP(const void *p)
{
	const uint8_t *q = (const uint8_t *)p;
	if (!q) return 0;
	if (q < g_mem || q >= g_mem + GUEST_MEM_SIZE) guest_fatal("host pointer passed to guest");
	return (uint32_t)(q - g_mem);
}

uint32_t get_eflags(void)
{
	uint32_t f = 0x202;
	if (COND_C()) f |= 0x001;
	if (COND_P()) f |= 0x004;
	if (COND_Z()) f |= 0x040;
	if (COND_S()) f |= 0x080;
	if (COND_O()) f |= 0x800;
	return f;
}

void guest_fatal(const char *msg)
{
	fprintf(stderr, "FFE runtime fatal error: %s\n", msg);
	fflush(stderr);
	abort();
}

void guest_divide_error(void)
{
	guest_fatal("integer divide by zero");
}

/* ------------------------------------------------------------------ */
/* Guest heap: simple first-fit allocator with coalescing.
 * Block header (8 bytes): size (incl. header, multiple of 8) | used bit, prev size */

static uint32_t heap_start, heap_end;

#define HDR_SIZE(a)  (RD32(a) & ~7u)
#define HDR_USED(a)  (RD32(a) & 1u)

static void heap_init(uint32_t start, uint32_t end)
{
	heap_start = (start + 15u) & ~15u;
	heap_end = end & ~15u;
	WR32(heap_start, (heap_end - heap_start));
	WR32(heap_start + 4, 0);
}

static uint32_t guest_malloc(uint32_t size)
{
	uint32_t need = (size + 8u + 15u) & ~15u;
	uint32_t a = heap_start;
	if (size == 0) need = 16;
	while (a < heap_end) {
		uint32_t bs = HDR_SIZE(a);
		if (bs == 0) guest_fatal("guest heap corrupted");
		if (!HDR_USED(a) && bs >= need) {
			if (bs - need >= 32u) {
				uint32_t n = a + need;
				WR32(n, bs - need);
				WR32(n + 4, need);
				if (n + (bs - need) < heap_end) WR32(n + (bs - need) + 4, bs - need);
				bs = need;
			}
			WR32(a, bs | 1u);
			return a + 8;
		}
		a += bs;
	}
	return 0;
}

static void guest_free(uint32_t p)
{
	uint32_t a, bs, next, prevsz;
	if (!p) return;
	a = p - 8;
	bs = HDR_SIZE(a);
	WR32(a, bs);
	next = a + bs;
	if (next < heap_end && !HDR_USED(next)) {   /* merge with next */
		bs += HDR_SIZE(next);
		WR32(a, bs);
	}
	prevsz = RD32(a + 4);
	if (prevsz && !HDR_USED(a - prevsz)) {      /* merge with previous */
		uint32_t pa = a - prevsz;
		bs += prevsz;
		WR32(pa, bs);
		a = pa;
	}
	if (a + bs < heap_end) WR32(a + bs + 4, bs);
}

/* Copy a host string into a guest buffer allocated once per call site slot */
static uint32_t guest_strdup_slot(uint32_t *slot, const char *s)
{
	size_t n = strlen(s) + 1;
	if (*slot) guest_free(*slot);
	*slot = guest_malloc((uint32_t)n);
	if (!*slot) return 0;
	memcpy(g_mem + *slot, s, n);
	return *slot;
}

/* ------------------------------------------------------------------ */
/* Control transfer */

void guest_call(uint32_t addr)
{
	const GuestCodeEntry *e;
	uint32_t idx;
	if (addr < GUEST_CODE_BASE || ((addr - GUEST_CODE_BASE) & 3u) ||
	    (idx = (addr - GUEST_CODE_BASE) / 4u) >= GUEST_CODE_COUNT) {
		char buf[64];
		sprintf(buf, "indirect call/jump to bad address 0x%08x", (unsigned)addr);
		guest_fatal(buf);
	}
	e = &ffe_code_table[idx];
	if (e->region) e->region(e->entry);
	else if (e->ext) e->ext();
	else {
		char buf[128];
		sprintf(buf, "indirect transfer to non-entry label %s", e->name);
		guest_fatal(buf);
	}
}

/* Call guest code with cdecl arguments from the host */
uint32_t guest_invoke(uint32_t addr, int nargs, const uint32_t *args)
{
	int i;
	uint32_t saved_esp = esp;
	for (i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
	PUSH32(RETADDR);
	guest_call(addr);
	esp = saved_esp;
	return eax;
}

#define MAX_JMP 16
static struct { uint32_t key; GuestJmp j; } jmp_slots[MAX_JMP];
static int jmp_count;

GuestJmp *guest_jmp_slot(uint32_t buf)
{
	int i;
	for (i = 0; i < jmp_count; i++) if (jmp_slots[i].key == buf) return &jmp_slots[i].j;
	if (jmp_count == MAX_JMP) guest_fatal("too many SetJmp buffers");
	jmp_slots[jmp_count].key = buf;
	return &jmp_slots[jmp_count++].j;
}

void guest_longjmp(void)
{
	uint32_t buf = ARG(0);
	uint32_t val = ARG(1);
	int i;
	for (i = 0; i < jmp_count; i++) {
		if (jmp_slots[i].key == buf) {
			guest_longjmp_val = val;
			longjmp(jmp_slots[i].j.jb, 1);
		}
	}
	guest_fatal("LongJmp without SetJmp");
}

/* ------------------------------------------------------------------ */
/* printf-family formatting with guest arguments */

static size_t guest_vformat(char *out, size_t cap, uint32_t fmt, uint32_t ap)
{
	const char *f = HPC(fmt);
	size_t len = 0;
	char spec[32], tmp[512];
	if (!f) return 0;
	if (cap) out[0] = 0;
	while (*f) {
		int n = 0, sl = 0, lng = 0;
		char conv;
		if (*f != '%') {
			if (len + 1 < cap) out[len] = *f;
			len++; f++;
			continue;
		}
		spec[sl++] = *f++;
		if (*f == '%') {
			if (len + 1 < cap) out[len] = '%';
			len++; f++;
			continue;
		}
		while (*f && strchr("-+ #0", *f) && sl < 20) spec[sl++] = *f++;
		if (*f == '*') { sl += sprintf(spec + sl, "%d", (int32_t)RD32(ap)); ap += 4; f++; }
		else while (isdigit((unsigned char)*f) && sl < 24) spec[sl++] = *f++;
		if (*f == '.') {
			spec[sl++] = *f++;
			if (*f == '*') { sl += sprintf(spec + sl, "%d", (int32_t)RD32(ap)); ap += 4; f++; }
			else while (isdigit((unsigned char)*f) && sl < 28) spec[sl++] = *f++;
		}
		while (*f == 'l' || *f == 'h' || *f == 'L' || *f == 'N' || *f == 'F') { if (*f == 'l') lng++; f++; }
		conv = *f;
		if (!conv) break;
		f++;
		switch (conv) {
		case 'd': case 'i':
			if (lng >= 2) { spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
				n = snprintf(tmp, sizeof tmp, spec, (long long)RD64(ap)); ap += 8; }
			else { spec[sl++] = conv; spec[sl] = 0; n = snprintf(tmp, sizeof tmp, spec, (int)(int32_t)RD32(ap)); ap += 4; }
			break;
		case 'u': case 'x': case 'X': case 'o':
			if (lng >= 2) { spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
				n = snprintf(tmp, sizeof tmp, spec, (unsigned long long)RD64(ap)); ap += 8; }
			else { spec[sl++] = conv; spec[sl] = 0; n = snprintf(tmp, sizeof tmp, spec, (unsigned)RD32(ap)); ap += 4; }
			break;
		case 'c':
			spec[sl++] = 'c'; spec[sl] = 0;
			n = snprintf(tmp, sizeof tmp, spec, (int)(int32_t)RD32(ap)); ap += 4;
			break;
		case 'p':
			n = snprintf(tmp, sizeof tmp, "%08X", (unsigned)RD32(ap)); ap += 4;
			break;
		case 's': {
			const char *s = HPC(RD32(ap));
			ap += 4;
			spec[sl++] = 's'; spec[sl] = 0;
			n = snprintf(tmp, sizeof tmp, spec, s ? s : "(null)");
			break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
			spec[sl++] = conv; spec[sl] = 0;
			n = snprintf(tmp, sizeof tmp, spec, RDF64(ap)); ap += 8;
			break;
		case 'n':
			WR32(RD32(ap), (uint32_t)len); ap += 4;
			n = 0;
			break;
		default:
			tmp[0] = '%'; tmp[1] = conv; tmp[2] = 0; n = 2;
			break;
		}
		if (n < 0) n = 0;
		if (n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;
		if (len < cap) {
			size_t room = cap - 1 - len;
			memcpy(out + len, tmp, (size_t)n < room ? (size_t)n : room);
		}
		len += (size_t)n;
	}
	if (cap) out[len < cap ? len : cap - 1] = 0;
	return len;
}

/* ------------------------------------------------------------------ */
/* FILE handles */

#define MAX_FILES 32
#define FILE_HANDLE_BASE 0x00000100u
static FILE *files[MAX_FILES];

static FILE *hfile(uint32_t h)
{
	if (h < FILE_HANDLE_BASE || h >= FILE_HANDLE_BASE + MAX_FILES) return NULL;
	return files[h - FILE_HANDLE_BASE];
}

/* ------------------------------------------------------------------ */
/* Thunks: each one reads cdecl args via ARG(n), sets eax and pops the
 * return address like the original 'ret'. */

#define THUNK(name) void X_##name(void)
#define RET(v) do { eax = (uint32_t)(v); esp += 4u; return; } while (0)
#define RETV() do { esp += 4u; return; } while (0)

/* --- libc --- */
THUNK(memcpy)  { memcpy(HPN(ARG(0)), HPN(ARG(1)), ARG(2)); RET(ARG(0)); }
THUNK(memmove) { memmove(HPN(ARG(0)), HPN(ARG(1)), ARG(2)); RET(ARG(0)); }
THUNK(memset)  { memset(HPN(ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
THUNK(strcat)  { strcat(HPNC(ARG(0)), HPNC(ARG(1))); RET(ARG(0)); }
THUNK(strcpy)  { strcpy(HPNC(ARG(0)), HPNC(ARG(1))); RET(ARG(0)); }
THUNK(strncat) { strncat(HPNC(ARG(0)), HPNC(ARG(1)), ARG(2)); RET(ARG(0)); }
THUNK(strlen)  { RET(strlen(HPNC(ARG(0)))); }
THUNK(strcmp)  { RET((uint32_t)strcmp(HPNC(ARG(0)), HPNC(ARG(1)))); }
THUNK(strrchr) { char *p = strrchr(HPNC(ARG(0)), (int)ARG(1)); RET(p ? GP(p) : 0); }
THUNK(tolower) { RET((uint32_t)tolower((int)ARG(0))); }
THUNK(toupper) { RET((uint32_t)toupper((int)ARG(0))); }
THUNK(atoi)    { RET((uint32_t)atoi(HPNC(ARG(0)))); }
THUNK(abs)     { int32_t v = (int32_t)ARG(0); RET((uint32_t)(v < 0 ? -v : v)); }
THUNK(sqrt)    { FPUSH(sqrt(RDF64(esp + 4u))); RETV(); }
THUNK(malloc)  { RET(guest_malloc(ARG(0))); }
THUNK(free)    { guest_free(ARG(0)); RETV(); }

static uint32_t atexit_fns[32];
static int atexit_count;
THUNK(atexit)
{
	if (atexit_count == 32) RET(1);
	atexit_fns[atexit_count++] = ARG(0);
	RET(0);
}

static void run_guest_atexit(void)
{
	while (atexit_count > 0) guest_invoke(atexit_fns[--atexit_count], 0, NULL);
}

THUNK(exit)
{
	int code = (int)ARG(0);
	run_guest_atexit();
	exit(code);
}

THUNK(signal) { RET(0); }
THUNK(system) { RET(0xFFFFFFFFu); }
THUNK(remove) { RET((uint32_t)remove(HPNC(ARG(0)))); }

THUNK(fopen)
{
	int i;
	for (i = 0; i < MAX_FILES; i++) {
		if (!files[i]) {
			files[i] = fopen(HPNC(ARG(0)), HPNC(ARG(1)));
			RET(files[i] ? FILE_HANDLE_BASE + (uint32_t)i : 0);
		}
	}
	RET(0);
}

THUNK(fclose)
{
	uint32_t h = ARG(0);
	FILE *f = hfile(h);
	int r = -1;
	if (f) { r = fclose(f); files[h - FILE_HANDLE_BASE] = NULL; }
	RET((uint32_t)r);
}

THUNK(fread)  { FILE *f = hfile(ARG(3)); RET(f ? fread(HPN(ARG(0)), ARG(1), ARG(2), f) : 0); }
THUNK(fwrite) { FILE *f = hfile(ARG(3)); RET(f ? fwrite(HPN(ARG(0)), ARG(1), ARG(2), f) : 0); }
THUNK(fseek)  { FILE *f = hfile(ARG(0)); RET(f ? (uint32_t)fseek(f, (long)(int32_t)ARG(1), (int)ARG(2)) : 0xFFFFFFFFu); }
THUNK(ftell)  { FILE *f = hfile(ARG(0)); RET(f ? (uint32_t)ftell(f) : 0xFFFFFFFFu); }
THUNK(fflush) { FILE *f = hfile(ARG(0)); RET((uint32_t)fflush(f)); }
THUNK(fputc)  { FILE *f = hfile(ARG(1)); RET(f ? (uint32_t)fputc((int)ARG(0), f) : 0xFFFFFFFFu); }

THUNK(fgetpos)
{
	FILE *f = hfile(ARG(0));
	long p = f ? ftell(f) : -1;
	WR32(ARG(1), (uint32_t)p);
	WR32(ARG(1) + 4, 0);
	RET(p < 0 ? 0xFFFFFFFFu : 0);
}

static char fmtbuf[8192];

THUNK(printf)
{
	size_t n = guest_vformat(fmtbuf, sizeof fmtbuf, ARG(0), esp + 8u);
	fputs(fmtbuf, stdout);
	RET(n);
}

THUNK(sprintf)
{
	size_t n = guest_vformat(fmtbuf, sizeof fmtbuf, ARG(1), esp + 12u);
	memcpy(HPN(ARG(0)), fmtbuf, n + 1 < sizeof fmtbuf ? n + 1 : sizeof fmtbuf);
	RET(n);
}

THUNK(fprintf)
{
	FILE *f = hfile(ARG(0));
	size_t n = guest_vformat(fmtbuf, sizeof fmtbuf, ARG(1), esp + 12u);
	if (f) fputs(fmtbuf, f);
	RET(n);
}

THUNK(vfprintf)
{
	FILE *f = hfile(ARG(0));
	size_t n = guest_vformat(fmtbuf, sizeof fmtbuf, ARG(1), ARG(2));
	if (f) fputs(fmtbuf, f);
	RET(n);
}

/* fscanf: run each conversion separately with host pointers */
THUNK(fscanf)
{
	FILE *f = hfile(ARG(0));
	const char *fmt = HPC(ARG(1));
	uint32_t ap = esp + 12u;
	int count = 0;
	char piece[256];
	if (!f || !fmt) RET(0xFFFFFFFFu);
	while (*fmt) {
		int pl = 0, suppress = 0, r;
		/* copy literals up to and including one conversion */
		while (*fmt && *fmt != '%' && pl < 200) piece[pl++] = *fmt++;
		if (*fmt == '%') {
			piece[pl++] = *fmt++;
			if (*fmt == '%') { piece[pl++] = *fmt++; piece[pl] = 0; fscanf(f, piece); continue; }
			if (*fmt == '*') { suppress = 1; piece[pl++] = *fmt++; }
			while (isdigit((unsigned char)*fmt) && pl < 240) piece[pl++] = *fmt++;
			if (*fmt == 'h' || *fmt == 'l') {
				if (fmt[0] == 'l' && strchr("feEgG", fmt[1])) piece[pl++] = 'l';
				fmt++;
			}
			piece[pl++] = *fmt;
			piece[pl] = 0;
			if (suppress) r = fscanf(f, piece);
			else { r = fscanf(f, piece, HP(RD32(ap))); ap += 4; }
			if (*fmt) fmt++;
			if (r == EOF) RET(count ? (uint32_t)count : 0xFFFFFFFFu);
			if (r == 0 && !suppress) break;
			if (!suppress) count++;
		} else {
			piece[pl] = 0;
			fscanf(f, piece);
		}
	}
	RET((uint32_t)count);
}

/* --- configuration (ffecfg.c): guest only has 32 bytes for CfgStruct,
 * so keep the real host struct in a side table. */
#define MAX_CFG 8
static struct { uint32_t key; CfgStruct cfg; } cfgs[MAX_CFG];

static CfgStruct *hcfg(uint32_t g)
{
	int i, free_i = -1;
	for (i = 0; i < MAX_CFG; i++) {
		if (cfgs[i].key == g) return &cfgs[i].cfg;
		if (!cfgs[i].key && free_i < 0) free_i = i;
	}
	if (free_i < 0) guest_fatal("too many config structs");
	cfgs[free_i].key = g;
	memset(&cfgs[free_i].cfg, 0, sizeof(CfgStruct));
	return &cfgs[free_i].cfg;
}

THUNK(CfgOpen)        { RET((uint32_t)CfgOpen(hcfg(ARG(0)), HPC(ARG(1)))); }
THUNK(CfgClose)       { RET((uint32_t)CfgClose(hcfg(ARG(0)))); }
THUNK(CfgFindSection) { RET((uint32_t)CfgFindSection(hcfg(ARG(0)), HPC(ARG(1)))); }
THUNK(CfgGetKeyVal)   { RET((uint32_t)CfgGetKeyVal(hcfg(ARG(0)), HPC(ARG(1)), (int *)HP(ARG(2)))); }

/* --- BlitClipWrapper (ffemisc.c), calls back into guest blit function */
THUNK(BlitClipWrapper)
{
	uint32_t pData = ARG(0), func = ARG(6);
	int32_t xpos = (int32_t)ARG(1), ypos = (int32_t)ARG(2);
	int32_t width = (int32_t)ARG(3), height = (int32_t)ARG(4), jump = (int32_t)ARG(5);
	int32_t srcx, srcy, t, scanw = width + jump;
	uint32_t args[6];
	if (xpos >= 0) srcx = 0; else { srcx = -xpos; width += xpos; xpos = 0; }
	if (ypos >= 0) srcy = 0; else { srcy = -ypos; height += ypos; ypos = 0; }
	if ((t = xpos + width - 320) > 0) width -= t;
	if ((t = ypos + height - 200) > 0) height -= t;
	jump = scanw - width;
	pData += (uint32_t)(srcy * scanw + srcx);
	args[0] = pData; args[1] = (uint32_t)xpos; args[2] = (uint32_t)ypos;
	args[3] = (uint32_t)width; args[4] = (uint32_t)height; args[5] = (uint32_t)jump;
	guest_invoke(func, 6, args);
	RETV();
}

/* --- platform API (ffeapi.h) --- */
THUNK(SystemInit)    { SystemInit(); RETV(); }
THUNK(SystemCleanup) { SystemCleanup(); RETV(); }

THUNK(VideoBlit)        { VideoBlit((uint8_t *)HP(ARG(0)), (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5)); RETV(); }
THUNK(VideoMaskedBlit)  { VideoMaskedBlit((uint8_t *)HP(ARG(0)), (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5)); RETV(); }
THUNK(VideoReverseBlit) { VideoReverseBlit((uint8_t *)HP(ARG(0)), (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5)); RETV(); }
THUNK(VideoGetPalValue) { VideoGetPalValue((int32_t)ARG(0), (uint8_t *)HP(ARG(1))); RETV(); }
THUNK(VideoSetPalValue) { VideoSetPalValue((int32_t)ARG(0), (uint8_t *)HP(ARG(1))); RETV(); }
THUNK(VideoPointerExclusive) { RET((uint32_t)VideoPointerExclusive()); }
THUNK(VideoPointerEnable)    { VideoPointerEnable(); RETV(); }
THUNK(VideoPointerDisable)   { VideoPointerDisable(); RETV(); }

THUNK(InputMouseReadButtons) { RET((uint32_t)InputMouseReadButtons()); }
THUNK(InputMouseReadMickeys) { InputMouseReadMickeys((int32_t *)HP(ARG(0)), (int32_t *)HP(ARG(1))); RETV(); }
THUNK(InputMouseReadPos)     { InputMouseReadPos((int32_t *)HP(ARG(0)), (int32_t *)HP(ARG(1))); RETV(); }
THUNK(InputJoyReadPos)       { InputJoyReadPos((int32_t *)HP(ARG(0)), (int32_t *)HP(ARG(1))); RETV(); }
THUNK(InputJoyReadButtons)   { RET((uint32_t)InputJoyReadButtons()); }
THUNK(InputKeybReadStates)   { InputKeybReadStates((uint8_t *)HP(ARG(0))); RETV(); }
THUNK(InputKeybGetLastKey)   { RET((uint32_t)InputKeybGetLastKey()); }
THUNK(InputKeybSetLastKey)   { InputKeybSetLastKey((int32_t)ARG(0)); RETV(); }

THUNK(TimerGetTimeStamp) { RET(TimerGetTimeStamp()); }
THUNK(TimerSleep)        { TimerSleep(); RETV(); }
THUNK(TimerFrameUpdate)  { TimerFrameUpdate(); RETV(); }

THUNK(SoundCheckInit)      { SoundCheckInit((int32_t *)HP(ARG(0)), (int32_t *)HP(ARG(1)), (int32_t *)HP(ARG(2))); RETV(); }
THUNK(SoundPlaySong)       { SoundPlaySong((int32_t)ARG(0)); RETV(); }
THUNK(SoundStopSong)       { SoundStopSong(); RETV(); }
THUNK(SoundSongDone)       { RET((uint32_t)SoundSongDone()); }
THUNK(SoundStopAllSamples) { SoundStopAllSamples(); RETV(); }
THUNK(SoundPlaySample)     { SoundPlaySample((int32_t)ARG(0), (int32_t)ARG(1), (int32_t)ARG(2)); RETV(); }
THUNK(SoundStreamGetUsedBlocks) { RET((uint32_t)SoundStreamGetUsedBlocks()); }
THUNK(SoundStreamReset)    { SoundStreamReset(); RETV(); }
THUNK(SoundStreamAddBlock) { SoundStreamAddBlock(HP(ARG(0)), (int32_t)ARG(1)); RETV(); }
THUNK(SoundStreamStart)    { SoundStreamStart(); RETV(); }
THUNK(SoundStreamWait)     { SoundStreamWait(); RETV(); }
THUNK(SoundStreamStop)     { SoundStreamStop(); RETV(); }

/* Directory functions returning char*: the result may point at a host
 * buffer, so copy it into guest memory if needed. */
static uint32_t ret_str(char *res, uint32_t *slot)
{
	if (!res) return 0;
	if ((uint8_t *)res >= g_mem && (uint8_t *)res < g_mem + GUEST_MEM_SIZE) return GP(res);
	return guest_strdup_slot(slot, res);
}

static uint32_t s_avi, s_cmd, s_path;
THUNK(DirMakeAVIName)   { RET(ret_str(DirMakeAVIName(HPC(ARG(0)), HPC(ARG(1))), &s_avi)); }
THUNK(DirMakeCmmdrName) { RET(ret_str(DirMakeCmmdrName(HPC(ARG(0)), HPC(ARG(1))), &s_cmd)); }
THUNK(DirGetCmmdrPath)  { RET(ret_str(DirGetCmmdrPath(), &s_path)); }
THUNK(DirResetCmmdrPath) { DirResetCmmdrPath(); RETV(); }
THUNK(DirNavigateTree)  { DirNavigateTree((FileInfo *)HP(ARG(0))); RETV(); }
THUNK(DirFindFirst)     { RET((uint32_t)DirFindFirst((FileInfo *)HP(ARG(0)))); }
THUNK(DirFindNext)      { RET((uint32_t)DirFindNext((FileInfo *)HP(ARG(0)))); }

/* ------------------------------------------------------------------ */
/* Startup */

int asmmain(int argc, char **argv)
{
	uint32_t args[2], gargv, heap_top;
	int i;

	if (!g_mem) {
		g_mem = (uint8_t *)calloc(1, GUEST_MEM_SIZE);
		if (!g_mem) { fprintf(stderr, "Cannot allocate guest memory\n"); return 1; }
		memcpy(g_mem + GUEST_DATA_BASE, ffe_data_image, ffe_data_size);
	}
	heap_top = GUEST_STACK_TOP - GUEST_STACK_SIZE;
	heap_init(GUEST_DATA_END + 0x1000u, heap_top);
	esp = GUEST_STACK_TOP;
	FINIT();

	/* copy argv into guest memory */
	if (argc < 0 || !argv) argc = 0;
	gargv = guest_malloc(4u * (uint32_t)(argc + 1));
	for (i = 0; i < argc; i++) {
		size_t n = strlen(argv[i]) + 1;
		uint32_t s = guest_malloc((uint32_t)n);
		memcpy(g_mem + s, argv[i], n);
		WR32(gargv + 4u * (uint32_t)i, s);
	}
	WR32(gargv + 4u * (uint32_t)argc, 0);

	args[0] = (uint32_t)argc;
	args[1] = gargv;
	guest_invoke(A__asmmain, 2, args);
	run_guest_atexit();
	return (int)eax;
}
