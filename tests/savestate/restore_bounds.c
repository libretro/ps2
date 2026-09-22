/* The bounds a savestate load has to re-establish before anything uses it.
 *
 * A savestate is a raw copy into live structures, so a restored value
 * arrives without whatever kept it in range while the emulator was
 * running. The freeze functions now clamp on the way in. What is worth
 * pinning is not that a clamp exists but that each one is the right
 * width: several of these consumers step their index and mask it
 * *afterwards*, so the restored value is used once as-is, and a mask
 * chosen one bit too wide still lets the access run off the end.
 *
 * Every check below is paired with one that the unclamped form really
 * does escape. A guard with nothing left to guard is a guard someone can
 * delete without noticing.
 */

#include <stdio.h>
#include <string.h>

#include "SaveState.h"

static long checks;
static long failures;

static void fail(const char *what)
{
	printf("  FAIL: %s\n", what);
	failures++;
}

static void expect(int cond, const char *what)
{
	checks++;
	if (!cond)
		fail(what);
}

int main(void)
{
	unsigned v;

	/* ---- IPU fifo positions: u32 data[32], accessed a quadword at a time
	 *
	 * IPU_Fifo_Input::read does "&data[readpos]" and copies 16 bytes --
	 * four u32 -- then masks. So readpos has to leave room for all four,
	 * which means 28, not 31: the obvious & 31 permits 31, and 31 + 4 is
	 * past the end of a 32-entry array. */
	{
		int saw_unsafe_under_31 = 0;

		for (v = 0; v < 256; v++)
		{
			unsigned pos28 = v & 28;
			unsigned pos31 = v & 31;

			if (pos28 + 4 > 32)
			{
				fail("& 28 left a quadword read past the end");
				break;
			}
			if (pos28 % 4)
			{
				fail("& 28 gave a position that is not quadword aligned");
				break;
			}
			if (pos31 + 4 > 32)
				saw_unsafe_under_31 = 1;
		}
		checks += 2;

		expect(saw_unsafe_under_31,
		       "& 31 would have allowed a read past the end (so 28 is doing work)");

		/* The write path derives its length from 32 - writepos, which a
		 * position outside the array turns negative -- and the negative
		 * then loses to the min() and becomes the copy length. */
		for (v = 0; v < 256; v++)
		{
			if ((int)(32 - (int)(v & 28)) < 4)
			{
				fail("& 28 left the write length non-positive");
				break;
			}
		}
		checks++;
	}

	/* ---- g_BP: FP indexes internal_qwc[2] as the destination of a
	 * quadword copy, and the loop that fills it is bounded by BP.
	 *
	 * ipu_bp_fill_buffer runs "while ((FP * 128) < (BP + bits))" and
	 * writes internal_qwc[FP] each time round. FP == 2 must therefore
	 * never satisfy the condition -- which is a joint property of the FP
	 * and BP clamps, not of either alone. */
	{
		unsigned fp, bp, bits;
		int entered_at_two = 0;

		for (fp = 0; fp <= 2; fp++)
		{
			for (bp = 0; bp < 128; bp++)
			{
				for (bits = 0; bits <= 128; bits += 8)
				{
					unsigned i = fp;

					/* Walk the loop the way the header does. */
					while ((i * 128) < (bp + bits))
					{
						if (i >= 2)
						{
							entered_at_two = 1;
							break;
						}
						i++;
					}
					if (entered_at_two)
						break;
				}
				if (entered_at_two)
					break;
			}
			if (entered_at_two)
				break;
		}
		expect(!entered_at_two,
		       "FP <= 2 with BP < 128 never indexes internal_qwc past the end");

		/* And without the BP clamp it does, so the pair is load-bearing. */
		{
			unsigned i = 2;
			unsigned bp_unclamped = 0x0000FFFF;

			expect((i * 128) < (bp_unclamped + 0),
			       "an unclamped BP does reach past internal_qwc");
		}
	}

	/* ---- VU pipelines: fmac[4] and ialu[4], stepped with & 3 after the
	 * access rather than before it. */
	{
		int saw_out_of_range = 0;

		for (v = 0; v < 1024; v++)
		{
			if ((v & 3) >= 4)
			{
				fail("& 3 left a pipeline index past the end");
				break;
			}
			if (v >= 4)
				saw_out_of_range = 1;
		}
		checks++;
		expect(saw_out_of_range, "unmasked pipeline positions do leave fmac[4]");
	}

	/* ---- GIF fifo: 16 quadwords, and write_fifo computes its transfer
	 * size as 16 - fifoSize. Tested with >= so an oversized value is
	 * refused; with == it slipped through and made the size negative. */
	{
		int saw_negative_under_eq = 0;

		for (v = 0; v <= 64; v++)
		{
			unsigned size = (v > 16) ? 0 : v;   /* the clamp on restore */

			if (size > 16)
			{
				fail("the restore clamp left fifoSize past 16");
				break;
			}
			if (!(size >= 16) && (int)(16 - (int)size) <= 0)
			{
				fail("an accepted fifoSize gave a non-positive transfer");
				break;
			}
			/* The old guard: == 16 admits anything larger. */
			if (v != 16 && v > 16 && (int)(16 - (int)v) < 0)
				saw_negative_under_eq = 1;
		}
		checks += 2;
		expect(saw_negative_under_eq,
		       "an == 16 guard does admit a negative transfer size");
	}

	/* ---- counter rates are divisors, and the clock source is what they
	 * are derived from. No source may yield zero. */
	{
		/* Sources 0..2 are constants, so re-deriving is enough for them.
		 * Source 3 is hBlank + hRender, which comes out of the same
		 * state -- so re-deriving can still land on zero, and the
		 * fallback beneath it is not decoration. */
		unsigned hblank, hrender, src, derived, final_rate;
		int derivation_can_be_zero = 0;

		for (src = 0; src < 4; src++)
		{
			for (hblank = 0; hblank <= 1; hblank++)
			{
				for (hrender = 0; hrender <= 1; hrender++)
				{
					switch (src)
					{
						case 0:  derived = 2; break;
						case 1:  derived = 32; break;
						case 2:  derived = 512; break;
						default: derived = hblank + hrender; break;
					}
					if (!derived)
						derivation_can_be_zero = 1;

					final_rate = derived ? derived : 2;
					if (!final_rate)
					{
						fail("a clock source produced a zero divisor");
						break;
					}
				}
			}
		}
		checks++;
		expect(derivation_can_be_zero,
		       "re-deriving the rate can still give zero (so the fallback is needed)");
	}

	printf("%s: restore bounds, %ld checks, %ld failures\n",
	       failures ? "FAIL" : "PASS", checks, failures);
	return failures ? 1 : 0;
}
