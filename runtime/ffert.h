/*
 * ffert.h - runtime for the recompiled (asm -> C) JJFFE core.
 *
 * The original game core is 32-bit x86 assembly.  tools/asm2c.py translates
 * it into C that runs on 64-bit hosts.  Guest pointers are 32-bit offsets
 * into g_mem, registers are globals, flags are evaluated lazily and the x87
 * FPU is emulated with doubles.
 */
#ifndef FFERT_H
#define FFERT_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define FFE_INLINE static __inline
#define FFE_NORETURN __declspec(noreturn)
#else
#define FFE_INLINE static inline
#define FFE_NORETURN __attribute__((noreturn))
#endif

/* ------------------------------------------------------------------ */
/* Guest memory */

#define GUEST_MEM_SIZE   (128u * 1024u * 1024u)
#define GUEST_STACK_SIZE (4u * 1024u * 1024u)
#define GUEST_STACK_TOP  (GUEST_MEM_SIZE - 0x1000u)

extern uint8_t *g_mem;

FFE_INLINE uint32_t RD8(uint32_t a)  { return g_mem[a]; }
FFE_INLINE uint32_t RD16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }
FFE_INLINE uint32_t RD32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
FFE_INLINE uint64_t RD64(uint32_t a) { uint64_t v; memcpy(&v, g_mem + a, 8); return v; }
FFE_INLINE void WR8(uint32_t a, uint32_t v)  { g_mem[a] = (uint8_t)v; }
FFE_INLINE void WR16(uint32_t a, uint32_t v) { uint16_t t = (uint16_t)v; memcpy(g_mem + a, &t, 2); }
FFE_INLINE void WR32(uint32_t a, uint32_t v) { memcpy(g_mem + a, &v, 4); }
FFE_INLINE void WR64(uint32_t a, uint64_t v) { memcpy(g_mem + a, &v, 8); }
FFE_INLINE double RDF32(uint32_t a) { float f; memcpy(&f, g_mem + a, 4); return (double)f; }
FFE_INLINE double RDF64(uint32_t a) { double d; memcpy(&d, g_mem + a, 8); return d; }
FFE_INLINE void WRF32(uint32_t a, double d) { float f = (float)d; memcpy(g_mem + a, &f, 4); }
FFE_INLINE void WRF64(uint32_t a, double d) { memcpy(g_mem + a, &d, 8); }

/* Guest <-> host pointer conversion */
#define HP(a)  ((a) ? (void *)(g_mem + (uint32_t)(a)) : (void *)0)
#define HPC(a) ((char *)HP(a))
/* non-null variant for libc calls that require valid pointers */
#define HPN(a) ((void *)(g_mem + (uint32_t)(a)))
#define HPNC(a) ((char *)HPN(a))
uint32_t GP(const void *p);   /* host pointer inside g_mem -> guest addr */

/* ------------------------------------------------------------------ */
/* Registers */

extern uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;

#define RETADDR 0xFEEDF00Du
#define PUSH32(v) do { uint32_t _pv = (uint32_t)(v); esp -= 4u; WR32(esp, _pv); } while (0)
#define PUSH16(v) do { uint32_t _pv = (uint32_t)(v); esp -= 2u; WR16(esp, _pv); } while (0)
/* Argument n of an external (cdecl) call, seen from inside the thunk:
 * [esp] holds the pushed return address. */
#define ARG(n) RD32(esp + 4u + 4u * (n))

/* ------------------------------------------------------------------ */
/* Lazy flags */

enum { LF_ADD, LF_SUB, LF_OTHER, LF_EXPL };
extern int lf_op, lf_sz;
extern uint32_t lf_x, lf_y, lf_res;
extern uint32_t lf_cf, lf_of, lf_zf, lf_sf, lf_pf;

#define LF_SIGN(sz) (1u << ((sz) * 8 - 1))
#define LF_MASK(sz) ((sz) == 4 ? 0xFFFFFFFFu : ((1u << ((sz) * 8)) - 1u))

#define SETF_ADD(x, y, r, sz) (lf_op = LF_ADD, lf_x = (x), lf_y = (y), lf_res = (r), lf_sz = (sz))
#define SETF_SUB(x, y, r, sz) (lf_op = LF_SUB, lf_x = (x), lf_y = (y), lf_res = (r), lf_sz = (sz))
#define SETF_LOGIC(r, sz)     (lf_op = LF_OTHER, lf_cf = 0, lf_of = 0, lf_res = (r), lf_sz = (sz))

FFE_INLINE uint32_t COND_C(void)
{
	switch (lf_op) {
	case LF_ADD: return lf_res < lf_x;
	case LF_SUB: return lf_x < lf_y;
	default: return lf_cf;
	}
}

