/*
 * Nearest-sampled sprites at scale (gs_sprite_fit_nearest, GSSpriteSnap.c).
 *
 * Natively a pixel of a sprite sampled nearest reads the texel under its
 * position, its top left corner. At scale S the pixel's fragments sit
 * across it, at (j + 1/2) / S. A strip copied one pixel along, at texel
 * centres - the picture read at x + 1/2, which the GS reads as texel x -
 * had the right half of each pixel's fragments reading texel x + 1: the
 * copy came out half a pixel along, and at the strip's last column its
 * fragments read past the strip. A frame blurred by such strips, 64
 * pixels wide, showed a line at each strip edge and the whole picture a
 * pixel off.
 *
 * Fitted, the fragments of each pixel read inside the texels the native
 * pixel covers: from a texture at native size, the texel its position
 * reads; from one drawn at the same scale, that texel's own fragment,
 * as a copy between two scaled targets should.
 */

#include <math.h>
#include <stdio.h>

#include "../../pcsx2/GS/Renderers/HW/GSSpriteSnap.c"

/* An axis of a sprite over pixels lo .. hi - 1, its coordinate u0 at lo
 * and stepping du texels a pixel. */
struct axis
{
   double u0, du;
   int lo, hi;
};

/* The coordinate at position P after adding delta. */
static double coord(const struct axis *a, double delta, double P)
{
   return a->u0 + delta + a->du * (P - a->lo);
}

/* Checks each fragment at scale S against the native pixel it lies in:
 * read from a texture at native size it must read inside the texels the
 * native pixel's footprint covers, starting from the one it reads; read
 * from one at scale S, the texel of that footprint at its own place. */
static int check_axis(const struct axis *a, int S, int fitted, const char *what)
{
   const double delta = fitted ? gs_sprite_fit_nearest((float)a->u0,
         (float)(a->u0 + a->du * (a->hi - a->lo)), a->lo * 16, a->hi * 16) : 0.0;
   const int r = (int)floor(fabs(a->du) + 0.5);
   int k, j;

   for (k = a->lo; k < a->hi; k++)
   {
      /* The GS takes the coordinate in fixed point: a float a hair under
       * a texel boundary is on it. */
      const int native = (int)floor(coord(a, 0.0, k) + 1e-5);

      for (j = 0; j < S; j++)
      {
         const double P  = k + (j + 0.5) / S;
         const double u  = coord(a, delta, P);
         const int    tn = (int)floor(u);
         const int    ts = (int)floor(u * S);
         /* The native footprint: r texels from the one read, up or down. */
         const int f_lo = a->du > 0 ? native : native - r + 1;
         const int f_hi = a->du > 0 ? native + r - 1 : native;
         /* The scaled texel at the fragment's own place in it. */
         const int want = a->du > 0 ? native * S + (j * r) :
                                      native * S + (S - 1) - (j * r);

         if (tn < f_lo || tn > f_hi || (r == 1 && ts != want))
         {
            if (fitted)
               printf("  %s, %dx: pixel %d fragment %d reads %d (scaled %d), natively %d\n",
                     what, S, k, j, tn, ts, native);
            return 1;
         }
      }
   }
   return 0;
}

int main(void)
{
   /* A strip copied at texel centres, the vertical blur's one row down,
    * texel edges, stepping down, two texels a pixel, and a coordinate a
    * float carries a hair under a texel boundary. */
   static const struct axis cases[] = {
      { 0.5,     1.0,  0, 64 },
      { 0.5,     1.0,  1, 448 },
      { 64.0,    1.0,  0, 64 },
      { 100.5,  -1.0,  0, 64 },
      { 0.5,     2.0,  0, 128 },
      { 2.9999999, 1.0, 0, 32 }
   };
   static const char *names[] = {
      "strip copy", "row blur", "texel edges", "stepping down", "halving", "float edge"
   };
   static const int scales[] = { 2, 3, 4, 8 };
   int fail = 0;
   unsigned c, s;

   for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
      for (s = 0; s < sizeof(scales) / sizeof(scales[0]); s++)
         fail += check_axis(&cases[c], scales[s], 1, names[c]);

   /* A step of one and a half texels has no fit: left as it is. */
   if (gs_sprite_fit_nearest(0.5f, 0.5f + 1.5f * 64.0f, 0, 64 * 16) != 0.0f)
   {
      printf("  a step of 1.5 texels was moved\n");
      fail++;
   }

   /* Negative control: unfitted, the strip copy reads the next texel. */
   if (!check_axis(&cases[0], 2, 0, names[0]))
   {
      printf("  negative control: the unfitted strip copy reads its own texels\n");
      fail++;
   }

   printf("nearest sprites at scale read the native texels: %s\n", fail ? "FAIL" : "ok");
   return fail != 0;
}
