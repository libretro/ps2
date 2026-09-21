/* C89 conformance check for the vertex kernels. Compiled as C89, pedantic.
 * Every kernel the header exposes is referenced here, so a construct that
 * is not C89 fails this lane rather than reaching a console toolchain. */
#include "pgs_vertex_kernels.h"

static struct pgs_kick_regs g_regs;
static int32_t g_a[2], g_b[2], g_c[2], g_lo[2], g_hi[2], g_sl[2], g_sh[2];
static uint16_t g_uv0[2], g_uv1[2];

int pgs_c89_smoke(unsigned long xy, unsigned long z, void *p, void *a)
{
   float fog;
   int r;

   fog = (float)g_regs.fog;
   pgs_build_position(&g_regs, (uint32_t)xy, (uint32_t)z, p);
   pgs_build_position_padded(&g_regs, (uint32_t)xy, (uint32_t)z, p);
   pgs_build_attribute(&g_regs, fog, a);

   pgs_pair_min_max3(g_a, g_b, g_c, 1, g_lo, g_hi);
   pgs_pair_min_max3(g_a, g_b, g_c, 0, g_lo, g_hi);
   pgs_pair_clamp(g_sl, g_sh, g_lo, g_hi);

   r  = pgs_ivec2_eq(g_a, g_b);
   r += pgs_u16vec2_eq(g_uv0, g_uv1);
   return r;
}
