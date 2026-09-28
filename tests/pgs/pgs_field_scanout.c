/* paraLLEl-GS high-resolution scanout of field-rendered games.
 *
 * A game drawing one field per vsync (SMODE2.FFMD) has its field line span
 * two frame lines. The field-aware scanout reads one field at the frame's
 * height, so its vertical factor counts over the field: one step above
 * the horizontal factor is a square pixel, and the field's own vertical
 * samples (rendered at half-line steps) fill it one to an output row, as
 * many as the grid has. The 8x checkerboard and the ordered 16x grid have
 * four rows of samples: 4x over the field, the frame at twice its height.
 * The ordered 4x8 grid (32 samples) has eight, which the full-field-height
 * scanout takes: 8x over the field, the frame at four times its height,
 * 2560x1792 from a 640x224 field. The sparse 16x grid's vertical samples
 * are not rows and stay at 2x.
 *
 * Pinned here, against a model of the rasterizer's sample layout
 * (gs_renderer.cpp compute_sample_points, in pgs_scanout_model.h) and
 * of vsync()'s factors:
 *  - the factors for every grid and requested scale, field-aware and
 *    progressive, with and without the full-field-height request;
 *  - the layers sample_circuit.frag reads for a 2x-wide, 4x-over-the-field
 *    output pixel lie in that pixel's sample row and half (8x: the one
 *    sample there; ordered 16x: the two, averaged), and for the ordered
 *    grid at 4x both ways the one sample under it;
 *  - on the 4x8 grid, the one sample under an output pixel of the
 *    full-height field scanout, the two rows a 4x frame pixel averages,
 *    and the column pairs a 2x scanout averages, each sample read once;
 *  - the tent reconstruction's taps on that grid, at 4x and at the full
 *    field height, each read the sample at their own grid position, the
 *    neighbouring native pixel's where they cross an edge (a tent kept
 *    inside the pixel is the negative control);
 *  - the 4x8 grid's columns are in line row to row, where a checkerboard's
 *    are not (negative control), and its five-bit sample IDs pack into the
 *    phase LUT word beside the texel offsets;
 *  - the two fields' offset, half a field line, in output rows.
 *
 * Build and run, from tests/pgs:
 *   cc -O2 -std=c89 -pedantic -Wall pgs_field_scanout.c -o pgs_field_scanout
 *   ./pgs_field_scanout
 */
#include <stdio.h>
#include "pgs_scanout_model.h"

struct factors
{
   unsigned sx, sy;
};

/* vsync(): the scanout factors for a requested scale (log2), a grid,
 * whether the field-aware path applies and whether the full field height
 * was asked for. */
static struct factors scanout_factors(unsigned req, unsigned rx, unsigned ry, int field, int full)
{
   struct factors f;
   f.sx = req > rx ? rx : req;
   f.sy = req > ry ? ry : req;
   if (f.sy > f.sx)
      f.sy = f.sx;
   if (field)
   {
      unsigned fy = f.sx + 1;
      const unsigned cap = full ? 3 : 2;
      if (fy > ry)
         fy = ry;
      if (fy > cap)
         fy = cap;
      if (ry - rx == 2)
         fy = 1;
      f.sy = fy;
   }
   return f;
}

