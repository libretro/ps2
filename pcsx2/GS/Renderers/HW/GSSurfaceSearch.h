/* Where GSTextureCache::ComputeSurfaceOffset starts its sweep for the
 * offset of surface A inside surface B, when A starts at or after B. C.
 *
 * A's first block lies in the page (a_bp - b_bp) / 32 pages into B, and
 * B lays its pages out bw * 64 pixels to a row - for a format whose page
 * is 128 wide that can leave half a page at the end of each row, over
 * the start of the next. Rows of
 * pages before the one holding A's page hold only blocks below it, so the
 * sweep starts at that row, or at the top of B's rectangle when that is
 * further down; the row is counted with the half page as a whole one, so
 * a hit through the overhang of the row above is not stepped over.
 *
 * a_bp, b_bp: the two base pointers, in blocks, a_bp >= b_bp
 * b_bw:       GS_SURFACE_PAGES_PER_ROW of B
 * pgs_y:      B's page height in pixels
 * rect_y:     the top of B's rectangle */
#ifndef GS_SURFACE_SEARCH_H
#define GS_SURFACE_SEARCH_H

/* Pages per row of a buffer bw * 64 pixels wide, of a format whose page
 * is pgs_x pixels wide, with a part page counted whole. */
#define GS_SURFACE_PAGES_PER_ROW(bw, pgs_x) \
	((bw) ? ((unsigned)(bw) * 64u + (unsigned)(pgs_x) - 1u) / (unsigned)(pgs_x) : 1u)

#define GS_SURFACE_SEARCH_FIRST_ROW(a_bp, b_bp, b_bw, pgs_y, rect_y) \
	((int)((((a_bp) - (b_bp)) >> 5) / (b_bw) * (pgs_y)) > (int)(rect_y) \
		? (int)((((a_bp) - (b_bp)) >> 5) / (b_bw) * (pgs_y)) : (int)(rect_y))

#endif
