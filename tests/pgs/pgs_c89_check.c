/* C89 conformance check for the vertex kernels. Compiled as C89, pedantic. */
#include "pgs_vertex_kernels.h"

static struct pgs_kick_regs g_regs;

void pgs_c89_smoke(unsigned long xy, unsigned long z, void *p, void *a)
{
   float fog;

   fog = (float)g_regs.fog;
   pgs_build_position(&g_regs, (uint32_t)xy, (uint32_t)z, p);
   pgs_build_position_padded(&g_regs, (uint32_t)xy, (uint32_t)z, p);
   pgs_build_attribute(&g_regs, fog, a);
}