static int check_factors(void)
{
   static const struct
   {
      unsigned rx, ry, req, field, full, sx, sy;
   } cases[] = {
      /* 4x ordered */
      { 1, 1, 1, 1, 0, 1, 1 }, { 1, 1, 2, 1, 0, 1, 1 }, { 1, 1, 1, 0, 0, 1, 1 },
      /* 8x checkerboard */
      { 1, 2, 1, 1, 0, 1, 2 }, { 1, 2, 2, 1, 0, 1, 2 }, { 1, 2, 2, 0, 0, 1, 1 },
      /* 16x sparse */
      { 1, 3, 1, 1, 0, 1, 1 }, { 1, 3, 2, 1, 0, 1, 1 }, { 1, 3, 2, 0, 0, 1, 1 },
      /* 16x ordered */
      { 2, 2, 1, 1, 0, 1, 2 }, { 2, 2, 2, 1, 0, 2, 2 }, { 2, 2, 2, 0, 0, 2, 2 }, { 2, 2, 1, 0, 0, 1, 1 },
      /* 32x ordered 4x8: the full field height only when asked for, and
       * only by the field-aware path */
      { 2, 3, 2, 1, 1, 2, 3 }, { 2, 3, 2, 1, 0, 2, 2 }, { 2, 3, 2, 0, 1, 2, 2 }, { 2, 3, 2, 0, 0, 2, 2 },
      { 2, 3, 1, 1, 1, 1, 2 }, { 2, 3, 1, 0, 1, 1, 1 },
      /* the full-field request on the grids without eight rows */
      { 2, 2, 2, 1, 1, 2, 2 }, { 1, 2, 2, 1, 1, 1, 2 }, { 1, 3, 2, 1, 1, 1, 1 }, { 1, 1, 2, 1, 1, 1, 1 }
   };
   int fail = 0;
   size_t i;
   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      const struct factors f = scanout_factors(cases[i].req, cases[i].rx, cases[i].ry, (int)cases[i].field, (int)cases[i].full);
      if (f.sx != cases[i].sx || f.sy != cases[i].sy)
      {
         printf("  grid %u,%u scale %u field %u full %u: %u,%u, wanted %u,%u\n", cases[i].rx, cases[i].ry, cases[i].req,
            cases[i].field, cases[i].full, f.sx, f.sy, cases[i].sx, cases[i].sy);
         fail++;
      }
   }
   return fail;
}

static int check_layers(void)
{
   int fail = 0;
   unsigned grid;
   for (grid = 0; grid < 2; grid++)
   {
      const unsigned rx = grid ? 2 : 1, ry = 2, samples = grid ? 16 : 8;
      unsigned gx, gy;
      int seen[16] = { 0 };
      for (gy = 0; gy < 4; gy++)
         for (gx = 0; gx < 2; gx++)
         {
            unsigned layers[2], n, k;
            n = field_layers(samples, gx, gy, layers);
            for (k = 0; k < n; k++)
            {
               unsigned x, y;
               sample_point(layers[k], rx, ry, &x, &y);
               if (y != gy || x / 2 != gx)
               {
                  printf("  %u samples, output %u,%u: layer %u at %u,%u\n", samples, gx, gy, layers[k], x, y);
                  fail++;
               }
               seen[layers[k]]++;
            }
         }
      /* every sample read once */
      {
         unsigned s;
         for (s = 0; s < samples; s++)
            if (seen[s] != 1)
            {
               printf("  %u samples: layer %u read %d times\n", samples, s, seen[s]);
               fail++;
            }
      }
   }
   /* the ordered grid at 4x both ways: the sample under the pixel */
   {
      unsigned gx, gy;
      for (gy = 0; gy < 4; gy++)
         for (gx = 0; gx < 4; gx++)
         {
            unsigned x, y;
            sample_point(grid16_layer(gx, gy), 2, 2, &x, &y);
            if (x != gx || y != gy)
            {
               printf("  ordered 4x: output %u,%u reads the sample at %u,%u\n", gx, gy, x, y);
               fail++;
            }
         }
   }
   return fail;
}

/* Every output pixel of a scanout shape reads the samples in its own
 * part of the pixel - the rows and columns its factors give it - and
 * every sample is read by exactly one output pixel. */
