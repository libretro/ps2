/* gs_vertex against GSVertex: layout, every accessor, the store, then the
 * numbers.
 *
 * The record is the one the whole GS agrees on, so layout is checked first
 * and hardest -- a field moving here is not a compile error anywhere, it is
 * wrong geometry.
 */
/* GSVertex has both an x86 and an arm64 backend (GSVector4i_arm64.h), so
 * every lane here runs on both, and the NEON bodies are checked against
 * GSVertex's own NEON accessors rather than only against scalar. */
#define HAVE_GSVERTEX 1
#include "GS/Renderers/Common/GSVertex.h"

/* The reference the accessors are checked against.
 *
 * These are the GSVector forms GSVertex used before it delegated to
 * gs_vertex, kept here deliberately: once the header delegates, comparing
 * against GetVertex*() would compare gs_vertex with itself and prove
 * nothing. Written against GSVector4i/GSVector4, so on arm64 they are that
 * class's NEON backend and this stays a real two-implementation check. */
static __forceinline_odr GSVector4i RefXY(const GSVertex& v)
{ return GSVector4i(v.m[1]).upl16().xyxy(); }
static __forceinline_odr GSVector4i RefZ(const GSVertex& v)
{ return GSVector4i(v.m[1]).yyyy(); }
static __forceinline_odr GSVector4i RefUV(const GSVertex& v)
{ return GSVector4i(v.m[1]).uph16().xyxy(); }
static __forceinline_odr GSVector4 RefST(const GSVertex& v)
{ return GSVector4::cast(GSVector4i(v.m[0])).xyxy(); }
static __forceinline_odr GSVector4i RefRGBA(const GSVertex& v)
{ return GSVector4i(v.m[0]).uph8().upl16(); }
static __forceinline_odr GSVector4 RefQ(const GSVertex& v)
{ return GSVector4::cast(GSVector4i(v.m[0])).wwww(); }
static __forceinline_odr GSVector4i RefFOG(const GSVertex& v)
{ return GSVector4i(v.m[1]).wwww(); }
extern "C" {
#include "gs_vertex.h"
}
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <vector>
#include <ctime>
#include <algorithm>

/* ---------------- layout ---------------- */
static int layout(void)
{
	int f = 0;
#define PIN(cond, what) do { if (!(cond)) { printf("  LAYOUT %s\n", what); f++; } } while (0)
	PIN(sizeof(union gs_vertex) == 32, "size is not 32");
	PIN(alignof(union gs_vertex) == 32, "alignment is not 32");
#if defined(HAVE_GSVERTEX)
	PIN(sizeof(union gs_vertex) == sizeof(GSVertex), "size differs from GSVertex");
	PIN(offsetof(struct gs_vertex_fields, ST)    == offsetof(GSVertex, ST),    "ST offset");
	PIN(offsetof(struct gs_vertex_fields, RGBAQ) == offsetof(GSVertex, RGBAQ), "RGBAQ offset");
	PIN(offsetof(struct gs_vertex_fields, XYZ)   == offsetof(GSVertex, XYZ),   "XYZ offset");
	PIN(offsetof(struct gs_vertex_fields, U)     == offsetof(GSVertex, UV),    "UV offset");
	PIN(offsetof(struct gs_vertex_fields, FOG)   == offsetof(GSVertex, FOG),   "FOG offset");
#endif
	PIN(offsetof(struct gs_vertex_fields, ST) == 0 && offsetof(struct gs_vertex_fields, RGBAQ) == 8 &&
	    offsetof(struct gs_vertex_fields, XYZ) == 16 && offsetof(struct gs_vertex_fields, U) == 24 &&
	    offsetof(struct gs_vertex_fields, FOG) == 28, "absolute offsets moved");
#undef PIN
	printf("%s: layout, %d failures\n", f ? "FAIL" : "PASS", f);
	return f;
}

