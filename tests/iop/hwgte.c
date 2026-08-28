/*  GTE divider checks that need no console capture.
 *
 *  ps2autotests is a PS2 suite and has nothing for the PS1 geometry
 *  engine, so the GTE in pcsx2/IopGte.cpp has no oracle here. Two parts
 *  of it can still be checked objectively, because they have published
 *  definitions the implementation must agree with rather than merely
 *  look plausible against.
 *
 *  The 257-entry reciprocal seed table is built at run time by the
 *  Newton-Raphson iteration the silicon uses. That iteration has a closed
 *  form, so the generator can be checked against it entry by entry: if
 *  someone edits the loop, this says so.
 *
 *  The reciprocal itself is written with the 0x2000000 term factored out
 *  of the first refinement. That is exact -- 0x2000000 is a multiple of
 *  256, so splitting it across the shift loses nothing -- but it makes
 *  the code look unlike the published sequence, so both forms are run
 *  over every divisor and compared.
 *
 *  Usage: tests/iop/hwgte
 */

#include <stdio.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;

static u8 g_table[0x101];

/* Exactly as pcsx2/IopGte.cpp builds it. */
static void build_table(void)
{
	u32 divisor;
	for (divisor = 0x8000; divisor < 0x10000; divisor += 0x80)
	{
		u32 xa = 512;
		unsigned i;
		for (i = 1; i < 5; i++)
			xa = (xa * (1024 * 512 - ((divisor >> 7) * xa))) >> 18;
		g_table[(divisor >> 7) & 0xFF] = (u8)(((xa + 1) >> 1) - 0x101);
	}
	g_table[0x100] = g_table[0xFF];
}

/* The published closed form of the same table. */
static u8 table_closed_form(unsigned i)
{
	int v = (int)((0x40000 / (i + 0x100) + 1) / 2) - 0x101;
	if (v < 0)    v = 0;
	if (v > 0xFF) v = 0xFF;
	return (u8)v;
}

/* As pcsx2/IopGte.cpp writes it. */
static s32 recip_ours(u16 divisor)
{
	const s32 x   = 0x101 + g_table[(((divisor & 0x7FFF) + 0x40) >> 7)];
	const s32 tmp = (((s32)divisor * -x) + 0x80) >> 8;
	return ((x * (131072 + tmp)) + 0x80) >> 8;
}

/* The published sequence, with the constant left in place. */
static s32 recip_published(u16 d)
{
	const s32 u  = 0x101 + g_table[((s32)d - 0x7FC0) >> 7];
	const s32 d1 = (0x2000080 - ((s32)d * u)) >> 8;
	return (0x80 + (d1 * u)) >> 8;
}

int main(void)
{
	unsigned i;
	int bad_table = 0, bad_recip = 0, first_t = -1;
	u32 d;

	build_table();

	for (i = 0; i <= 0x100; i++)
		if (g_table[i] != table_closed_form(i))
		{ if (first_t < 0) first_t = (int)i; bad_table++; }

	/* Only divisors with the top bit set reach the reciprocal: gte_divide
	 * normalises before calling it. */
	for (d = 0x8000; d < 0x10000; d++)
		if (recip_ours((u16)d) != recip_published((u16)d))
			bad_recip++;

	printf("gte seed table:  %d/257 entries differ from the closed form\n",
	       bad_table);
	if (bad_table)
		printf("  first at %d: built %02x  closed form %02x\n",
		       first_t, g_table[first_t], table_closed_form((unsigned)first_t));
	printf("gte reciprocal:  %d/32768 divisors differ from the published form\n",
	       bad_recip);

	if (!bad_table && !bad_recip)
		printf("hwgte: divider matches its published definition\n");
	return (bad_table || bad_recip) ? 1 : 0;
}