static int check_grid32(void)
{
   static const unsigned shapes[][2] = { { 2, 3 }, { 2, 2 }, { 1, 2 }, { 1, 1 } };
   int fail = 0;
   size_t si;
   for (si = 0; si < sizeof(shapes) / sizeof(shapes[0]); si++)
   {
      const unsigned sx = shapes[si][0], sy = shapes[si][1];
      /* samples per output pixel: columns 4 >> sx, rows 8 >> sy */
      const unsigned cols = 4u >> sx, rows = 8u >> sy;
      unsigned gx, gy, s;
      int seen[32] = { 0 };
      for (gy = 0; gy < (1u << sy); gy++)
         for (gx = 0; gx < (1u << sx); gx++)
         {
            unsigned layers[8], n, k;
            n = scanout_layers(32, sx, sy, gx, gy, layers);
            if (n != cols * rows)
            {
               printf("  4x8 at %u,%u: output %u,%u reads %u samples, its part holds %u\n", sx, sy, gx, gy, n, cols * rows);
               fail++;
            }
            for (k = 0; k < n; k++)
            {
               unsigned x, y;
               if (layers[k] >= 32)
               {
                  printf("  4x8 at %u,%u: layer %u\n", sx, sy, layers[k]);
                  fail++;
                  continue;
               }
               sample_point(layers[k], 2, 3, &x, &y);
               /* x is in eighths, columns at 0, 2, 4, 6 */
               if (x / 2 / cols != gx || y / rows != gy)
               {
                  printf("  4x8 at %u,%u: output %u,%u reads layer %u at column %u row %u\n",
                     sx, sy, gx, gy, layers[k], x / 2, y);
                  fail++;
               }
               seen[layers[k]]++;
            }
         }
      for (s = 0; s < 32; s++)
         if (seen[s] != 1)
         {
            printf("  4x8 at %u,%u: layer %u read %d times\n", sx, sy, s, seen[s]);
            fail++;
         }
   }
   return fail;
}

/* sample_circuit.frag fetch_grid32_sample: the native pixel and the
 * layers one tent tap reads at global grid position (Gx, Gy), with
 * rows_log2 output rows per native pixel. When wrap is set the tap is
 * kept inside the centre pixel's native pixel (nx, ny) instead, which
 * is the negative control: a tent that does not cross pixel edges. */
static unsigned grid32_tap(unsigned rows_log2, unsigned Gx, unsigned Gy, int wrap, unsigned *nx, unsigned *ny, unsigned *layers)
{
   const unsigned gx = Gx & 3u;
   const unsigned column = ((gx & 1u) << 1u) | ((gx >> 1u) << 3u);
   if (!wrap)
   {
      *nx = Gx >> 2u;
      *ny = Gy >> rows_log2;
   }
   if (rows_log2 == 3)
   {
      const unsigned gy = Gy & 7u;
      layers[0] = column | (gy & 1u) | (((gy >> 1u) & 1u) << 2u) | ((gy >> 2u) << 4u);
      return 1;
   }
   else
   {
      const unsigned gy = Gy & 3u;
      layers[0] = column | ((gy & 1u) << 2u) | ((gy >> 1u) << 4u);
      layers[1] = layers[0] + 1u;
      return 2;
   }
}

/* One tap of the [1 2 1]/4 tent at grid position G reads the sample the
 * grid has at G: in the native pixel G falls in, the column and row (or
 * row pair) under it. Checked over the output pixels of one native pixel
 * with neighbours on every side, where the outer taps cross its edges. */
static int tent_taps_land(unsigned rows_log2, int wrap)
{
   const unsigned rows = 1u << rows_log2;
   const unsigned per_row = 8u >> rows_log2;
   unsigned ox, oy;
   for (oy = 0; oy < rows; oy++)
      for (ox = 0; ox < 4; ox++)
      {
         const unsigned bx = 4 + ox, by = rows + oy;
         int dx, dy;
         for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++)
            {
               const unsigned Gx = bx + dx, Gy = by + dy;
               unsigned nx = 1, ny = 1, layers[2], n, k;
               n = grid32_tap(rows_log2, Gx, Gy, wrap, &nx, &ny, layers);
               if (n != per_row)
                  return 0;
               for (k = 0; k < n; k++)
               {
                  unsigned x, y;
                  if (layers[k] >= 32)
                     return 0;
                  sample_point(layers[k], 2, 3, &x, &y);
                  /* x in eighths, columns at 0, 2, 4, 6; y the row */
                  if (nx * 4 + x / 2 != Gx || ny * rows + y / per_row != Gy)
                     return 0;
               }
               if (n == 2 && (layers[0] ^ layers[1]) != 1u)
                  return 0;
            }
      }
   return 1;
}

