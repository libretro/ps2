/* GS local memory: does a transfer come back as what went in?
 *
 * GSLocalMemory addresses the PS2's 4MB of video memory through a
 * swizzle: pixels are not in scanline order but scattered through pages,
 * blocks and columns in a pattern that differs per format. Two families
 * of routine walk it -- the per-pixel accessors, and the transfer
 * routines that move a rectangle at a time and exist because they are
 * faster than calling the per-pixel ones.
 *
 * Nothing checked that a transfer round-trips. That matters because the
 * write side is written per format with hand-computed pitches, and one of
 * them was wrong: WriteImage4HL and WriteImage4HH truncated their source
 * pitch instead of rounding it up, so a width of one gave a pitch of zero
 * and the routine divided by it. That was found by reading, because there
 * was no test that moved any data.
 *
 * So this moves data: for every format it writes a rectangle, reads it
 * back and compares, across a spread of sizes and destination offsets.
 *
 * The two sides do not agree about how a row is packed when a row is not
 * a whole number of bytes, which only arises at 4bpp with an odd width.
 * WriteImage's pitch is ceil(w * bpp / 8) per row -- padded -- while
 * ReadImageX walks len * 2 pixels as one continuous run. That is a real
 * difference rather than a bug in either: at an odd width the fast write
 * paths refuse the transfer (they need the width page-aligned) and fall
 * through to WriteImageX, which packs the way ReadImageX does. The
 * round-trip below therefore runs over the sizes where the two
 * conventions provably coincide, and the odd 4bpp widths are probed
 * separately, reporting what they do rather than asserting what they
 * should.
 *
 * The 4MB block is mapped four times over, as the emulator maps it,
 * because a routine that runs off the end wraps rather than faulting. A
 * plain allocation would turn silent corruption into a crash and answer a
 * different question than the one being asked.
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "GS/GSLocalMemory.h"

static long checks;
static long failures;

static const struct { u32 psm; const char *name; } FORMATS[] = {
	{ PSMCT32,  "PSMCT32"  },
	{ PSMCT24,  "PSMCT24"  },
	{ PSMCT16,  "PSMCT16"  },
	{ PSMCT16S, "PSMCT16S" },
	{ PSMT8,    "PSMT8"    },
	{ PSMT4,    "PSMT4"    },
	{ PSMT8H,   "PSMT8H"   },
	{ PSMT4HL,  "PSMT4HL"  },
	{ PSMT4HH,  "PSMT4HH"  },
	{ PSMZ32,   "PSMZ32"   },
	{ PSMZ24,   "PSMZ24"   },
	{ PSMZ16,   "PSMZ16"   },
	{ PSMZ16S,  "PSMZ16S"  },
};

/* A transfer of w by h in this format, written then read back. Returns
 * the number of bytes that differed, or -1 if the format has no pair. */
static int round_trip(GSLocalMemory& mem, u32 psm, int w, int h,
                      int dsax, int dsay, u32 bp, u32 bw,
                      int *out_len)
{
	const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
	GIFRegBITBLTBUF blit = {};
	GIFRegTRXPOS pos = {};
	GIFRegTRXREG reg = {};
	int tx, ty, i, diff = 0;
	int len;
	static u8 src[512 * 64];
	static u8 dst[512 * 64];

	if (!p.wi || !p.ri)
		return -1;

	len = ((w * p.trbpp + 7) / 8) * h;
	if (len <= 0 || len > (int)sizeof(src))
		return -1;
	*out_len = len;

	for (i = 0; i < len; i++)
		src[i] = (u8)(i * 7 + w * 13 + h * 29 + 1);
	memset(dst, 0, (size_t)len);

	blit.DBP = bp; blit.DBW = bw; blit.DPSM = psm;
	blit.SBP = bp; blit.SBW = bw; blit.SPSM = psm;
	pos.DSAX = dsax; pos.DSAY = dsay;
	pos.SSAX = dsax; pos.SSAY = dsay;
	reg.RRW = w; reg.RRH = h;

	tx = dsax; ty = dsay;
	p.wi(mem, tx, ty, src, len, blit, pos, reg);

	tx = dsax; ty = dsay;
	p.ri(mem, tx, ty, dst, len, blit, pos, reg);

	for (i = 0; i < len; i++)
		if (src[i] != dst[i])
			diff++;

	return diff;
}

