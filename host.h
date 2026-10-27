/*
 * This is used by the C output mode of the 68k assembler.
 * Loads 68k executable into m68k memory and applies all the relocations.
 * Executable is in an atari .prg format.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "m68000.h"

//#define likely(x)       __builtin_expect((x),1)
//#define unlikely(x)     __builtin_expect((x),0)

#define LOAD_BASE	0x0
#define MEM_SIZE	(0x110000)

union Reg {
	u16	word[2];
	u32	_u32;
	u16	_u16;
	u8	_u8;
	s32	_s32;
	s16	_s16;
	s8	_s8;
};

/* m68000 state ----------------------------------------------------- */
extern s8 m68kram[MEM_SIZE];
extern union Reg Regs[16];
/* status flags */
/* it is an optimisation that instead of having a Z (zero) flag we have
 * an nZ (not zero) flag, because this way we can usually just stick
 * the result in nZ. */
extern s32 N,nZ,V,C,X;
extern s32 bN,bnZ,bV,bC,bX;
extern s32 rdest; /* return address from interrupt. zero if none in service */
extern s32 exceptions_pending;
extern s32 exceptions_pending_nums[32];
extern u32 exception_handlers[32];

void SetReg (int reg, int val);
int GetReg (int reg);

#define GetZFlag() (!nZ)
#define GetNFlag() (N)
#define GetCFlag() (C)
#define GetVFlag() (V)
#define GetXFlag() (X)
#define SetZFlag(val) nZ = !(val)

void FlagException (int num);
void load_binfile (const char *bin_filename);

/* Exceptions (the VBL) are flagged on other threads (SDL's timer thread in
 * src/main.c, the oracle's watchdog) and taken on the 68000 thread, so these
 * two variables are only ever changed with atomics.  nums[n] counts the
 * exceptions n flagged and not yet taken; bit n of exceptions_pending only
 * says "look at nums[n]":
 *   FlagException, any thread:  nums[n]++, then bit n |= (release)
 *   TakeException, the 68000 thread only:  if bit n and nums[n] > 0, nums[n]--;
 *     if that was the last one, or nums[n] was 0 (a bit set late, after its
 *     count was taken), bit n &= ~ (acquire), then bit n |= if nums[n] > 0.
 * nums[n] is never negative: only TakeException decrements it, after reading
 * it > 0, and every other write increments it.  Bit n never stays set while
 * nums[n] is 0: the next TakeException clears it.  Bit n is never left clear
 * while nums[n] > 0: |= and &= are read-modify-writes of one variable, so one
 * comes first; if the flagger's |= does, the acquire &= reads it, so its
 * nums[n]++ is seen by the re-check, which sets the bit again.  So each
 * exception flagged is taken exactly once, at a later jump check if not at
 * the first one: never lost and never taken twice.  On one
 * thread every step leaves the values the old ++/--/^= code did, which the
 * oracle hashes.  Relaxed loads are a plain ldr on arm64: the check at every
 * jump costs what it did, but cannot be torn or hoisted out of a loop. */
static inline s32 ExceptionsPending (void)
{
	return __atomic_load_n (&exceptions_pending, __ATOMIC_RELAXED);
}

static inline int TakeException (int num)
{
	s32 bit = (s32) (1u<<num), n;
	if (!(__atomic_load_n (&exceptions_pending, __ATOMIC_RELAXED) & bit)) return 0;
	n = __atomic_load_n (&exceptions_pending_nums[num], __ATOMIC_RELAXED);
	if (n > 0) n = __atomic_fetch_sub (&exceptions_pending_nums[num], 1, __ATOMIC_RELAXED);
	if (n <= 1) {
		__atomic_fetch_and (&exceptions_pending, ~bit, __ATOMIC_ACQUIRE);
		if (__atomic_load_n (&exceptions_pending_nums[num], __ATOMIC_RELAXED) > 0)
			__atomic_fetch_or (&exceptions_pending, bit, __ATOMIC_RELAXED);
	}
	return n > 0;
}

