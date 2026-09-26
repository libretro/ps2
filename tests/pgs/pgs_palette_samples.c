/* paraLLEl-GS: a super-sampled palette texture is read sample for sample.
 *
 * With super-sampled textures, a sprite reading a texture drawn at the
 * super-sampling rate gets it as an array, one layer per sample, and
 * triangle_setup picks how its pixels read the layers: the sample under
 * them, or a resampling that blends the texels around their coordinates
 * (blur kernels, up- and downsampling). A palette texture's texels are
 * indices, bytes of memory: in the 8-bit view of a page drawn at 32 bits,
 * each texel is one channel of one pixel, and the texel beside it is
 * another channel or another pixel. Blending them mixes bytes that do not
 * belong together -- a colour grade that reads the red, green and blue
 * bytes of each pixel through a palette came out with its channels offset
 * and tinted. So every palette read of an arrayed texture is forced to
 * sample mapping (TEX_INFO_FORCE_SAMPLE_MAPPING): sample k of the texel is
 * sample k of that byte.
 *
 * Pinned here: in the PSMT8 view of a PSMCT32 page (the mapping
 * ps_convert_rgba_8i and the HW channel shuffle gate use), no texel's
 * horizontal or vertical neighbour is the same channel of the same pixel;
 * and the flag rule forces mapping for every palette format, where the rule
 * it replaces left reads without a region-repeat clamp to the resampler.
 *
 * Build and run, from tests/pgs:
 *   cc -O2 -std=c89 -pedantic -Wall pgs_palette_samples.c -o pgs_palette_samples
 *   ./pgs_palette_samples
 */
#include <stdio.h>

/* The pixel and channel a PSMT8 texel of a PSMCT32 page holds. */
static void t8_byte(int u, int v, int* x, int* y, int* channel)
{
   const int flip = (((v >> 1) ^ (v >> 2)) & 1) << 2;
   *x = ((u & 7) ^ flip) | ((u >> 4) << 3);
   *y = ((v >> 2) << 1) | (v & 1);
   /* rows 0-1 of four hold R and B, rows 2-3 G and A; the upper eight
    * columns of sixteen take the second of the pair */
   *channel = (((v >> 1) & 1) ? 1 : 0) + (((u >> 3) & 1) ? 2 : 0);
}

static int same_byte(int u0, int v0, int u1, int v1)
{
   int x0, y0, c0, x1, y1, c1;
   t8_byte(u0, v0, &x0, &y0, &c0);
   t8_byte(u1, v1, &x1, &y1, &c1);
   return x0 == x1 && y0 == y1 && c0 == c1;
}

static int check_neighbours(void)
{
   int u, v, fail = 0, covered = 0;
   static int seen[32][64][4];
   for (v = 0; v < 64; v++)
      for (u = 0; u < 128; u++)
      {
         int x, y, c;
         t8_byte(u, v, &x, &y, &c);
         if (x < 64 && y < 32 && !seen[y][x][c]++)
            covered++;
         if (u + 1 < 128 && same_byte(u, v, u + 1, v))
            fail++;
         if (v + 1 < 64 && same_byte(u, v, u, v + 1))
            fail++;
      }
   /* the view is the page: every byte once */
   if (covered != 64 * 32 * 4)
   {
      printf("  the 8-bit view holds %d of the page's %d bytes\n", covered, 64 * 32 * 4);
      return 1;
   }
   if (fail)
      printf("  %d neighbouring texels are the same byte\n", fail);
   return fail;
}

/* Palette formats, as is_palette_format: PSMT8, T4, T8H, T4HL, T4HH. */
static int is_palette(unsigned psm)
{
   return psm == 0x13 || psm == 0x14 || psm == 0x1b || psm == 0x24 || psm == 0x2c;
}

static int forced(int arrayed, unsigned psm, int region_repeat)
{
   (void)region_repeat;
   return arrayed && is_palette(psm);
}

static int forced_old(int arrayed, unsigned psm, int region_repeat)
{
   return arrayed && is_palette(psm) && region_repeat;
}

static int check_rule(void)
{
   int fail = 0;
   if (!forced(1, 0x13, 0) || !forced(1, 0x13, 1) || !forced(1, 0x14, 0))
   {
      printf("  a palette read of an arrayed texture is not forced to mapping\n");
      fail++;
   }
   if (forced(1, 0x00, 0) || forced(0, 0x13, 0))
   {
      printf("  a colour read, or a single-sampled one, was forced\n");
      fail++;
   }
   if (forced_old(1, 0x13, 0))
   {
      printf("  negative: the region-repeat rule forced a plain palette read\n");
      fail++;
   }
   return fail;
}

int main(void)
{
   int fail = 0;
   fail += check_neighbours();
   fail += check_rule();
   printf("palette samples: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