FFE_INLINE uint32_t COND_O(void)
{
	uint32_t s = LF_SIGN(lf_sz);
	switch (lf_op) {
	case LF_ADD: return ((lf_x ^ lf_res) & (lf_y ^ lf_res) & s) != 0;
	case LF_SUB: return ((lf_x ^ lf_y) & (lf_x ^ lf_res) & s) != 0;
	default: return lf_of;
	}
}

FFE_INLINE uint32_t COND_Z(void) { return lf_op == LF_EXPL ? lf_zf : (lf_res == 0); }
FFE_INLINE uint32_t COND_S(void) { return lf_op == LF_EXPL ? lf_sf : ((lf_res & LF_SIGN(lf_sz)) != 0); }

FFE_INLINE uint32_t COND_P(void)
{
	uint32_t v;
	if (lf_op == LF_EXPL) return lf_pf;
	v = lf_res & 0xFFu;
	v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
	return (~v) & 1u;
}

FFE_INLINE int32_t lf_sext(uint32_t v, int sz)
{
	return sz == 4 ? (int32_t)v : sz == 2 ? (int32_t)(int16_t)v : (int32_t)(int8_t)v;
}

FFE_INLINE uint32_t COND_L(void)
{
	if (lf_op == LF_SUB) return lf_sext(lf_x, lf_sz) < lf_sext(lf_y, lf_sz);
	return COND_S() != COND_O();
}

FFE_INLINE uint32_t COND_LE(void)
{
	if (lf_op == LF_SUB) return lf_sext(lf_x, lf_sz) <= lf_sext(lf_y, lf_sz);
	return COND_Z() || (COND_S() != COND_O());
}

FFE_INLINE uint32_t COND_BE(void)
{
	if (lf_op == LF_SUB) return lf_x <= lf_y;
	return COND_C() || COND_Z();
}

/* Turn the lazy state into explicit flag values (for partial updates). */
FFE_INLINE void lf_materialize(void)
{
	if (lf_op == LF_EXPL) return;
	{
		uint32_t c = COND_C(), o = COND_O(), z = COND_Z(), s = COND_S(), p = COND_P();
		lf_cf = c; lf_of = o; lf_zf = z; lf_sf = s; lf_pf = p;
		lf_op = LF_EXPL;
	}
}

FFE_INLINE void SETF_INC(uint32_t r, int sz)
{
	uint32_t c = COND_C();
	lf_op = LF_OTHER; lf_cf = c; lf_of = (r == LF_SIGN(sz)); lf_res = r; lf_sz = sz;
}

FFE_INLINE void SETF_DEC(uint32_t r, int sz)
{
	uint32_t c = COND_C();
	lf_op = LF_OTHER; lf_cf = c; lf_of = (r == LF_SIGN(sz) - 1u); lf_res = r; lf_sz = sz;
}

FFE_INLINE void SETF_ADC(uint32_t x, uint32_t y, uint32_t c, uint32_t r, int sz)
{
	lf_op = LF_OTHER;
	lf_cf = ((uint64_t)x + y + c) > LF_MASK(sz);
	lf_of = ((x ^ r) & (y ^ r) & LF_SIGN(sz)) != 0;
	lf_res = r; lf_sz = sz;
}

FFE_INLINE void SETF_SBB(uint32_t x, uint32_t y, uint32_t c, uint32_t r, int sz)
{
	lf_op = LF_OTHER;
	lf_cf = (uint64_t)x < (uint64_t)y + c;
	lf_of = ((x ^ y) & (x ^ r) & LF_SIGN(sz)) != 0;
	lf_res = r; lf_sz = sz;
}

FFE_INLINE void SETF_MUL(int flag, uint32_t r, int sz)
{
	lf_op = LF_OTHER; lf_cf = lf_of = flag ? 1u : 0u; lf_res = r & LF_MASK(sz); lf_sz = sz;
}

/* Shifts: n is already masked to 1..31 and non-zero */
FFE_INLINE uint32_t SHIFT_SHL(uint32_t x, uint32_t n, int sz)
{
	int bits = sz * 8;
	uint32_t r = (n >= 32u) ? 0u : (x << n) & LF_MASK(sz);
	lf_op = LF_OTHER;
	lf_cf = (n <= (uint32_t)bits) ? (x >> (bits - n)) & 1u : 0u;
	lf_of = ((r & LF_SIGN(sz)) != 0) ^ lf_cf;
	lf_res = r; lf_sz = sz;
	return r;
}