#ifdef M68K_DEBUG
#define BOUNDS_CHECK
#if 0
static inline void BOUNDS_CHECK (u32 pos, int num)
{
	if ((pos+num) > MEM_SIZE) {
		printf ("Error. 68K memory access out of bounds (address $%x, line %d).\n", pos, line_no);
		abort ();
	}
}
#endif
#endif /* M68K_DEBUG */


static inline u32 do_get_mem_long(u32 *a)
{
#ifdef __i386__
	u32 val = *a;
	__asm__ ("bswap	%0\n":"=r"(val):"0"(val));
	return val;
#elif LITTLE_ENDIAN
    u8 *b = (u8 *)a;
    return (*b << 24) | (*(b+1) << 16) | (*(b+2) << 8) | (*(b+3));
#else
    return *a;
#endif
}

static inline u16 do_get_mem_word(u16 *a)
{
#ifdef __i386__
	u16 val = *a;
	__asm__ ("rorw $8,%0" : "=q" (val) :  "0" (val));
	return val;
#elif LITTLE_ENDIAN
    u8 *b = (u8 *)a;
    return (*b << 8) | (*(b+1));
#else
    return *a;
#endif
}

static inline u8 do_get_mem_byte(u8 *a)
{
    return *a;
}

static inline void do_put_mem_long(u32 *a, u32 v)
{
#ifdef __i386__
	__asm__ ("bswap	%0\n":"=r"(v):"0"(v));
	*a = v;
#elif LITTLE_ENDIAN
    u8 *b = (u8 *)a;
    
    *b = v >> 24;
    *(b+1) = v >> 16;    
    *(b+2) = v >> 8;
    *(b+3) = v;
#else
    *a = v;
#endif
}

static inline void do_put_mem_word(u16 *a, u16 v)
{
#ifdef __i386__
	__asm__ ("rorw $8,%0" : "=q" (v) :  "0" (v));
	*a = v;
#elif LITTLE_ENDIAN
    u8 *b = (u8 *)a;
    
    *b = v >> 8;
    *(b+1) = v;
#else
    *a = v;
#endif
}

static inline void do_put_mem_byte(u8 *a, u8 v)
{
    *a = v;
}

static inline s32 rdlong (u32 pos)
{
#ifdef M68K_DEBUG
	BOUNDS_CHECK (pos,4);
#endif /* M68K_DEBUG */
	return do_get_mem_long ((u32 *)(m68kram+pos));
}
static inline s16 rdword (u32 pos)
{
#ifdef M68K_DEBUG
	BOUNDS_CHECK (pos,2);
#endif /* M68K_DEBUG */
	return do_get_mem_word ((u16 *)(m68kram+pos));
}
static inline s8 rdbyte (u32 pos)
{
#ifdef M68K_DEBUG
	BOUNDS_CHECK (pos,1);
#endif /* M68K_DEBUG */
	return do_get_mem_byte ((u8 *)(m68kram+pos));
}
static inline void wrbyte (u32 pos, int val)
{
#ifdef M68K_DEBUG
	BOUNDS_CHECK (pos,1);
#endif /* M68K_DEBUG */
	do_put_mem_byte ((u8 *)(m68kram+pos), (u8)val);
}
static inline void wrword (u32 pos, int val)
{
#ifdef M68K_DEBUG
	BOUNDS_CHECK (pos,2);
#endif /* M68K_DEBUG */
	do_put_mem_word ((u16 *)(m68kram+pos), (u16)val);
}
static inline void wrlong (u32 pos, int val)
{
#ifdef M68K_DEBUG
	BOUNDS_CHECK (pos,4);
#endif /* M68K_DEBUG */
	do_put_mem_long ((u32 *)(m68kram+pos), (u32)val);
}

#ifdef M68K_DEBUG
void m68k_print_line_no ();
#endif /* M68K_DEBUG */
