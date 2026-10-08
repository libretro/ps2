/* The sweep GSTextureCache::ComputeSurfaceOffset runs to find surface A's
 * offset inside surface B, when A starts at or after B, starts at the row
 * GS_SURFACE_SEARCH_FIRST_ROW names (GSSurfaceSearch.h). Rows above it
 * hold only blocks below A's page, so starting there finds the offset a
 * sweep from the top of B finds, and misses where it misses - for every
 * format, buffer width and pair of base pointers tried here, through the
 * real swizzle. And it does start lower when A is pages into B: a start
 * that stayed at the top would pass the first check and sweep every
 * row above A for nothing. C89; the swizzle is reached through glue.cpp.
 */
#include <stdio.h>

#include "GS/Renderers/HW/GSSurfaceSearch.h"

int  gss_init(void);
unsigned gss_bn(unsigned psm, int x, int y, unsigned bp, unsigned bw);
void gss_dims(unsigned psm, int* bs_x, int* bs_y, int* pgs_x, int* pgs_y);

/* GS_PSM values: the colour, Z and indexed formats. */
static const unsigned PSMS[] = {
	0x00, 0x01, 0x02, 0x0a, 0x13, 0x14, 0x1b, 0x24, 0x2c,
	0x30, 0x31, 0x32, 0x3a
};

/* The sweep as the texture cache runs it, from row y0: the first <x,y>
 * in B whose block is a_bp. 1 and the hit, or 0 for a miss. */
static int sweep(unsigned psm, unsigned b_bp, unsigned bw, unsigned a_bp,
	int y0, int rect_x, int rect_z, int rect_w, int dx, int dy, int* hx, int* hy)
{
	int x, y;
	for (y = y0; y < rect_w; y += dy)
		for (x = rect_x; x < rect_z; x += dx)
			if (gss_bn(psm, x, y, b_bp, bw) == a_bp)
			{
				*hx = x;
				*hy = y;
				return 1;
			}
	return 0;
}

int main(void)
{
	unsigned i, bw, b_bp, a_off;
	long checks = 0, skipped = 0;
	int failures = 0;

	if (!gss_init())
	{
		printf("gssearch: cannot build GSLocalMemory\n");
		return 1;
	}
	for (i = 0; i < sizeof(PSMS) / sizeof(PSMS[0]); i++)
	{
		const unsigned psm = PSMS[i];
		int bs_x, bs_y, pgs_x, pgs_y;
		gss_dims(psm, &bs_x, &bs_y, &pgs_x, &pgs_y);
		for (bw = 1; bw <= 10; bw += 3)
			for (b_bp = 0; b_bp < 0x400; b_bp += 0x1a0)
				for (a_off = 0; a_off < 32 * 24; a_off += 13)
				{
					const unsigned a_bp   = b_bp + a_off;
					const unsigned b_bw   = GS_SURFACE_PAGES_PER_ROW(bw, pgs_x);
					const int rect_x = 0, rect_y = 0;
					const int rect_z = (int)bw * 64, rect_w = 1024;
					const int y0 = GS_SURFACE_SEARCH_FIRST_ROW(a_bp, b_bp, b_bw, pgs_y, rect_y);
					int fx = 0, fy = 0, sx = 0, sy = 0, full, fast;
					full = sweep(psm, b_bp, bw, a_bp, rect_y, rect_x, rect_z, rect_w, bs_x, bs_y, &fx, &fy);
					fast = sweep(psm, b_bp, bw, a_bp, y0, rect_x, rect_z, rect_w, bs_x, bs_y, &sx, &sy);
					checks++;
					if (full != fast || (full && (fx != sx || fy != sy)))
					{
						if (failures++ < 10)
							printf("  FAIL: psm %02x bw %u b %03x a %03x: from the top %d (%d,%d), from row %d %d (%d,%d)\n",
								psm, bw, b_bp, a_bp, full, fx, fy, y0, fast, sx, sy);
					}
					if (y0 > rect_y)
						skipped++;
				}
	}
	if (!skipped)
	{
		printf("  FAIL: the sweep never starts below the top of B\n");
		failures++;
	}
	printf("gssearch: %ld searches, %ld start below the top%s\n", checks, skipped,
		failures ? "" : ", all find what a sweep from the top finds");
	printf(failures ? "gssearch: FAILED (%d)\n" : "gssearch: ok\n", failures);
	return failures != 0;
}