FFE_INLINE uint32_t SHIFT_SHR(uint32_t x, uint32_t n, int sz)
{
	uint32_t r = (x & LF_MASK(sz)) >> n;
	lf_op = LF_OTHER;
	lf_cf = (x >> (n - 1)) & 1u;
	lf_of = (x & LF_SIGN(sz)) != 0;
	lf_res = r; lf_sz = sz;
	return r;
}

FFE_INLINE uint32_t SHIFT_SAR(uint32_t x, uint32_t n, int sz)
{
	int32_t sx = lf_sext(x, sz);
	uint32_t r = (uint32_t)(sx >> (n > 31u ? 31u : n)) & LF_MASK(sz);
	lf_op = LF_OTHER;
	lf_cf = (uint32_t)(sx >> (n - 1)) & 1u;
	lf_of = 0;
	lf_res = r; lf_sz = sz;
	return r;
}

FFE_INLINE uint32_t SHIFT_RCL(uint32_t x, uint32_t n, int sz)
{
	int bits = sz * 8;
	uint32_t c, i;
	lf_materialize();
	c = lf_cf;
	x &= LF_MASK(sz);
	for (i = 0; i < n; i++) {
		uint32_t top = (x >> (bits - 1)) & 1u;
		x = ((x << 1) | c) & LF_MASK(sz);
		c = top;
	}
	lf_cf = c;
	lf_of = ((x >> (bits - 1)) & 1u) ^ c;
	return x;
}

FFE_INLINE uint32_t SHIFT_ROL(uint32_t x, uint32_t n, int sz)
{
	int bits = sz * 8;
	n %= (uint32_t)bits;
	lf_materialize();
	x &= LF_MASK(sz);
	if (n) x = ((x << n) | (x >> (bits - n))) & LF_MASK(sz);
	lf_cf = x & 1u;
	lf_of = ((x >> (bits - 1)) & 1u) ^ lf_cf;
	return x;
}

FFE_INLINE uint32_t SHIFT_ROR(uint32_t x, uint32_t n, int sz)
{
	int bits = sz * 8;
	n %= (uint32_t)bits;
	lf_materialize();
	x &= LF_MASK(sz);
	if (n) x = ((x >> n) | (x << (bits - n))) & LF_MASK(sz);
	lf_cf = (x >> (bits - 1)) & 1u;
	lf_of = lf_cf ^ ((x >> (bits - 2)) & 1u);
	return x;
}

uint32_t get_eflags(void);

/* ------------------------------------------------------------------ */
/* x87 FPU */

extern double fpu_st[8];
extern int fpu_top;
extern uint16_t fpu_cw, fpu_sw;

#define ST(i) fpu_st[(fpu_top + (i)) & 7]
#define FPUSH(v) do { double _fv = (v); fpu_top = (fpu_top - 1) & 7; fpu_st[fpu_top] = _fv; } while (0)
#define FPOP() (fpu_top = (fpu_top + 1) & 7)

FFE_INLINE void FCOM(double a, double b)
{
	fpu_sw &= ~(0x4500u);           /* C3 C2 C0 */
	if (a != a || b != b) fpu_sw |= 0x4500u;
	else if (a < b) fpu_sw |= 0x0100u;
	else if (a == b) fpu_sw |= 0x4000u;
}

FFE_INLINE uint32_t FSTSW(void)
{
	return (uint32_t)((fpu_sw & ~0x3800u) | ((unsigned)(fpu_top & 7) << 11));
}

FFE_INLINE double FROUND(double x)
{
	switch ((fpu_cw >> 10) & 3) {
	case 0: {
		double f = floor(x), d = x - f;
		if (d > 0.5) return f + 1.0;
		if (d < 0.5) return f;
		return fmod(f, 2.0) == 0.0 ? f : f + 1.0;   /* ties to even */
	}
	case 1: return floor(x);
	case 2: return ceil(x);
	default: return x < 0 ? ceil(x) : floor(x);
	}
}

FFE_INLINE uint32_t FIST32(double x)
{
	double r = FROUND(x);
	if (!(r >= -2147483648.0 && r <= 2147483647.0)) return 0x80000000u;
	return (uint32_t)(int32_t)r;
}

FFE_INLINE uint32_t FIST16(double x)
{
	double r = FROUND(x);
	if (!(r >= -32768.0 && r <= 32767.0)) return 0x8000u;
	return (uint32_t)(uint16_t)(int16_t)r;
}

FFE_INLINE uint64_t FIST64(double x)
{
	double r = FROUND(x);
	if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return 0x8000000000000000ull;
	return (uint64_t)(int64_t)r;
}

