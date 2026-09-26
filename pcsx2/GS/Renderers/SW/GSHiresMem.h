/*
 * Local memory at twice the resolution, for the software renderer.
 *
 * At 2x the renderer draws every primitive twice: at native into local
 * memory, which stays the GS's own and what everything else reads, and at
 * twice the size into a second memory four times as large, which is what
 * the display shows. A native pixel owns the 2x2 pixels of the second
 * memory at twice its position.
 *
 * The second memory is laid out a native page at a time: native page p
 * owns the four pages 4p .. 4p + 3 there, as a square of two by two pages
 * (4p + 0 top left, + 1 top right, + 2 bottom left, + 3 bottom right),
 * each swizzled as a native page of the same format. So a pixel's place
 * in the second memory depends on its native page and format alone, not
 * on the buffer width it is drawn with, and runs of four or eight
 * horizontally adjacent pixels sit exactly as they do natively, which is
 * what the scanline code's stores assume.
 *
 * The address of a pixel is the sum of a part from its row and a part
 * from its column, as native addresses are, so the scanline code reaches
 * it through the same kind of row and column tables.
 *
 * The second memory holds a native block only while nothing but the 2x
 * drawing has written it since it last took the block's pixels; a block
 * written any other way is stale, and takes its native pixels again, each
 * on its 2x2, before the 2x drawing or the display next uses it. A page is
 * held in one layout at a time; used in another, all of it is stale.
 */

#ifndef GS_HIRES_MEM_H
#define GS_HIRES_MEM_H

