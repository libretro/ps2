/* An EE upload into a render target at another buffer width marks the
 * target pixels it wrote.
 *
 * The EE can write a few blocks into memory a wider render target covers --
 * palettes uploaded into the unused corner of a frame buffer, say. The
 * texture cache marks the target dirty where the upload landed
 * (GSTextureCache::DirtyRectByPage), so a later read of that area takes
 * local memory and the target is refreshed there. A small colour upload
 * laid out as the target is places each of its blocks by the block's own
 * address: its page on the target's page grid, its position in the page by
 * its number, its pixels where they sit in the block. The page re-layout
 * the rest of the function uses works in the upload's width, and for an
 * upload one page wide into a ten-page target put the block a row of blocks
 * away; the palette there then read the target's stale copy.
 *
 * Pinned here, against the GS's own PSMCT32 addressing (block and column
 * tables): for uploads at several widths, offsets and sizes, the pixels the
 * rule marks are exactly the target pixels sharing an address with an
 * uploaded pixel. The negative control places blocks row by row within the
 * page, the shape the re-layout produced.
 *
 * Build and run, from tests/gstc:
 *   cc -O2 -std=c89 -pedantic -Wall upload_dirty.c -o upload_dirty
 *   ./upload_dirty
 */
#include <stdio.h>
#include <string.h>

typedef unsigned int u32;

static const unsigned char block_table[4][8] = {
   { 0, 1, 4, 5, 16, 17, 20, 21 },
   { 2, 3, 6, 7, 18, 19, 22, 23 },
   { 8, 9, 12, 13, 24, 25, 28, 29 },
   { 10, 11, 14, 15, 26, 27, 30, 31 }
};

static const unsigned char column_table32[8][8] = {
   { 0, 1, 4, 5, 8, 9, 12, 13 },
   { 2, 3, 6, 7, 10, 11, 14, 15 },
   { 16, 17, 20, 21, 24, 25, 28, 29 },
   { 18, 19, 22, 23, 26, 27, 30, 31 },
   { 32, 33, 36, 37, 40, 41, 44, 45 },
   { 34, 35, 38, 39, 42, 43, 46, 47 },
   { 48, 49, 52, 53, 56, 57, 60, 61 },
   { 50, 51, 54, 55, 58, 59, 62, 63 }
};

/* PSMCT32 word address of pixel (x, y) in a buffer at block bp, bw pages wide. */
static u32 word32(u32 x, u32 y, u32 bp, u32 bw)
{
   const u32 page = (y / 32) * bw + x / 64;
   const u32 block = bp + page * 32 + block_table[(y % 32) / 8][(x % 64) / 8];
   return (block & 16383u) * 64 + column_table32[y % 8][x % 8];
}

/* The block number of pixel (x, y): GSSwizzleInfo::bn for PSMCT32. */
static u32 bn32(u32 x, u32 y, u32 bp, u32 bw)
{
   return (bp + ((y / 32) * bw + x / 64) * 32 + block_table[(y % 32) / 8][(x % 64) / 8]) & 16383u;
}

#define TW 640
#define TH 128
static unsigned char marked[TH][TW], truth[TH][TW];

static void mark(int x0, int y0, int x1, int y1)
{
   int x, y;
   for (y = y0; y < y1; y++)
      for (x = x0; x < x1; x++)
         if (x >= 0 && y >= 0 && x < TW && y < TH)
            marked[y][x] = 1;
}

