/* An 8-bit view of a 32-bit render target, on the GPU and at scale.
 *
 * Games read a colour buffer back through PSMT8 to run every byte of every
 * pixel through a palette (Ridge Racer V's intro grades its picture that
 * way, one 64x32 block at a time, three channels summed). The hardware
 * renderer builds that view with ps_convert_rgba_8i, which maps each texel
 * of the 8-bit page back to the pixel and byte it aliases. Three things
 * about that path are pinned here, each against the swizzle tables rather
 * than against itself:
 *
 *   1. The texel -> (pixel, byte) mapping the shader computes matches what
 *      the GS block and column tables say for PSMT8 over PSMCT32, for every
 *      texel of a page, including a texture that starts on a later page of
 *      the target (PageOffset) and a target pitch that differs from the
 *      texture's (SBW vs DBW).
 *   2. At an integer scale the converted texture is the scaled index
 *      texture: texel index picks the pixel, position inside the texel
 *      picks the sample inside that pixel.
 *   3. A draw that samples such a texture picks the same texels at any
 *      scale as it does at native resolution (the PS_SAMPLE_MAP path takes
 *      the coordinate at the first fragment of the native pixel), so a
 *      3:1 minifying sprite lands on the rows a native draw reads.
 *
 * Also pinned: the channel shuffle emulation, which fetches by position,
 * is only used for draws that put one channel of each texel on the pixel
 * it aliases (IsChannelShuffleIdentity), or that copy a buffer page by
 * page; Ridge Racer V's 3:1 block draws and its copies of a block to other
 * pages go through the exact conversion instead. The fetch starts at the
 * page the texture begins on, and a draw addressed at a page of a target
 * lands on that page: both use the page's position in the target, which is
 * checked against the GS page addressing, as is a read whose base lies
 * before a target but whose coordinates land on it.
 *
 * Build and run, from tests/gsindexed:
 *   cc -O2 -std=c89 -pedantic -Wall indexed_view.c -o indexed_view
 *   ./indexed_view
 */
#include <stdio.h>
#include <stdlib.h>

typedef unsigned int u32;

/* ---- GS swizzle tables (GSTables.cpp) ---------------------------------- */

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

static const unsigned char column_table8[16][16] = {
   { 0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54 },
   { 8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62 },
   { 33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23 },
   { 41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31 },
   { 96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86 },
   { 104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94 },
   { 65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119 },
   { 73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127 },
   { 128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182 },
   { 136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190 },
   { 161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151 },
   { 169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159 },
   { 224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214 },
   { 232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222 },
   { 193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247 },
   { 201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255 }
};

/* Byte address inside a page of PSMCT32 (64x32 pixels, 4 bytes each). */
static u32 addr32(u32 x, u32 y)
{
   return block_table[y / 8][x / 8] * 256u + column_table32[y % 8][x % 8] * 4u;
}

/* Byte address inside a page of PSMT8 (128x64 texels). */
static u32 addr8(u32 u, u32 v)
{
   return block_table[v / 16][u / 16] * 256u + column_table8[v % 16][u % 16];
}

/* ---- ps_convert_rgba_8i, as the three shaders write it ----------------- */

struct fetch
{
   u32 x, y;   /* pixel of the source target */
   u32 byte;   /* 0 = R, 1 = G, 2 = B, 3 = A */
};

static struct fetch shader_convert(u32 pos_x, u32 pos_y, u32 sbw, u32 dbw, u32 page_offset, u32 scale)
{
   struct fetch f;
   u32 sub_x = 0, sub_y = 0;
   u32 block_x, block_y, sub_bx, sub_by, coord_x, coord_y;
   u32 bxy_x, bxy_y, block_num, boff_x, boff_y;
   u32 is_col23, is_col13, is_col12;

   if (scale > 1)
   {
      sub_x = pos_x % scale;
      sub_y = pos_y % scale;
      pos_x /= scale;
      pos_y /= scale;
   }