/* ---------------- accessors ---------------- */
static uint64_t X = 0x243F6A8885A308D3ull;
static uint32_t rnd(){ X ^= X<<13; X ^= X>>7; X ^= X<<17; return (uint32_t)(X>>16); }

#if !defined(HAVE_GSVERTEX)
static int accessors(void) { printf("SKIP: accessors, GSVertex is x86-only\n"); return 0; }
#else
static int accessors(void)
{
	long n = 0, f = 0;
	int k;
	for (k = 0; k < 2000000; k++) {
		GS_VERTEX_ALIGN32 uint8_t raw[32];
		uint32_t *w = (uint32_t *)raw;
		int j;
		for (j = 0; j < 8; j++) w[j] = rnd();
		/* seed the edges of every field */
		if ((k & 15) == 0) memset(raw, 0x00, 32);
		if ((k & 15) == 1) memset(raw, 0xff, 32);
		if ((k & 15) == 2) { w[4] = 0xffff0000u; w[6] = 0x0000ffffu; }

		const GSVertex *a = (const GSVertex *)raw;
		const union gs_vertex *b = (const union gs_vertex *)raw;
		GS_VERTEX_ALIGN32 uint8_t r1[16], r2[16];

#define CMP(expr_a, expr_b, what) do { \
		{ auto va_ = (expr_a); memcpy(r1, &va_, 16); } \
		{ auto vb_ = (expr_b); memcpy(r2, &vb_, 16); } \
		n++; \
		if (memcmp(r1, r2, 16)) { \
			if (f < 4) printf("  MISMATCH %s at %d\n", what, k); \
			f++; } } while (0)

		CMP(RefXY(*a),   gs_vertex_xy(b),   "XY");
		CMP(RefZ(*a),    gs_vertex_z(b),    "Z");
		CMP(RefUV(*a),   gs_vertex_uv(b),   "UV");
		CMP(RefRGBA(*a), gs_vertex_rgba(b), "RGBA");
		CMP(RefFOG(*a),  gs_vertex_fog(b),  "FOG");
		CMP(RefST(*a),   gs_vertex_st(b),   "ST");
		CMP(RefQ(*a),    gs_vertex_q(b),    "Q");
#undef CMP
	}
	printf("%s: accessors, %ld comparisons, %ld mismatches\n", f ? "FAIL" : "PASS", n, f);
	return f != 0;
}
#endif /* HAVE_GSVERTEX */

static gs_vec4i gs_vertex_make_half(int k)
{
	GS_VERTEX_ALIGN32 union gs_vertex t;
	int j;
	for (j = 4; j < 8; j++) t.w[j] = (uint32_t)(k * 2654435761u + j);
	return t.m[1];
}

/* ---------------- the per-vertex store ---------------- */
/* Its AVX form assembles the line from two halves rather than moving 32
 * bytes in one go, so "it obviously copies" is not a safe assumption. */
static int store_lane(void)
{
	long f = 0;
	int k;
	for (k = 0; k < 200000; k++) {
		GS_VERTEX_ALIGN32 union gs_vertex src, dst;
		int j;
		for (j = 0; j < 8; j++) src.w[j] = rnd();
		if ((k & 7) == 0) memset(&src, 0x00, 32);
		if ((k & 7) == 1) memset(&src, 0xff, 32);
		memset(&dst, 0xA5, 32);
		gs_vertex_store(&dst, &src);
		if (memcmp(&dst, &src, 32)) f++;
	}
	/* and with the source half written immediately before, which is how
	 * VertexKick reaches it -- the shape a wide load would mishandle. */
	{
		GS_VERTEX_ALIGN32 union gs_vertex src, dst;
		for (k = 0; k < 200000; k++) {
			int j;
			for (j = 0; j < 4; j++) src.w[j] = rnd();
			src.m[1] = gs_vertex_make_half(k);
			memset(&dst, 0x5A, 32);
			gs_vertex_store(&dst, &src);
			if (memcmp(&dst, &src, 32)) f++;
		}
	}
	printf("%s: store, %ld failures\n", f ? "FAIL" : "PASS", f);
	return f != 0;
}

