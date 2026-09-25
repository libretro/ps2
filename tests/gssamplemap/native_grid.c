/* The native pixel a scaled fragment belongs to, under a half-pixel offset.
 *
 * Point-sampled sprites drawn at scale pick their texel by native pixel
 * (the sample map, PS_SAMPLE_MAP): each fragment takes the coordinate at
 * the first fragment of its native pixel, where a native draw samples.
 * The native taps filter (PS_NATIVE_TAPS) groups fragments the same way.
 * The half-pixel offset that aligns a draw to native moves the geometry
 * half a native pixel on, so the draw's native pixels start half a native
 * pixel into the scaled grid. Grouped on the unshifted grid, the first
 * fragments of a sprite's first column belong to the native pixel before
 * it and read a texel outside the sprite; where two sprites meet, that
 * column is neither's, and a line runs through the picture.
 *
 * Pinned here, for a sprite from native x 512, one texel per pixel, at
 * 2x, 3x, 4x and 8x: with the grid started at the offset, every covered
 * fragment reads the texel of its own native pixel; the unshifted grid
 * reads outside the sprite at its first column; with no offset the two
 * are the same.
 *
 * The Normal offset moves a draw by the same half native pixel (SetupIA:
 * the scaled half-fragment offset times the scale), at every scale, with
 * nothing added for any one of them: pinned against the native-aligned
 * offset for scales 2 to 12 and target widths up to 2048.
 *
 * Build and run, from tests/gssamplemap:
 *   cc -O2 -std=c89 -pedantic -Wall native_grid.c -o native_grid -lm
 *   ./native_grid
 */
#include <math.h>
#include <stdio.h>

#define X0 512
#define X1 640

/* The texel a fragment reads: the coordinate at the first fragment of its
 * native pixel, the grid starting `grid` fragments in. u runs one texel
 * per native pixel from the sprite's left edge, which the offset `h`
 * (native pixels) has moved on. */
static int texel_of(int f, int s, double h, double grid)
{
   const double frag = f + 0.5;
   const double u = frag / s - h - X0;
   const double d = floor((frag - grid) / s) * s + grid + 0.5 - frag;
   return (int)floor(u + d / s);
}

static int check(int s, double h, int *outside_unshifted)
{
   int f, bad = 0;
   *outside_unshifted = 0;
   for (f = 0; f < X1 * s + s * 2; f++)
   {
      const double frag = f + 0.5;
      int want, got, old;
      /* covered by the sprite, as drawn with the offset */
      if (frag < (X0 + h) * s || frag >= (X1 + h) * s)
         continue;
      want = (int)floor(frag / s - h) - X0; /* its own native pixel */
      got = texel_of(f, s, h, h * s);
      old = texel_of(f, s, h, 0.0);
      if (got != want)
         bad++;
      if (old < 0 || old >= X1 - X0)
         (*outside_unshifted)++;
   }
   return bad;
}

/* SetupIA's clip-space offsets: the scaled one, -1 / (unscaled * scale),
 * times the scale under the Normal offset; the native-aligned one,
 * -1 / unscaled. Both are half a native pixel, in fragments scale / 2. */
static int check_normal_offset(void)
{
   int s, w, bad = 0;
   for (s = 2; s <= 12; s++)
      for (w = 64; w <= 2048; w += 64)
      {
         const float scaled = (-1.0f / (float)(w * s)) * (float)s;
         const float native = -1.0f / (float)w;
         const double frags = -(double)scaled * (w * s) / 2.0;
         if (fabs((double)scaled - (double)native) > 1e-6 * fabs((double)native) || fabs(frags - s / 2.0) > 1e-3)
         {
            if (bad++ < 4)
               printf("  scale %d width %d: Normal offset %.9g, native %.9g\n", s, w, (double)scaled, (double)native);
         }
      }
   return bad;
}

int main(void)
{
   static const int scales[] = { 2, 3, 4, 8 };
   unsigned i;
   int fail = 0;

   for (i = 0; i < sizeof(scales) / sizeof(scales[0]); i++)
   {
      const int s = scales[i];
      int outside, bad, f, same = 1;

      bad = check(s, 0.5, &outside);
      if (bad)
      {
         printf("  scale %d: %d fragments read another native pixel's texel\n", s, bad);
         fail++;
      }
      if (s % 2 == 0 && !outside)
      {
         printf("  scale %d: the unshifted grid should read outside the sprite\n", s);
         fail++;
      }

      for (f = X0 * s; f < X1 * s; f++)
         if (texel_of(f, s, 0.0, 0.0) != (int)floor((f + 0.5) / s) - X0)
            same = 0;
      if (!same || check(s, 0.0, &outside))
      {
         printf("  scale %d: with no offset the grid does not give the native texel\n", s);
         fail++;
      }
   }

   if (check_normal_offset())
      fail++;

   printf("native grid: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