   block_x = (pos_x & ~15u) >> 1;
   block_y = (pos_y & ~3u) >> 1;
   sub_bx = pos_x & 7u;
   sub_by = pos_y & 1u;
   coord_x = block_x | sub_bx;
   coord_y = block_y | sub_by;

   bxy_x = coord_x / 64u;
   bxy_y = coord_y / 32u;
   block_num = bxy_y * (dbw / 128u) + bxy_x + page_offset;
   boff_x = (block_num % (sbw / 64u)) * 64u;
   boff_y = (block_num / (sbw / 64u)) * 32u;
   coord_x = (coord_x % 64u) + boff_x;
   coord_y = (coord_y % 32u) + boff_y;

   is_col23 = pos_y & 4u;
   is_col13 = pos_y & 2u;
   is_col12 = is_col23 ^ (is_col13 << 1);
   coord_x ^= is_col12;

   f.x = coord_x * scale + sub_x;
   f.y = coord_y * scale + sub_y;
   f.byte = ((pos_y & 2u) ? 1u : 0u) | ((pos_x & 8u) ? 2u : 0u);
   return f;
}

/* GSDevice::IndexedConversionPageOffset */
static u32 page_offset_of(u32 offset_x, u32 offset_y, u32 sbw)
{
   return (offset_y / 32) * ((sbw < 64 ? 64 : sbw) / 64) + (offset_x / 64);
}

/* ---- 1. the swizzle ---------------------------------------------------- */