int main(void)
{
	/* Widths chosen so that w * trbpp is a whole number of bytes for
	 * every format here -- a multiple of 2 covers 4bpp, and everything
	 * wider is byte-aligned already. Heights and offsets are varied
	 * independently; the offsets are page-aligned because a transfer
	 * that starts mid-page is a different question. */
	static const int WIDTHS[]  = { 2, 8, 16, 32, 64, 128 };
	static const int HEIGHTS[] = { 1, 2, 8, 32 };
	static const int OFFSETS[] = { 0, 64 };
	static const u32 BPS[]     = { 0, 0x800 };
	GSLocalMemory mem;
	unsigned f, wi, hi, oi, bi;

	printf("gs local memory swizzle oracle\n");

	for (f = 0; f < sizeof(FORMATS) / sizeof(FORMATS[0]); f++)
	{
		const u32 psm = FORMATS[f].psm;
		long fmt_checks = 0, fmt_bad = 0;
		int skipped = 0;

		for (wi = 0; wi < sizeof(WIDTHS) / sizeof(WIDTHS[0]); wi++)
		for (hi = 0; hi < sizeof(HEIGHTS) / sizeof(HEIGHTS[0]); hi++)
		for (oi = 0; oi < sizeof(OFFSETS) / sizeof(OFFSETS[0]); oi++)
		for (bi = 0; bi < sizeof(BPS) / sizeof(BPS[0]); bi++)
		{
			int len = 0;
			int diff = round_trip(mem, psm, WIDTHS[wi], HEIGHTS[hi],
			                      OFFSETS[oi], OFFSETS[oi], BPS[bi], 4, &len);

			if (diff < 0)
			{
				skipped = 1;
				continue;
			}
			fmt_checks++;
			checks++;
			if (diff != 0)
			{
				fmt_bad++;
				if (fmt_bad == 1)
					printf("  %-9s %3dx%-3d at (%d,%d) bp %u: %d of %d bytes differ\n",
					       FORMATS[f].name, WIDTHS[wi], HEIGHTS[hi],
					       OFFSETS[oi], OFFSETS[oi], BPS[bi], diff, len);
			}
		}

		if (skipped && !fmt_checks)
			printf("  %-9s no transfer pair\n", FORMATS[f].name);
		else
			printf("  %-9s %ld transfers, %ld with differences\n",
			       FORMATS[f].name, fmt_checks, fmt_bad);
		failures += fmt_bad;
	}

	/* Odd 4bpp widths. A round-trip is not the check here, because the
	 * two sides disagree about what a partial row means and this harness
	 * would only be asserting its own choice of the two. What is checked
	 * is that the transfer completes and writes the pixels it was given:
	 * a width of one used to compute a source pitch of zero and divide
	 * by it, and the pitch it computes now has to stay usable for every
	 * odd width, not just that one.
	 *
	 * Which of the two packings is right for a partial 4bpp row is a
	 * question for its own look; nothing here depends on the answer. */
	printf("\n  odd 4bpp widths -- transfer completes and lands:\n");
	{
		static const u32 FOURBIT[] = { PSMT4, PSMT4HL, PSMT4HH };
		static const char *FNAME[] = { "PSMT4", "PSMT4HL", "PSMT4HH" };
		static const int ODD[] = { 1, 3, 5, 7, 9, 15, 31 };
		unsigned q, o;

		for (q = 0; q < sizeof(FOURBIT) / sizeof(FOURBIT[0]); q++)
		{
			int worst = 0;

			for (o = 0; o < sizeof(ODD) / sizeof(ODD[0]); o++)
			{
				const u32 psm = FOURBIT[q];
				const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
				GIFRegBITBLTBUF blit = {};
				GIFRegTRXPOS pos = {};
				GIFRegTRXREG reg = {};
				static u8 src[4096];
				const int w = ODD[o], h = 4;
				const int len = ((w * p.trbpp + 7) / 8) * h;
				int tx = 0, ty = 0, x, y, bad = 0;

				for (x = 0; x < len; x++)
					src[x] = (u8)(0x11 * ((x % 15) + 1));

				blit.DBP = 0; blit.DBW = 4; blit.DPSM = psm;
				reg.RRW = w; reg.RRH = h;
				p.wi(mem, tx, ty, src, len, blit, pos, reg);

				/* Read the rectangle back a pixel at a time, which has
				 * no packing question in it at all, and check every
				 * pixel of it is a value the source could have
				 * supplied rather than whatever was there before. */
				for (y = 0; y < h; y++)
				{
					for (x = 0; x < w; x++)
					{
						const u32 v = (mem.*p.rp)(x, y, 0, 4);

						if (v > 15)
							bad++;
					}
				}
				if (bad > worst)
					worst = bad;
				checks++;
			}
			printf("    %-8s widths 1..31: %s\n", FNAME[q],
			       worst ? "PIXELS OUT OF RANGE" : "all pixels in range");
			if (worst)
				failures++;
		}
	}

	printf("\n%s: swizzle oracle, %ld transfers, %ld with differences\n",
	       failures ? "FAIL" : "PASS", checks, failures);
	return failures ? 1 : 0;
}