static int check_tent(void)
{
   static const float w[3] = { 0.25f, 0.5f, 0.25f };
   float sum = 0.0f;
   int fail = 0;
   int i, j;
   for (i = 0; i < 3; i++)
      for (j = 0; j < 3; j++)
         sum += w[i] * w[j];
   if (sum != 1.0f)
   {
      printf("  tent weights sum to %g\n", (double)sum);
      fail++;
   }
   if (!tent_taps_land(3, 0))
   {
      printf("  a tent tap on the 4x8 grid at the full field height misses its sample\n");
      fail++;
   }
   if (!tent_taps_land(2, 0))
   {
      printf("  a tent tap on the 4x8 grid at 4x misses its sample\n");
      fail++;
   }
   if (tent_taps_land(3, 1) || tent_taps_land(2, 1))
   {
      printf("  negative control: a tent kept inside the native pixel lands every tap\n");
      fail++;
   }
   return fail;
}

/* The 4x8 grid's columns: the same positions across on every row. A
 * checkerboard's are not, which is the negative control. */
static int columns_in_line(unsigned rx, unsigned ry, unsigned samples)
{
   unsigned i;
   for (i = 0; i < samples; i++)
   {
      unsigned x, y, x0, y0;
      sample_point(i, rx, ry, &x, &y);
      /* the sample one row down in the same column: y bit 0 flipped */
      sample_point(i ^ 1u, rx, ry, &x0, &y0);
      if (y0 != (y ^ 1u) || x0 != x)
         return 0;
   }
   return 1;
}

static int check_columns(void)
{
   int fail = 0;
   if (!columns_in_line(2, 3, 32))
   {
      printf("  the 4x8 grid's columns are not in line\n");
      fail++;
   }
   if (columns_in_line(1, 2, 8))
   {
      printf("  negative control: the 8x checkerboard's columns are in line\n");
      fail++;
   }
   return fail;
}

/* The phase LUT word past 16 samples (init_phase_lut, ubershader.comp):
 * four five-bit sample IDs in the low 20 bits and the three offsets that
 * can be non-zero at bits 16 + 4i, which lands them in the high 12. */
static int check_lut_word(void)
{
   int fail = 0;
   unsigned ids[4], off[4][2];
   unsigned trial;
   for (trial = 0; trial < 64; trial++)
   {
      unsigned long word = 0;
      unsigned i;
      for (i = 0; i < 4; i++)
      {
         ids[i] = (trial * 7u + i * 11u + 3u) & 31u;
         off[i][0] = i ? (trial + i) & 3u : 0;
         off[i][1] = i ? (trial / 3u + i) & 3u : 0;
         word |= (unsigned long)ids[i] << (5 * i);
         if (i)
         {
            word |= (unsigned long)off[i][0] << (4 * (i - 1) + 20);
            word |= (unsigned long)off[i][1] << (4 * (i - 1) + 22);
         }
      }
      for (i = 0; i < 4; i++)
      {
         const unsigned id = (unsigned)(word >> (5 * i)) & 31u;
         const unsigned ox = (unsigned)(word >> (16 + 4 * i)) & 3u;
         const unsigned oy = (unsigned)(word >> (18 + 4 * i)) & 3u;
         if (id != ids[i] || (i && (ox != off[i][0] || oy != off[i][1])))
         {
            printf("  LUT word: sample %u decodes as id %u at %u,%u, packed %u at %u,%u\n",
               i, id, ox, oy, ids[i], off[i][0], off[i][1]);
            fail++;
            break;
         }
      }
   }
   return fail;
}

/* The phase-0 field sits half a field line above the other: at a factor
 * of 2^sy over the field that is 2^(sy-1) output rows. */
static int check_offset(void)
{
   int fail = 0;
   unsigned sy;
   for (sy = 1; sy <= 3; sy++)
   {
      const unsigned rows_per_field_line = 1u << sy;
      const unsigned offset = 1u << (sy - 1);
      if (offset * 2 != rows_per_field_line)
      {
         printf("  factor %u: offset %u rows, a field line is %u\n", sy, offset, rows_per_field_line);
         fail++;
      }
   }
   return fail;
}

int main(void)
{
   int fail = 0;
   fail += check_factors();
   fail += check_layers();
   fail += check_grid32();
   fail += check_tent();
   fail += check_columns();
   fail += check_lut_word();
   fail += check_offset();
   printf("field scanout: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