FFE_INLINE void FPREM(void)
{
	double a = ST(0), b = ST(1);
	double q = (a / b);
	int64_t qi;
	q = q < 0 ? ceil(q) : floor(q);
	qi = (int64_t)q;
	ST(0) = fmod(a, b);
	fpu_sw &= ~(0x4700u);   /* C3 C2 C1 C0 */
	if (qi & 1) fpu_sw |= 0x0200u;  /* C1 = Q0 */
	if (qi & 2) fpu_sw |= 0x4000u;  /* C3 = Q1 */
	if (qi & 4) fpu_sw |= 0x0100u;  /* C0 = Q2 */
}

FFE_INLINE void FINIT(void) { fpu_top = 0; fpu_cw = 0x037F; fpu_sw = 0; }

/* SAHF: load SF ZF AF PF CF from AH */
FFE_INLINE void SAHF(void)
{
	uint32_t ah = (eax >> 8) & 0xFFu;
	uint32_t o = COND_O();
	lf_op = LF_EXPL;
	lf_of = o;
	lf_sf = (ah >> 7) & 1u;
	lf_zf = (ah >> 6) & 1u;
	lf_pf = (ah >> 2) & 1u;
	lf_cf = ah & 1u;
}

/* ------------------------------------------------------------------ */
/* String instructions (direction flag is always clear in this code) */

FFE_INLINE void rep_movs(int sz)
{
	uint32_t n = ecx * (uint32_t)sz;
	if (n) {
		if (edi <= esi || edi >= esi + n) {
			/* same result as a forward byte copy */
			memmove(g_mem + edi, g_mem + esi, n);
		} else {
			uint32_t i;   /* forward overlapping copy replicates pattern */
			for (i = 0; i < n; i++) g_mem[edi + i] = g_mem[esi + i];
		}
		esi += n; edi += n;
	}
	ecx = 0;
}
#define REP_MOVS(sz) rep_movs(sz)

FFE_INLINE void rep_stos(int sz)
{
	while (ecx) {
		if (sz == 1) WR8(edi, eax); else if (sz == 2) WR16(edi, eax); else WR32(edi, eax);
		edi += (uint32_t)sz; ecx--;
	}
}
#define REP_STOS(sz) rep_stos(sz)

FFE_INLINE void REPNE_SCASB(void)
{
	uint32_t al = eax & 0xFFu;
	while (ecx) {
		uint32_t b = RD8(edi);
		edi++; ecx--;
		SETF_SUB(al, b, (al - b) & 0xFFu, 1);
		if (al == b) break;
	}
}

FFE_INLINE void REPE_SCASB(void)
{
	uint32_t al = eax & 0xFFu;
	while (ecx) {
		uint32_t b = RD8(edi);
		edi++; ecx--;
		SETF_SUB(al, b, (al - b) & 0xFFu, 1);
		if (al != b) break;
	}
}

/* ------------------------------------------------------------------ */
/* Control transfer */

typedef struct {
	void (*region)(int e);   /* recompiled code region, or NULL */
	int entry;               /* entry index inside region */
	void (*ext)(void);       /* host thunk for external functions */
	const char *name;
} GuestCodeEntry;

extern const GuestCodeEntry ffe_code_table[];

void guest_call(uint32_t addr);          /* run code at addr (ret already pushed) */
uint32_t guest_invoke(uint32_t addr, int nargs, const uint32_t *args);
FFE_NORETURN void guest_fatal(const char *msg);
FFE_NORETURN void guest_divide_error(void);

/* SetJmp/LongJmp of the original code mapped onto host setjmp */
typedef struct {
	jmp_buf jb;
	uint32_t regs[8];
	int fpu_top;
} GuestJmp;

GuestJmp *guest_jmp_slot(uint32_t guestbuf);
extern uint32_t guest_longjmp_val;
FFE_NORETURN void guest_longjmp(void);

#define GUEST_SETJMP() do { \
	GuestJmp *_j = guest_jmp_slot(RD32(esp)); \
	_j->regs[0] = ebx; _j->regs[1] = ecx; _j->regs[2] = edx; _j->regs[3] = esi; \
	_j->regs[4] = edi; _j->regs[5] = ebp; _j->regs[6] = esp; _j->fpu_top = fpu_top; \
	if (setjmp(_j->jb)) { \
		ebx = _j->regs[0]; ecx = _j->regs[1]; edx = _j->regs[2]; esi = _j->regs[3]; \
		edi = _j->regs[4]; ebp = _j->regs[5]; esp = _j->regs[6]; fpu_top = _j->fpu_top; \
		eax = guest_longjmp_val; \
	} else eax = 0; \
} while (0)

/* Host entry point: runs the recompiled _asmmain */
int asmmain(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* FFERT_H */
