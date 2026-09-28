#include <math.h>

#include "GSSpriteSnap.h"

int gs_sprite_snap_edge(int v, int o)
{
   const int e = v - o;
   const int n = -((-e) & ~15);

   if (n + o > 0xffff)
      return ((0xffff - o) & ~15) + o;
   return n + o;
}

float gs_sprite_fit_nearest(float u_lo, float u_hi, int p_lo, int p_hi)
{
   /* Coordinates this close to a texel boundary are on it: the GS takes
    * them in fixed point, and a float carries them with a rounding. */
   const float eps = 1.0f / 1024.0f;
   float du, r, f;

   if (p_hi <= p_lo)
      return 0.0f;
   du = (u_hi - u_lo) * 16.0f / (float)(p_hi - p_lo);
   r  = (float)floor(du + 0.5f);
   if (r == 0.0f || fabs(du - r) > eps)
      return 0.0f;

   f = u_lo - (float)floor(u_lo);
   if (f > 1.0f - eps || f < eps)
      f = 0.0f;
   /* Stepping up, the boundary at the pixel's position is the one below
    * the texel it reads; stepping down, the one above it. */
   return r > 0.0f ? -f : 1.0f - f;
}

float gs_sprite_centre_linear(float u0, float u1, int p0, int p1)
{
   if (p1 == p0)
      return 0.0f;
   return -0.5f * (u1 - u0) * 16.0f / (float)(p1 - p0);
}