/* The rule: each block of the upload at its own address in the target. */
static void rule(u32 sbp, u32 sbw, int rx0, int ry0, int rx1, int ry1, u32 tbp, u32 tbw, int row_major)
{
   int bx, by;
   for (by = ry0 & ~7; by < ry1; by += 8)
      for (bx = rx0 & ~7; bx < rx1; bx += 8)
      {
         const u32 rel = (bn32((u32)bx, (u32)by, sbp, sbw) - tbp) & 16383u;
         const u32 page = rel >> 5, blk = rel & 31;
         int px = -1, py = 0, x, y;
         if (row_major)
         {
            px = (int)(blk % 8) * 8;
            py = (int)(blk / 8) * 8;
         }
         else
            for (y = 0; y < 32 && px < 0; y += 8)
               for (x = 0; x < 64; x += 8)
                  if ((bn32((u32)x, (u32)y, 0, 1) & 31) == blk)
                  {
                     px = x;
                     py = y;
                     break;
                  }
         {
            /* the upload's part of this block, moved to where the block is */
            const int x0 = bx > rx0 ? bx : rx0, y0 = by > ry0 ? by : ry0;
            const int x1 = bx + 8 < rx1 ? bx + 8 : rx1, y1 = by + 8 < ry1 ? by + 8 : ry1;
            const int ox = (int)(page % tbw) * 64 + px - bx, oy = (int)(page / tbw) * 32 + py - by;
            mark(x0 + ox, y0 + oy, x1 + ox, y1 + oy);
         }
      }
}

/* The oracle: target pixels whose address an uploaded pixel wrote. */
static void oracle(u32 sbp, u32 sbw, int rx0, int ry0, int rx1, int ry1, u32 tbp, u32 tbw)
{
   static unsigned char written[16384 * 64 / 8];
   int x, y;
   memset(written, 0, sizeof(written));
   for (y = ry0; y < ry1; y++)
      for (x = rx0; x < rx1; x++)
      {
         const u32 w = word32((u32)x, (u32)y, sbp, sbw);
         written[w >> 3] |= (unsigned char)(1u << (w & 7));
      }
   for (y = 0; y < TH; y++)
      for (x = 0; x < TW; x++)
      {
         const u32 w = word32((u32)x, (u32)y, tbp, tbw);
         truth[y][x] = (written[w >> 3] >> (w & 7)) & 1;
      }
}

static int agrees(void)
{
   return memcmp(marked, truth, sizeof(marked)) == 0;
}

int main(void)
{
   /* sbp, sbw, rect; the target is 640x128 at 0x1a40, ten pages wide */
   static const struct
   {
      u32 sbp, sbw;
      int x0, y0, x1, y1;
   } ups[] = {
      { 0x1ed0, 1, 0, 0, 8, 2 },   /* a 16-colour palette, one page wide */
      { 0x1ed2, 1, 0, 0, 8, 2 },
      { 0x1ee0, 1, 0, 0, 16, 2 },  /* two blocks across */
      { 0x1ed4, 1, 0, 0, 16, 16 }, /* a 256-colour palette */
      { 0x1b00, 2, 8, 4, 40, 12 }, /* two pages wide, off a block's corner */
      { 0x1c40, 4, 0, 0, 64, 8 }   /* a page's width of blocks */
   };
   const u32 tbp = 0x1a40, tbw = 10;
   int fail = 0, caught = 0;
   size_t i;
   for (i = 0; i < sizeof(ups) / sizeof(ups[0]); i++)
   {
      oracle(ups[i].sbp, ups[i].sbw, ups[i].x0, ups[i].y0, ups[i].x1, ups[i].y1, tbp, tbw);
      memset(marked, 0, sizeof(marked));
      rule(ups[i].sbp, ups[i].sbw, ups[i].x0, ups[i].y0, ups[i].x1, ups[i].y1, tbp, tbw, 0);
      if (!agrees())
      {
         printf("  upload %u at %x, width %u: marked pixels differ from those written\n", (unsigned)i, ups[i].sbp, ups[i].sbw);
         fail++;
      }
      memset(marked, 0, sizeof(marked));
      rule(ups[i].sbp, ups[i].sbw, ups[i].x0, ups[i].y0, ups[i].x1, ups[i].y1, tbp, tbw, 1);
      caught += !agrees();
   }
   /* negative: placing blocks row by row misses the palette uploads */
   if (caught < 2)
   {
      printf("  negative: row-by-row placement agreed with the GS\n");
      fail++;
   }
   printf("upload dirty rects: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
