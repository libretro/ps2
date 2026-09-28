/*
 * Magnified bilinear sprites at scale (gs_sprite_centre_linear,
 * GSSpriteSnap.c).
 *
 * Natively a pixel reads the filtered texture at its position, its top
 * left corner. Drawn at scale S, the pixel's fragments sit at
 * (j + 1/2) / S across it and read the texture there: their mean lands
 * half a pixel on from where the pixel reads, and a picture enlarged
 * through the filter - the levels of a glow drawn back over the frame at
 * twice, four and eight times their size - comes out half a pixel right
 * and down of the picture it glows around.
 *
 * With the shift, the fragments of each pixel centre on the pixel's own
 * position: their mean reads the texture where the native pixel does.
 */

#include <math.h>
#include <stdio.h>

#include "../../pcsx2/GS/Renderers/HW/GSSpriteSnap.c"

/* A sprite axis from pixel p0 to p1 (whole pixels), its coordinate u0 at
 * p0 and u1 at p1, in any unit; with the shift applied or not, the mean
 * over the fragments of pixel x at scale S of the coordinate they read,
 * less the coordinate the native pixel reads. */
static double error_at(double u0, double u1, int p0, int p1, int x, int S, int shifted)
{
   const double du = (u1 - u0) / (p1 - p0);
   const double shift = shifted ? gs_sprite_centre_linear((float)u0, (float)u1, p0 * 16, p1 * 16) : 0.0;
   double sum = 0.0;
   int j;

   for (j = 0; j < S; j++)
      sum += u0 + shift + du * (x + (j + 0.5) / S - p0);
   return sum / S - (u0 + du * (x - p0));
}

int main(void)
{
   /* Enlarged 2, 4 and 8 times, drawn left to right and right to left,
    * and at native size. */
   static const double steps[] = { 0.5, 0.25, 0.125, -0.5, 1.0 };
   static const int scales[] = { 2, 3, 4, 8 };
   int fail = 0;
   unsigned a, s;
   int x;

   for (a = 0; a < sizeof(steps) / sizeof(steps[0]); a++)
   {
      for (s = 0; s < sizeof(scales) / sizeof(scales[0]); s++)
      {
         for (x = 0; x < 640; x += 37)
         {
            const double e = error_at(0.5, 0.5 + steps[a] * 640.0, 0, 640, x, scales[s], 1);
            if (fabs(e) > 1e-3)
            {
               printf("  step %g, %dx, pixel %d: fragments read %g texels off the pixel\n",
                     steps[a], scales[s], x, e);
               fail++;
               break;
            }
         }
      }
   }

   /* A sprite covering no pixel is left alone. */
   if (gs_sprite_centre_linear(0.0f, 100.0f, 32, 32) != 0.0f)
   {
      printf("  an empty sprite was moved\n");
      fail++;
   }

   /* Negative control: unshifted, an enlargement to twice the size at 2x
    * reads a quarter texel - half a pixel - on. */
   if (fabs(error_at(0.5, 320.5, 0, 640, 100, 2, 0) - 0.25) > 1e-6)
   {
      printf("  negative control: the unshifted fragments are not half a pixel on\n");
      fail++;
   }

   printf("magnified sprites at scale centre on the native pixels: %s\n", fail ? "FAIL" : "ok");
   return fail != 0;
}
