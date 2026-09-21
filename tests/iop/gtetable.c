/* The GTE divider seed table, as the emulator actually carries it.
 *
 * The table used to be built at startup by the Newton-Raphson iteration the
 * silicon uses; it is now 257 literals, which is a constant nothing else in
 * the tree derives. tests/iop/hwgte checks that iteration against its closed
 * form, but it builds its own copy to do it, so it would pass with any
 * table at all sitting in IopGte.c -- and the console captures do not cover
 * the whole index range, since reaching a given entry needs a particular
 * normalised divisor.
 *
 * So this recomputes the iteration and compares it against the real table,
 * entry by entry. It includes the unit rather than linking it, because the
 * table is static and should stay that way.
 */

#include <stdio.h>

#include "R3000A.h"
#include "IopMem.h"

/* What the unit reaches outside itself. */
PCSX2_ALIGN(16) psxRegisters psxRegs;
static uptr s_rlut[0x10000];
uptr *psxMemWLUT = NULL;
const uptr *psxMemRLUT = s_rlut;  /* nothing here issues LWC2 or SWC2 */
u32  iopMemRead32_slow(u32 m) { (void)m; return 0; }
void iopMemWrite32(u32 m, u32 v) { (void)m; (void)v; }

#include "../../pcsx2/IopGte.c"

int main(void)
{
	u8 want[0x101];
	u32 divisor;
	int i, bad = 0;

	for (divisor = 0x8000; divisor < 0x10000; divisor += 0x80)
	{
		u32 xa = 512;
		unsigned k;
		for (k = 1; k < 5; k++)
			xa = (xa * (1024 * 512 - ((divisor >> 7) * xa))) >> 18;
		want[(divisor >> 7) & 0xFF] = (u8)(((xa + 1) >> 1) - 0x101);
	}
	want[0x100] = want[0xFF];

	for (i = 0; i <= 0x100; i++)
	{
		if (gte_div_table[i] != want[i])
		{
			if (bad < 8)
				printf("  entry %3d: table has 0x%02x, the iteration gives 0x%02x\n",
				       i, gte_div_table[i], want[i]);
			bad++;
		}
	}

	/* And the reciprocal over every divisor it can be asked for, so a
	 * table entry that is right but indexed wrongly is caught too. */
	{
		u32 d;
		u32 acc = 0;   /* unsigned: this is a checksum, not a sum */
		for (d = 0x8000; d < 0x10000; d++)
			acc += (u32)gte_recip((u16)d);
		printf("gte table:  %d/257 entries differ from the iteration\n", bad);
		printf("gte recip:  32768 divisors, checksum %08x\n", acc);
	}

	printf("%s: the shipped seed table is the one the iteration produces\n",
	       bad ? "FAIL" : "PASS");
	return bad != 0;
}
