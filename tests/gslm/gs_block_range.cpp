/* Does a rectangle's block range come from its corners?
 *
 * The texture cache decides whether a write to local memory touches a
 * cached surface by reducing both to a range of block numbers and
 * comparing. GSTextureCache::Surface::Inside and ::Overlaps build the
 * rectangle's range from two corners:
 *
 *     const u32 start_block = off.bnNoWrap(rect.x, rect.y);
 *     const u32 end_block   = off.bnNoWrap(rect.z - 1, rect.w - 1);
 *
 * which is only the range if the block numbering rises monotonically
 * from the top-left of a rectangle to its bottom-right. Within a page it
 * does not: the block swizzle is a scatter, and the order it visits
 * blocks in differs per format. Overlaps says as much in a comment --
 * "will not be correct for Z formats, as the block swizzle is not
 * sequential" -- and then rounds the end up to a page boundary when the
 * rectangle happens to be page aligned.
 *
 * Getting this wrong in one direction leaves a stale texture on screen,
 * because a write that should have invalidated a surface reports as
 * missing it. In the other it invalidates surfaces the write never
 * touched, which costs only time. So the useful measurement is not
 * whether the shortcut is exact -- it is not, and the code knows -- but
 * whether it ever reports a range *narrower* than the truth, and for
 * which formats and shapes.
 *
 * The truth here is the swizzle itself: every block the rectangle covers,
 * enumerated through the same bnNoWrap the shortcut calls, and the real
 * minimum and maximum taken over all of them.
 */
#include <cstdio>
#include <cstring>

#include "GS/GSLocalMemory.h"

static long checks;
static long narrower;

static const struct { u32 psm; const char *name; } FORMATS[] = {
	{ PSMCT32,  "PSMCT32"  },
	{ PSMCT24,  "PSMCT24"  },
	{ PSMCT16,  "PSMCT16"  },
	{ PSMCT16S, "PSMCT16S" },
	{ PSMT8,    "PSMT8"    },
	{ PSMT4,    "PSMT4"    },
	{ PSMZ32,   "PSMZ32"   },
	{ PSMZ24,   "PSMZ24"   },
	{ PSMZ16,   "PSMZ16"   },
	{ PSMZ16S,  "PSMZ16S"  },
};

int main(void)
{
	GSLocalMemory mem;
	unsigned f, i;

	/* In units of the format's own page, because a page is 64x32 at 32bpp
	 * and 64x64 at 16bpp and 128x64 at 4bpp. A table of fixed pixel sizes
	 * calls the same rectangle "one page" for one format and half a page
	 * for another, and then the page-aligned repair fires for one and not
	 * the other -- which reads as a difference between formats when it is
	 * only a difference between the rectangles. Offsets are in pages too,
	 * except the last, which is deliberately off both grids. */
	static const struct { int px, py, pw, ph, ox, oy; const char *what; } RECTS[] = {
		{ 0, 0, 1, 1, 0, 0, "one page"        },
		{ 0, 0, 4, 4, 0, 0, "4x4 pages"       },
		{ 0, 0, 1, 8, 0, 0, "tall"            },
		{ 0, 0, 8, 1, 0, 0, "wide"            },
		{ 1, 1, 1, 1, 0, 0, "page at (1,1)"   },
		{ 0, 0, 1, 1, 8, 8, "off-page origin" },
		{ 0, 0, 2, 2, 8, 16, "unaligned"      },
	};

	printf("gs block range from corners\n");
	printf("  %-9s %-16s %-11s %-11s %s\n",
	       "format", "shape", "after fixup", "enumerated", "verdict");

	for (f = 0; f < sizeof(FORMATS) / sizeof(FORMATS[0]); f++)
	{
		const u32 psm = FORMATS[f].psm;
		const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
		const u32 bp = 0, bw = 4;
		const GSOffset off = mem.GetOffset(bp, bw, psm);

		for (i = 0; i < sizeof(RECTS) / sizeof(RECTS[0]); i++)
		{
			const int left   = RECTS[i].px * p.pgs.x + RECTS[i].ox;
			const int top    = RECTS[i].py * p.pgs.y + RECTS[i].oy;
			const int right  = left + RECTS[i].pw * p.pgs.x;
			const int bottom = top + RECTS[i].ph * p.pgs.y;
			u32 lo = 0xffffffffu, hi = 0;
			u32 c_start, c_end;
			int x, y;
			const char *verdict;

			/* Every block the rectangle touches, through the same
			 * function the shortcut uses. Stepping from the block
			 * containing the top-left corner so an unaligned
			 * rectangle still covers the blocks it overlaps. */
			for (y = top & ~(p.bs.y - 1); y < bottom; y += p.bs.y)
			{
				for (x = left & ~(p.bs.x - 1); x < right; x += p.bs.x)
				{
					const u32 b = off.bnNoWrap(x, y);

					if (b < lo) lo = b;
					if (b > hi) hi = b;
				}
			}

			c_start = off.bnNoWrap(left, top);
			c_end   = off.bnNoWrap(right - 1, bottom - 1);

			/* The two repairs Overlaps makes before comparing. Modelled
			 * rather than called, because Surface::Overlaps needs the
			 * whole texture cache; this is the arithmetic from
			 * GSTextureCache.cpp and has to be kept beside it. */
			if ((right & (p.pgs.x - 1)) == 0 && (bottom & (p.pgs.y - 1)) == 0)
			{
				const u32 page_mask = (1u << 5) - 1;

				c_end = (((c_end + page_mask) & ~page_mask)) - 1;
				c_start &= ~page_mask;
			}
			else if (c_end < c_start && ((c_start - c_end) < (1u << 5)))
			{
				const u32 t = c_start;

				c_start = c_end;
				c_end = t;
			}

			checks++;

			/* Narrower than the truth is the one that matters: a
			 * write inside the real range but outside the reported
			 * one is a missed invalidation. */
			if (c_start > lo || c_end < hi)
			{
				verdict = "NARROWER";
				narrower++;
			}
			else if (c_start == lo && c_end == hi)
				verdict = "exact";
			else
				verdict = "wider (safe)";

			printf("  %-9s %-16s %4u..%-6u %4u..%-6u %s\n",
			       FORMATS[f].name, RECTS[i].what,
			       c_start, c_end, lo, hi, verdict);
		}
	}

	printf("\n%ld shapes, %ld reported a narrower range than the blocks cover\n",
	       checks, narrower);

	/* No verdict on the count: this reports what the shortcut does, and
	 * whether it is acceptable is a question about the callers, not about
	 * the arithmetic. Failing here would only encode today's answer. */
	return 0;
}
