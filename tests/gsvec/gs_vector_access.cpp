/* How a value is read and written around the operations.
 *
 * The operations themselves compile identically either way -- the codegen
 * lane records that. What can differ is the shape around them, so these are
 * the seven ways the GS actually gets values in and out of a vector, each
 * written both ways and counted.
 *
 * The one that moves is the last: several lanes read by name, scalar
 * arithmetic on each, written back. clang vectorises that through the union
 * and leaves the class's members scalar.
 */
#include "GS/GSVector.h"
extern "C" {
#include "GS/gs_vector.h"
}
/* 1. read a 2-int pair from memory into a vector (GSVector2i -> vector) */
extern "C" __m128i cpp_pair(const void*p){ return GSVector4i(*(const GSVector2i*)p); }
extern "C" __m128i c89_pair(const void*p){ return gs_v4i_loadl(p); }
/* 2. write two lanes back out as a pair */
extern "C" void cpp_wpair(__m128i v, void*o){ GSVector4i::storel(o, GSVector4i(v)); }
extern "C" void c89_wpair(__m128i v, void*o){ gs_v4i_storel(o, v); }
/* 3. build a vector from four runtime ints */
extern "C" __m128i cpp_four(int a,int b,int c,int d){ return GSVector4i(a,b,c,d); }
extern "C" __m128i c89_four(int a,int b,int c,int d){ return gs_v4i_set4(a,b,c,d); }
/* 4. splat one runtime int */
extern "C" __m128i cpp_splat(int a){ return GSVector4i(a); }
extern "C" __m128i c89_splat(int a){ return gs_v4i_set32(a); }
/* 5. read one lane and use it as a scalar */
extern "C" int cpp_lane(const void*p){ GSVector4i A=GSVector4i::load<true>(p); return A.z; }
extern "C" int c89_lane(const void*p){ union gs_v4i_view w; w.v=gs_v4i_load(p); return w.lane.z; }
/* 6. write one lane then use the whole vector */
extern "C" __m128i cpp_setl(const void*p,int x){ GSVector4i A=GSVector4i::load<true>(p); A.y=x; return A; }
extern "C" __m128i c89_setl(const void*p,int x){ union gs_v4i_view w; w.v=gs_v4i_load(p); w.lane.y=x; return w.v; }
/* 7. the full rect->scalar->rect round trip GSDirtyRect does */
extern "C" __m128i cpp_rt(const void*p,int dx,int sx){ GSVector4i A=GSVector4i::load<true>(p); GSVector4i R;
  R.left=(A.left*dx)/sx; R.top=(A.top*dx)/sx; R.right=(A.right*dx)/sx; R.bottom=(A.bottom*dx)/sx; return R; }
extern "C" __m128i c89_rt(const void*p,int dx,int sx){ union gs_v4i_view a,r; a.v=gs_v4i_load(p);
  r.rect.left=(a.rect.left*dx)/sx; r.rect.top=(a.rect.top*dx)/sx;
  r.rect.right=(a.rect.right*dx)/sx; r.rect.bottom=(a.rect.bottom*dx)/sx; return r.v; }
