/* paraLLEl-GS high-resolution scanout of field-rendered games.
 *
 * A game drawing one field per vsync (SMODE2.FFMD) has its field line span
 * two frame lines. The field-aware scanout reads one field at the frame's
 * height, so its vertical factor counts over the field: one step above
 * the horizontal factor is a square pixel, and the field's own vertical
 * samples (rendered at half-line steps) fill it one to an output row, as
 * many as the grid has. The 8x checkerboard and the ordered 16x grid have
 * four rows of samples: 4x over the field, the frame at twice its height.
 * The sparse 16x grid's vertical samples are not rows and stay at 2x.
 *
 * Pinned here, against a model of the rasterizer's sample layout
 * (gs_renderer.cpp compute_sample_points) and of vsync()'s factors:
 *  - the factors for every grid and requested scale, field-aware and
 *    progressive;
 *  - the layers sample_circuit.frag reads for a 2x-wide, 4x-over-the-field
 *    output pixel lie in that pixel's sample row and half (8x: the one
 *    sample there; ordered 16x: the two, averaged), and for the ordered
 *    grid at 4x both ways the one sample under it;
 *  - the two fields' offset, half a field line, in output rows.
 *
 * Build and run, from tests/pgs:
 *   cc -O2 -std=c89 -pedantic -Wall pgs_field_scanout.c -o pgs_field_scanout
 *   ./pgs_field_scanout
 */
#include <stdio.h>

/* compute_sample_points, in quarter pixels (the grid before rescaling). */
static void sample_point(unsigned i, unsigned rx, unsigned ry, unsigned *x, unsigned *y)
{
   if (ry - rx == 2)
   {
      static const unsigned sparse[4] = { 0, 2, 3, 1 };
      *y = i % 8;
      *x = (i / 8) * 4 + sparse[i % 4];
      return;
   }
   *y = (i & 1) + ((i >> 2) & 1) * 2;
   *x = ((i >> 1) & 1) + ((i >> 3) & 1) * 2;
   if (ry - rx == 1)
      *x = *x * 2 + (i % 2);
}

struct factors
{
   unsigned sx, sy;
};

/* vsync(): the scanout factors for a requested scale (log2), a grid and
 * whether the field-aware path applies. */
static struct factors scanout_factors(unsigned req, unsigned rx, unsigned ry, int field)
{
   struct factors f;
   f.sx = req > rx ? rx : req;
   f.sy = req > ry ? ry : req;
   if (f.sy > f.sx)
      f.sy = f.sx;
   if (field)
   {
      unsigned fy = f.sx + 1;
      if (fy > ry)
         fy = ry;
      if (fy > 2)
         fy = 2;
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
      unsigned rx, ry, req, field, sx, sy;
   } cases[] = {
      /* 4x ordered */
      { 1, 1, 1, 1, 1, 1 }, { 1, 1, 2, 1, 1, 1 }, { 1, 1, 1, 0, 1, 1 },
      /* 8x checkerboard */
      { 1, 2, 1, 1, 1, 2 }, { 1, 2, 2, 1, 1, 2 }, { 1, 2, 2, 0, 1, 1 },
      /* 16x sparse */
      { 1, 3, 1, 1, 1, 1 }, { 1, 3, 2, 1, 1, 1 }, { 1, 3, 2, 0, 1, 1 },
      /* 16x ordered */
      { 2, 2, 1, 1, 1, 2 }, { 2, 2, 2, 1, 2, 2 }, { 2, 2, 2, 0, 2, 2 }, { 2, 2, 1, 0, 1, 1 }
   };
   int fail = 0;
   size_t i;
   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      const struct factors f = scanout_factors(cases[i].req, cases[i].rx, cases[i].ry, (int)cases[i].field);
      if (f.sx != cases[i].sx || f.sy != cases[i].sy)
      {
         printf("  grid %u,%u scale %u field %u: %u,%u, wanted %u,%u\n", cases[i].rx, cases[i].ry, cases[i].req,
            cases[i].field, f.sx, f.sy, cases[i].sx, cases[i].sy);
         fail++;
      }
   }
   return fail;
}

/* sample_circuit.frag, 2x across and 4x over the field: the layers an
 * output pixel (gx, gy) reads. */
static unsigned field_layers(unsigned samples, unsigned gx, unsigned gy, unsigned *layers)
{
   if (samples == 8)
   {
      layers[0] = (gy & 1u) | (gx << 1u) | ((gy >> 1u) << 2u);
      return 1;
   }
   layers[0] = (gy & 1u) | ((gy >> 1u) << 2u) | (gx << 3u);
   layers[1] = layers[0] + 2u;
   return 2;
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
            const unsigned slice = (gy & 1u) | ((gx & 1u) << 1u) | ((gy >> 1u) << 2u) | ((gx >> 1u) << 3u);
            unsigned x, y;
            sample_point(slice, 2, 2, &x, &y);
            if (x != gx || y != gy)
            {
               printf("  ordered 4x: output %u,%u reads the sample at %u,%u\n", gx, gy, x, y);
               fail++;
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
   for (sy = 1; sy <= 2; sy++)
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
   fail += check_offset();
   printf("field scanout: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