static int check_swizzle(void)
{
   /* Target: 640 wide (SBW 640), texture TBW 20 (DBW 1280) as Ridge Racer
    * V sets it; the texture starts `page` pages into the target. */
   static const u32 pages[] = { 0, 1, 2, 9, 10, 23, 69 };
   int fail = 0;
   size_t pi;

   for (pi = 0; pi < sizeof(pages) / sizeof(pages[0]); pi++)
   {
      const u32 page = pages[pi];
      const u32 sbw = 640, dbw = 1280;
      u32 u, v;
      /* the target pixel this page starts on (10 pages per row) */
      const u32 base_x = (page % 10) * 64, base_y = (page / 10) * 32;
      const u32 poff = page_offset_of(base_x, base_y, sbw);

      if (poff != page)
      {
         printf("  page offset: pixel offset %u,%u gives page %u, want %u\n", base_x, base_y, poff, page);
         fail++;
      }

      for (v = 0; v < 64; v++)
      {
         for (u = 0; u < 128; u++)
         {
            const u32 a = addr8(u, v);           /* byte the texel aliases */
            struct fetch f = shader_convert(u, v, sbw, dbw, poff, 1);
            u32 ax;

            if (f.x < base_x || f.x >= base_x + 64 || f.y < base_y || f.y >= base_y + 32)
            {
               if (fail++ < 5)
                  printf("  page %u texel %u,%u fetched outside its page: %u,%u\n", page, u, v, f.x, f.y);
               continue;
            }
            ax = addr32(f.x - base_x, f.y - base_y) + f.byte;
            if (ax != a)
            {
               if (fail++ < 5)
                  printf("  page %u texel %u,%u: shader byte %u, tables byte %u\n", page, u, v, ax, a);
            }
         }
      }
   }
   printf("swizzle against the block tables: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 2. the scaled conversion ------------------------------------------ */

static int check_scaled(void)
{
   static const u32 scales[] = { 2, 3, 4, 8 };
   int fail = 0;
   size_t si;

   for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
   {
      const u32 s = scales[si];
      u32 X, Y;
      for (Y = 0; Y < 64 * s; Y++)
      {
         for (X = 0; X < 128 * s; X++)
         {
            struct fetch a = shader_convert(X, Y, 640, 1280, 3, s);
            struct fetch b = shader_convert(X / s, Y / s, 640, 1280, 3, 1);
            if (a.byte != b.byte || a.x != b.x * s + X % s || a.y != b.y * s + Y % s)
            {
               if (fail++ < 5)
                  printf("  scale %u texel %u,%u: %u,%u byte %u; native %u,%u byte %u\n", s, X, Y, a.x, a.y, a.byte, b.x, b.y, b.byte);
            }
         }
      }
   }
   printf("scaled conversion is the scaled index texture: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 3. texel choice at scale ------------------------------------------ */

/* A sprite as the game gives it: pixel box and texel box, 1/16 units. */
struct sprite
{
   int x0, y0, x1, y1;   /* pixels, 1/16 */
   int u0, v0, u1, v1;   /* texels, 1/16 */
};

/* The texel row a native draw reads for pixel row Y: the coordinate at the
 * row itself, stepping by the sprite's ratio (what GSRendererHW's
 * rasterisation resolves to, and what the software renderer matches). */
static int native_row_texel(const struct sprite* s, int Y)
{
   const double dv = (double)(s->v1 - s->v0) / (double)(s->y1 - s->y0);
   const double v = (double)s->v0 / 16.0 + dv * (double)(Y * 16 - s->y0) / 16.0;
   return (int)v;
}

/* PS_SAMPLE_MAP: fragment row Y at scale S -> texel row and sample row.
 * The hardware renderer lands the sprite's first texel coordinate on the
 * centre of its first fragment row and steps by the ratio per fragment;
 * the shader takes that interpolant and moves it to the first fragment of
 * the native pixel (d = floor(frag / S) * S + 0.5 - frag). */
static void mapped_row(const struct sprite* s, int S, int Y, int* texel, int* sub)
{
   const double dv = (double)(s->v1 - s->v0) / (double)(s->y1 - s->y0) / (double)S;
   const double y0 = (double)s->y0 / 16.0 * S;
   const double frag = (double)Y + 0.5;
   const double v_frag = (double)s->v0 / 16.0 + dv * (frag - 0.5 - y0);
   const double d = (double)((Y / S) * S) + 0.5 - frag;
   *texel = (int)(v_frag + dv * d);
   *sub = Y % S;
}

static int check_sample_map(void)
{
   /* Ridge Racer V draw A: 8x6 pixel sprite from an 8x18 texel box (3:1),
    * and draw B: 16x2 from 16x2, 0.5-texel offsets as the game sends. */
   static const struct sprite sprites[] = {
      { 0 * 16, 2 * 16, 8 * 16, 8 * 16, 72, 8, 200, 296 },
      { 0 * 16, 0 * 16, 16 * 16, 2 * 16, 136, 8, 392, 40 },
      { 8 * 16, 10 * 16, 16 * 16, 16 * 16, 328, 264, 456, 552 }
   };
   static const int scales[] = { 2, 3, 4, 8 };
   int fail = 0;
   size_t si, pi;

   for (pi = 0; pi < sizeof(sprites) / sizeof(sprites[0]); pi++)
   {
      const struct sprite* s = &sprites[pi];
      for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
      {
         const int S = scales[si];
         int Y;
         for (Y = (s->y0 / 16) * S; Y < (s->y1 / 16) * S; Y++)
         {
            int texel, sub;
            const int native = native_row_texel(s, Y / S);
            mapped_row(s, S, Y, &texel, &sub);
            if (texel != native || sub != Y % S)
            {
               if (fail++ < 8)
                  printf("  sprite %u scale %d row %d: texel %d sub %d, native row %d reads texel %d\n",
                     (unsigned)pi, S, Y, texel, sub, Y / S, native);
            }
         }
      }
   }
   printf("sample map picks the native draw's texels: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 4. a page's position in its target -------------------------------- */

/* GS page addressing for a buffer of width bw pages: the page a pixel is
 * on, counted from the base pointer (GSOffset::bn, in pages). */
static u32 page_of_pixel(u32 x, u32 y, u32 bw)
{
   return (y / 32u) * bw + x / 64u;
}

/* Where the channel shuffle fetch starts, and where a draw addressed at
 * that page goes, as GSRendererHW computes it. */
static void page_position(u32 page, u32 bw, u32* x, u32* y)
{
   *x = (page % bw) * 64u;
   *y = (page / bw) * 32u;
}

static int check_page_position(void)
{
   static const u32 widths[] = { 1, 2, 4, 8, 10 };
   int fail = 0;
   size_t wi;

   for (wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++)
   {
      const u32 bw = widths[wi];
      u32 page;
      /* a 448-row buffer: 14 rows of pages */
      for (page = 0; page < bw * 14u; page++)
      {
         u32 x, y, px, py;
         page_position(page, bw, &x, &y);
         if (page_of_pixel(x, y, bw) != page || (x % 64u) || (y % 32u))
         {
            if (fail++ < 8)
               printf("  width %u page %u: position %u,%u is page %u\n", bw, page, x, y, page_of_pixel(x, y, bw));
         }
         /* every pixel of the page is on it */
         for (py = y; py < y + 32u; py += 31u)
            for (px = x; px < x + 64u; px += 63u)
               if (page_of_pixel(px, py, bw) != page)
               {
                  if (fail++ < 8)
                     printf("  width %u page %u: pixel %u,%u is page %u\n", bw, page, px, py, page_of_pixel(px, py, bw));
               }
      }
   }
   /* A texture whose base is before a target but whose coordinates land
    * on it (Ridge Racer V reads its lamp glows from a scratch buffer at
    * 0x1a40 through the display buffer's address 0x9a0, texel 733,478,
    * both 10 pages wide): the texel's page is a page of the target, and
    * the offset puts the texel on the target pixel with the same place in
    * its page. GSTextureCache::LookupSource, the read-before-target case. */
   {
      const u32 bp = 0x9a0, tbp = 0x1a40, bw = 10, u = 733, v = 478;
      const u32 page_x = u & ~63u, page_y = v & ~31u;
      const u32 rect_bp = bp + page_of_pixel(page_x, page_y, bw) * 32u;
      u32 x, y;
      int tx, ty;
      if (rect_bp < tbp || (rect_bp - tbp) % 32u)
      {
         printf("  read-before-target: texel %u,%u of 0x%x is block 0x%x, not a page of 0x%x\n", u, v, bp, rect_bp, tbp);
         fail++;
      }
      page_position((rect_bp - tbp) / 32u, bw, &x, &y);
      tx = (int)x - (int)page_x;
      ty = (int)y - (int)page_y;
      if (tx != -192 || ty != -416 || (int)u + tx != 541 || (int)v + ty != 62 ||
          tbp + page_of_pixel((u32)((int)u + tx), (u32)((int)v + ty), bw) * 32u != rect_bp)
      {
         printf("  read-before-target: offset %d,%d puts texel %u,%u at %d,%d\n", tx, ty, u, v, (int)u + tx, (int)v + ty);
         fail++;
      }
   }
   /* Tomb Raider Legend's strips: the display is 8 pages wide, a strip
    * starts two pages in, so 128 pixels across. */
   {
      u32 x, y;
      page_position(2, 8, &x, &y);
      if (x != 128 || y != 0)
      {
         printf("  page 2 of an 8-wide buffer at %u,%u, not 128,0\n", x, y);
         fail++;
      }
   }
   printf("page position in a target: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 5. the channel shuffle gate --------------------------------------- */

/* The pixel and byte a texel of the 8-bit view aliases, by the tables. */
static void aliased_pixel(u32 u, u32 v, int* x, int* y, int* byte)
{
   const u32 a = addr8(u, v);
   u32 px, py;
   for (py = 0; py < 32; py++)
      for (px = 0; px < 64; px++)
         if (addr32(px, py) == (a & ~3u))
         {
            *x = (int)px;
            *y = (int)py;
            *byte = (int)(a & 3u);
            return;
         }
   *x = *y = *byte = -1;
}

/* The same, as GSRendererHW::IsChannelShuffleIdentity computes it. */
static void renderer_pixel(int u, int v, int* x, int* y)
{
   const int flip = (((v >> 1) ^ (v >> 2)) & 1) << 2;
   *x = ((u & 7) ^ flip) | ((u >> 4) << 3);
   *y = ((v >> 2) << 1) | (v & 1);
}

static int check_alias_formula(void)
{
   int fail = 0;
   u32 u, v;
   for (v = 0; v < 64; v++)
      for (u = 0; u < 128; u++)
      {
         int tx, ty, tb, rx, ry;
         aliased_pixel(u, v, &tx, &ty, &tb);
         renderer_pixel((int)u, (int)v, &rx, &ry);
         if (tx != rx || ty != ry)
         {
            if (fail++ < 8)
               printf("  texel %u,%u: renderer says pixel %d,%d, tables say %d,%d\n", u, v, rx, ry, tx, ty);
         }
      }
   printf("gate's texel-to-pixel formula matches the tables: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* GSRendererHW::IsChannelShuffleIdentity over a sprite list, with the
 * position asked about or not (a page-by-page copy is not asked). */
static int shuffle_identity(const struct sprite* v, size_t n, int wms, int minu, int maxu, int wmt, int minv, int maxv, int position)
{
   const int u_masked = wms == 3 && (minu & 8) == 0;
   const int v_masked = wmt == 3 && (minv & 2) == 0;
   size_t i;

   for (i = 0; i < n; i++)
   {
      const int w = abs(v[i].x1 - v[i].x0) >> 4;
      const int h = abs(v[i].y1 - v[i].y0) >> 4;
      const int uw = abs(v[i].u1 - v[i].u0) >> 4;
      const int vh = abs(v[i].v1 - v[i].v0) >> 4;
      const int umin = v[i].u0 < v[i].u1 ? v[i].u0 : v[i].u1;
      const int vmin = v[i].v0 < v[i].v1 ? v[i].v0 : v[i].v1;
      const int x0 = (v[i].x0 < v[i].x1 ? v[i].x0 : v[i].x1) >> 4;
      const int y0 = (v[i].y0 < v[i].y1 ? v[i].y0 : v[i].y1) >> 4;
      int u_first, u_last, v_first, v_last, u, vv, sx, sy, byte;

      if (w == 0 || h == 0 || uw != w || vh != h)
         return 0;
      /* the sprite's first coordinate is what its first pixel reads */
      u_first = umin >> 4;
      u_last = (umin + ((w - 1) << 4)) >> 4;
      v_first = vmin >> 4;
      v_last = (vmin + ((h - 1) << 4)) >> 4;
      if (!u_masked && (u_first >> 3) != (u_last >> 3))
         return 0;
      if (!v_masked && (v_first >> 1) != (v_last >> 1))
         return 0;
      if (!position)
         continue;

      u = u_first;
      vv = v_first;
      if (wms == 3)
         u = (u & minu) | maxu;
      if (wmt == 3)
         vv = (vv & minv) | maxv;
      aliased_pixel((u32)u, (u32)vv, &sx, &sy, &byte);
      if (sx != x0 || sy != y0)
         return 0;
   }
   return 1;
}

static int check_shuffle_gate(void)
{
   /* Ridge Racer V draw A (WMS 3 MINU 1015, WMT 3 MINV 1017): 3:1 in V. */
   static const struct sprite rr5_a[] = {
      { 0, 32, 128, 128, 72, 8, 200, 296 },
      { 128, 32, 256, 128, 328, 8, 456, 296 }
   };
   /* Ridge Racer V draw B (WMS 3 MINU 1015, WMT 1): 16x2 sprites, each
    * on the pixels its texels alias, and Tomb Raider Legend's red copy,
    * which is the same draw with rows of 8x2 sprites between (its two
    * kinds of row, then a row that reads the flipped columns). */
   static const struct sprite copy[] = {
      { 0, 0, 256, 32, 136, 8, 392, 40 },
      { 256, 0, 512, 32, 648, 8, 904, 40 },
      { 0, 32, 128, 64, 72, 72, 200, 104 },
      { 128, 32, 256, 64, 328, 72, 456, 104 },
      { 0, 64, 256, 96, 136, 136, 392, 168 }
   };
   /* The same page put at x 64 of the frame (Ridge Racer V's second and
    * third copies of a block): not where the texels alias. */
   static const struct sprite moved[] = {
      { 1024, 0, 1280, 32, 136, 8, 392, 40 }
   };
   /* Tomb Raider Legend's alpha pass: green texels copied 16 rows down. */
   static const struct sprite shifted[] = {
      { 512, 288, 768, 320, 136, 104, 392, 136 },
      { 0, 288, 256, 320, 1160, 104, 1416, 136 }
   };
   /* A channel shuffle drawn as 8x2 pixel sprites, each from its own
    * 8x2 texel box on one channel. Pixel rows 2-3 of a block hold their
    * red bytes in the flipped column order, so the sprite there starts
    * its box at texel 4 and needs bit 3 of U masked to stay on red. */
   static const struct sprite real[] = {
      { 0, 0, 128, 32, 0, 0, 128, 32 },
      { 128, 0, 256, 32, 256, 0, 384, 32 },
      { 0, 32, 128, 64, 64, 64, 192, 96 }
   };
   /* The same box put on rows 2-3 unflipped reads red bytes of other
    * pixels: what the game gets is not what the copy would give it. */
   static const struct sprite unflipped[] = {
      { 0, 32, 128, 64, 0, 64, 128, 96 }
   };
   int fail = 0;

   if (shuffle_identity(rr5_a, 2, 3, 1015, 0, 3, 1017, 0, 1))
   {
      printf("  Ridge Racer V draw A taken for a channel shuffle\n");
      fail++;
   }
   if (!shuffle_identity(copy, 5, 3, 1015, 0, 1, 0, 0, 1))
   {
      printf("  a block copy onto the pixels it aliases refused\n");
      fail++;
   }
   if (!shuffle_identity(copy, 5, 3, 1015, 0, 0, 0, 0, 1))
   {
      printf("  Tomb Raider Legend's red copy refused\n");
      fail++;
   }
   if (shuffle_identity(moved, 1, 3, 1015, 0, 1, 0, 0, 1))
   {
      printf("  a block copied elsewhere taken for a channel shuffle\n");
      fail++;
   }
   if (shuffle_identity(shifted, 2, 3, 1015, 0, 0, 0, 0, 1))
   {
      printf("  Tomb Raider Legend's shifted alpha pass taken for an identity\n");
      fail++;
   }
   if (!shuffle_identity(shifted, 2, 3, 1015, 0, 0, 0, 0, 0))
   {
      printf("  Tomb Raider Legend's alpha pass refused as a page copy\n");
      fail++;
   }
   if (!shuffle_identity(real, 2, 0, 0, 0, 0, 0, 0, 1))
   {
      printf("  a 1:1 single-channel read refused\n");
      fail++;
   }
   if (!shuffle_identity(real, 3, 3, 1015, 0, 3, 1021, 0, 1))
   {
      printf("  a masked single-channel read refused\n");
      fail++;
   }
   if (shuffle_identity(unflipped, 1, 3, 1015, 0, 3, 1021, 0, 1))
   {
      printf("  a read of the flipped columns taken for an identity\n");
      fail++;
   }
   printf("channel shuffle gate: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

int main(void)
{
   int fail = 0;
   fail += check_swizzle();
   fail += check_scaled();
   fail += check_sample_map();
   fail += check_page_position();
   fail += check_alias_formula();
   fail += check_shuffle_gate();
   return fail ? 1 : 0;
}
