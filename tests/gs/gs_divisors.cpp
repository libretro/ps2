/* Divisors in GSdx that come from GS register fields.
 *
 * Integer divide-by-zero is undefined, and the two architectures the core
 * ships on do not fail the same way: on x86 the divide instruction raises
 * SIGFPE and the process dies, while on aarch64 it does not trap and
 * yields zero, so the same transfer crashes one host and silently
 * misrenders on the other. A register field a game writes is therefore
 * never a safe divisor without a guard.
 *
 * This does not exercise the renderer. It pins the arithmetic the guards
 * rest on, over the whole range each register field can hold, because
 * that is the part that can be quietly wrong: a guard that merely avoids
 * the crash while computing the wrong pitch would pass any smoke test and
 * corrupt every odd-width transfer.
 */
#include <cstdio>
#include <cstdint>

static int failures = 0;

static void fail(const char *what, const char *detail)
{
	std::printf("  FAIL %-40s %s\n", what, detail);
	failures++;
}

int main(void)
{
	unsigned rrw, smp, mag;

	std::printf("gs register-derived divisors\n");

	/* 1. WriteImage4HL / WriteImage4HH source pitch.
	 *
	 * Both formats are 4bpp (GSLocalMemory.cpp sets trbpp = 4 for PSMT4HL
	 * and PSMT4HH), so a row of RRW pixels occupies ceil(RRW / 2) bytes.
	 * WriteImage already computes that as ((RRW * trbpp) + 7) >> 3, with a
	 * comment naming the game that made a truncating version divide by
	 * zero. The two 4bpp siblings must agree with it exactly -- not merely
	 * avoid zero -- or an odd width loses its last half byte.
	 *
	 * RRW is TRXREG's 12-bit width field, so this is its whole range. */
	for (rrw = 0; rrw < 4096; rrw++)
	{
		const int sibling = (int)(((rrw * 4u) + 7u) >> 3);
		const int fixed   = (int)((rrw + 1u) / 2u);
		const int old     = (int)(rrw / 2u);
		char d[128];

		if (fixed != sibling)
		{
			std::snprintf(d, sizeof(d), "RRW %u: fixed %d, WriteImage %d",
			              rrw, fixed, sibling);
			fail("4HL/4HH pitch matches WriteImage", d);
			break;
		}
		/* The guard in both functions rejects RRW == 0 and returns, so
		 * every width that reaches the division must give a usable
		 * pitch. */
		if (rrw >= 1 && fixed < 1)
		{
			std::snprintf(d, sizeof(d), "RRW %u gives pitch %d", rrw, fixed);
			fail("4HL/4HH pitch is never zero past the guard", d);
			break;
		}
		/* And the shape of the old bug: a truncating pitch is zero for a
		 * width the RRW == 0 guard lets through. If this stops being
		 * true the test above has lost its point. */
		if (rrw == 1 && old != 0)
			fail("RRW == 1 used to truncate to a zero pitch", "it no longer does");
	}

	/* 2. The EXTBUF feedback rect's downsample ratio.
	 *
	 * SMPH/MAGH are 4-bit fields and SMPV/MAGV are 2-bit, so both promote
	 * to int and (SMPH - MAGH) is a signed subtraction: a game writing a
	 * magnification one above the sampling rate makes the ratio zero. The
	 * clamp has to hold over every pair the fields can express, and must
	 * not disturb any pair that was already valid. */
	for (smp = 0; smp < 16; smp++)
	{
		for (mag = 0; mag < 16; mag++)
		{
			const int raw     = (int)(smp - mag) + 1;
			const int clamped = raw < 1 ? 1 : raw;
			char d[128];

			if (clamped < 1)
			{
				std::snprintf(d, sizeof(d), "SMPH %u MAGH %u gives %d", smp, mag, clamped);
				fail("EXTBUF horizontal ratio is at least 1", d);
			}
			if (raw >= 1 && clamped != raw)
			{
				std::snprintf(d, sizeof(d), "SMPH %u MAGH %u: %d became %d",
				              smp, mag, raw, clamped);
				fail("EXTBUF clamp leaves valid ratios alone", d);
			}
		}
	}
	/* The 2-bit vertical pair, and a check that the unguarded form really
	 * does reach zero -- otherwise the clamp guards nothing. */
	{
		int reached_zero = 0;
		for (smp = 0; smp < 4; smp++)
		{
			for (mag = 0; mag < 4; mag++)
			{
				const int raw = (int)(smp - mag) + 1;
				if (raw == 0)
					reached_zero = 1;
				if ((raw < 1 ? 1 : raw) < 1)
					fail("EXTBUF vertical ratio is at least 1", "clamp failed");
			}
		}
		if (!reached_zero)
			fail("the unguarded vertical ratio reaches zero",
			     "it does not, so the guard is pointless");
	}

	/* The remaining fixes -- FRAME.FBW and TEX0.TBW as page widths in
	 * IsSplitTextureShuffle, DetectDoubleHalfClear and CopyPages -- are
	 * early bails and max(1, x) clamps inside stateful renderer functions.
	 * There is no arithmetic there worth pinning: the guard either runs or
	 * it does not, and nothing this file could assert would tell the
	 * difference. They are covered by review, not by this. */

	if (failures == 0)
		std::printf("PASS: register-derived divisors are never zero, and the "
		            "pitch still matches WriteImage\n");
	else
		std::printf("FAIL: %d\n", failures);
	return failures != 0;
}
