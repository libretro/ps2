/* Instruction counts per operation, GSVector4i beside gs_vector.
 *
 * Compiled to assembly rather than run: the harness counts the instructions
 * in each pair and reports any that differ. Values in and value out, so
 * neither side can fold a load or a store the other cannot -- measured the
 * other way round, the two sides fold differently and the difference looks
 * like a win or a loss that is not in the operation at all.
 */
#include "GS/GSVector.h"
extern "C" {
#include "GS/gs_vector.h"
}
/* Values in, value out: no load or store for either side to fold, so the
 * comparison is the operation and nothing else. */
#define P(n, cppe, ce) \
extern "C" __m128i cpp_##n(__m128i x,__m128i y){ GSVector4i A(x),B(y); (void)B; return (cppe); } \
extern "C" __m128i c89_##n(__m128i x,__m128i y){ gs_vec4i a=x,b=y; (void)b; return (ce); }
P(add32,   A+B,                gs_v4i_add32(a,b))
P(sub32,   A-B,                gs_v4i_sub32(a,b))
P(and_,    A&B,                gs_v4i_and(a,b))
P(min_i32, A.min_i32(B),       gs_v4i_min_i32(a,b))
P(max_i32, A.max_i32(B),       gs_v4i_max_i32(a,b))
P(min_u32, A.min_u32(B),       gs_v4i_min_u32(a,b))
P(max_u32, A.max_u32(B),       gs_v4i_max_u32(a,b))
P(upl16,   A.upl16(),          gs_v4i_upl16(a))
P(uph16,   A.uph16(),          gs_v4i_uph16(a))
P(upl16v,  A.upl16(B),         gs_v4i_upl16v(a,b))
P(upl32,   A.upl32(),          gs_v4i_upl32(a))
P(ps32,    A.ps32(B),          gs_v4i_ps32(a,b))
P(pu32,    A.pu32(B),          gs_v4i_pu32(a,b))
P(blend8,  A.blend8(B,B),      gs_v4i_blend8(a,b,b))
P(blend32, A.blend32<12>(B),   GS_V4I_BLEND32(a,b,12))
P(sll32,   A.sll32<7>(),       GS_V4I_SLL32(a,7))
P(srl32,   A.srl32<11>(),      GS_V4I_SRL32(a,11))
P(sra32,   A.sra32<4>(),       GS_V4I_SRA32(a,4))
P(srl16,   A.srl16<5>(),       GS_V4I_SRL16(a,5))
P(srl_,    A.srl<8>(),         GS_V4I_SRL(a,8))
P(xyxy,    A.xyxy(),           GS_V4I_SHUFFLE32(a,0x44))
P(rinter,  A.rintersect(B),    gs_v4i_rintersect(a,b))
P(runion_, A.runion(B),        gs_v4i_runion(a,b))
P(insert32,A.insert32<1>(9),   GS_V4I_INSERT32(a,9,1))
P(insert8, A.insert8<5>(9),    GS_V4I_INSERT8(a,9,5))
P(zero,    GSVector4i::zero(), gs_v4i_zero())
P(ones,    GSVector4i::xffffffff(), gs_v4i_ones())