/* ---------------- numbers ---------------- */
static std::vector<union gs_vertex> SRC, DST;
static double now(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+1e-9*t.tv_nsec; }

#if defined(HAVE_GSVERTEX)
static uint64_t b_store_old(int n){
	for (int i=0;i<n;i++){ GSVector4i *RESTRICT d=(GSVector4i*)&DST[i&4095]; const GSVertex&s=*(const GSVertex*)&SRC[i&4095];
		d[0]=GSVector4i(s.m[0]); d[1]=GSVector4i(s.m[1]); } return n; }
static uint64_t b_acc_old(int n){
	GSVector4i a=GSVector4i::zero(); GSVector4 c=GSVector4::zero();
	for(int i=0;i<n;i++){ const GSVertex&v=*(const GSVertex*)&SRC[i&4095];
		a=a+RefXY(v)+RefZ(v)+RefUV(v)+RefRGBA(v)+RefFOG(v);
		c=c+RefST(v)+RefQ(v);} return (uint64_t)(uint32_t)a.extract32<0>()^(uint64_t)(int)c.x; }
/* Same accumulate on both sides -- only the accessor differs. */
static uint64_t b_acc_new(int n){
	GSVector4i a=GSVector4i::zero(); GSVector4 c=GSVector4::zero();
	for(int i=0;i<n;i++){ const union gs_vertex*v=&SRC[i&4095];
		a=a+GSVector4i(gs_vertex_xy(v))+GSVector4i(gs_vertex_z(v))+GSVector4i(gs_vertex_uv(v))
		   +GSVector4i(gs_vertex_rgba(v))+GSVector4i(gs_vertex_fog(v));
		c=c+GSVector4(gs_vertex_st(v))+GSVector4(gs_vertex_q(v));}
	return (uint64_t)(uint32_t)a.extract32<0>()^(uint64_t)(int)c.x; }
#endif /* HAVE_GSVERTEX */
static uint64_t b_store_portable(int n){
	for (int i=0;i<n;i++) gs_vertex_store(&DST[i&4095],&SRC[i&4095]);
	return n; }

struct V { const char*n; uint64_t(*f)(int); std::vector<double> s; };

int main(int argc, char **argv)
{
	int n = argc>1?atoi(argv[1]):4000000, rounds = argc>2?atoi(argv[2]):15;
	int fails = 0;

	fails += layout();
	fails += accessors();
	fails += store_lane();

	printf("\n");

	SRC.resize(4096); DST.resize(4096);
	uint64_t s=0x9E3779B97F4A7C15ull;
	for (auto &v : SRC){ uint32_t *w=(uint32_t*)&v; for(int k=0;k<8;k++){ s=s*6364136223846793005ull+1442695040888963407ull; w[k]=(uint32_t)(s>>20);} }

	std::vector<V> vs;
#if defined(HAVE_GSVERTEX)
	vs.push_back({"store, 2 x 16 B (reference)", b_store_old, {}});
	vs.push_back({"store, gs_vertex",           b_store_portable, {}});
	vs.push_back({"7 accessors, reference",      b_acc_old, {}});
	vs.push_back({"7 accessors, gs_vertex",     b_acc_new, {}});
#else
	vs.push_back({"store, gs_vertex",           b_store_portable, {}});
#endif
	uint64_t sink=0;
	for(auto&v:vs) sink+=v.f(4096);
	for(int r=0;r<rounds;r++) for(auto&v:vs){ double a=now(); sink+=v.f(n); double b=now(); v.s.push_back((b-a)*1e9/n); }
	printf("%-30s %10s\n","op","ns/vertex");
	for(auto&v:vs){ std::sort(v.s.begin(),v.s.end()); printf("  %-28s %8.3f\n", v.n, v.s[v.s.size()/2]); }
	if(sink==0x1234567ull)printf("x\n");
	return fails != 0;
}