#ifdef __cplusplus
extern "C" {
#endif

#define GS_HR_PAGES 512

/* One pixel format's page swizzle. */
typedef struct gs_hr_layout
{
   int swrow[64];  /* element offset in a page of the pixel (0, y)       */
   int swcol[64];  /* element offset of (x, 0), less that of (0, 0)      */
   int pw;         /* a page in pixels                                   */
   int ph;
   int page_elems; /* elements a page                                    */
   int elem_bytes; /* 4 or 2                                             */
   /* The pixels of each block of a page, as y * pw + x. */
   unsigned short blockpix[32][128];
   /* The pixel at each element offset of a page, as y * pw + x. */
   unsigned short inv[4096];
} gs_hr_layout_t;

/* Which layout the second memory holds each page in, and its stale
 * blocks. */
typedef struct gs_hr_pages
{
   unsigned stale[GS_HR_PAGES];    /* a bit a block of the page           */
   unsigned char psm[GS_HR_PAGES]; /* the layout's key, 0xff for none     */
} gs_hr_pages_t;

/* Completes a layout whose swrow and swcol, page size and element size
 * are set. */
void gs_hr_layout_init(gs_hr_layout_t *l);

/* Element offsets in the second memory, before wrapping (mask with
 * gs_hr_elem_mask): of the row Y of a buffer based at page `page`, `bw`
 * pages wide, and of the column X. A pixel's is the sum of the two. */
unsigned gs_hr_row(const gs_hr_layout_t *l, unsigned page, unsigned bw, int Y);
unsigned gs_hr_col(const gs_hr_layout_t *l, int X);
unsigned gs_hr_elem_mask(const gs_hr_layout_t *l);

/* Element offset in the four pages of a native page of the pixel (PX, PY)
 * of their square, 2pw by 2ph. */
unsigned gs_hr_super_elem(const gs_hr_layout_t *l, int PX, int PY);

/* The whole second memory stale, in no layout. */
void gs_hr_pages_reset(gs_hr_pages_t *s);

/* Marks a native block stale (block number, 0 .. 16383). */
void gs_hr_mark_block(gs_hr_pages_t *s, unsigned block);

/* Whether using the blocks `mask` of `page` in layout `psm` first needs
 * the page's native pixels. */
int gs_hr_page_needs(const gs_hr_pages_t *s, unsigned page, unsigned psm, unsigned mask);

/* Makes the blocks `mask` of `page` current in layout `psm`: a page held
 * in another layout is all stale first, and each stale block of the mask
 * takes its native pixels on their 2x2. vm and hr are the native and
 * second memories. */
void gs_hr_page_refresh(gs_hr_pages_t *s, const gs_hr_layout_t *l,
      unsigned psm, unsigned page, unsigned mask, const void *vm, void *hr);

/* How the display reads a format. */
#define GS_HR_READ_32 0 /* colour and alpha as they are                  */
#define GS_HR_READ_24 1 /* colour, alpha ta0                             */
#define GS_HR_READ_16 2 /* 5:5:5:1 expanded, alpha ta1 or ta0 by bit 15  */

/* Reads the 2x picture of a buffer (base page, width in pages) from native
 * pixel (x0, y0), w by h pixels of the second memory, into 32-bit RGBA
 * rows. Native coordinates wrap at 2048. colbuf holds w entries. */
void gs_hr_read(const gs_hr_layout_t *l, const void *hr, unsigned page,
      unsigned bw, int x0, int y0, int w, int h, int kind, unsigned ta0,
      unsigned ta1, unsigned *out, int out_pitch, unsigned *colbuf);

/* Reads a row of a texture at 2x: for the n native texels whose local
 * memory element addresses are `addr` (a native row), the 2n texels of
 * their 2x2s' row `sy` (0 or 1), as 32-bit RGBA. A texel whose block the
 * second memory holds in layout `psm` comes from there, expanded with
 * TEXA as the GS reads the format (GS_HR_READ_*); any other is its native
 * texel, `native[i]`, twice. */
void gs_hr_read_texels(const gs_hr_layout_t *l, const gs_hr_pages_t *s,
      unsigned psm, const void *hr, const unsigned *addr,
      const unsigned *native, int n, int sy, int kind, int aem,
      unsigned ta0, unsigned ta1, unsigned *out);

/* Sprites and textures at 2x, one axis at a time. Positions are in native
 * pixels, texture coordinates in 1/65536 texels; p0, t0 and p1, t1 are
 * the sprite's two vertices. */

/* A sprite covers the native pixels whose positions its edges enclose.
 * Drawn at twice the size from an edge between two of them, it would also
 * cover the half of the pixel before, sampled where the GS never does:
 * the edges move to the native pixel positions they cover from, the
 * texture coordinates with them along the sprite. */
void gs_hr_snap_sprite(float *p0, float *p1, float *t0, float *t1);

/* A sprite sampled nearest, its edges native pixel positions: natively
 * its first and last pixels read the texels at their positions, and at 2x
 * the samples of those pixels read inside the same texels, whether the
 * coordinates sit on texel centres or texel edges and however little the
 * sprite is scaled. The coordinates run from a quarter of the first texel
 * in from the side it is read from to a quarter from the far side of the
 * last, across the 2x pixels in between; at one texel to a pixel each
 * native pixel's texel lands on its 2x2. */
void gs_hr_fit_nearest(float p0, float p1, float *t0, float *t1);

/* Texture wrap modes, as CLAMP.WMS and WMT. */
#define GS_HR_REPEAT        0
#define GS_HR_CLAMP         1
#define GS_HR_REGION_CLAMP  2
#define GS_HR_REGION_REPEAT 3

/* One axis of a 2x texture's coordinate wrap, the native one's texels each
 * two wide: `size` texels, wrap mode `wm` with region minc, maxc. Where
 * the draw samples less than the whole texture the 2x texture holds only
 * that area, lo to hi native texels, and coordinates clamp to it, as they
 * reach no further natively. Gives tmin and tmax as the scanline code
 * takes them, the repeat mask, and the mode it runs them with: a repeat
 * by mask, or a region clamp. */
unsigned gs_hr_wrap_axis(unsigned wm, unsigned size, unsigned minc,
      unsigned maxc, int lo, int hi, unsigned short *tmin,
      unsigned short *tmax, unsigned *mask);

#ifdef __cplusplus
}
#endif

#endif
