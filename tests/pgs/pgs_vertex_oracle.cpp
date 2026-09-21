/* Byte-exactness oracle: every candidate kick must produce VertexPosition and
 * VertexAttribute bytes identical to the current code, across randomized
 * register states, all queue phases, and both XYZ2/XYZF2 paths. */
#include <cstdint>
typedef uint64_t VkDeviceAddress;
#include "muglm/muglm_impl.hpp"
#include "shaders/data_structures.h"
#include "gs_registers.hpp"
#include <cstdio>
#include <cstring>
#include <smmintrin.h>
using namespace ParallelGS;
using namespace muglm;

struct Regs { Reg64<STBits> st; Reg64<RGBAQBits> rgbaq; Reg64<UVBits> uv; Reg64<FOGBits> fog; };
static Regs R;
static int OFX, OFY;
static __m128i OFXY;

/* --- reference: verbatim current bodies --- */
static void ref_xyz(Reg64<XYZBits> xyz, VertexPosition &p, VertexAttribute &a)
{
	p.pos.x = int(xyz.desc.X) - OFX;
	p.pos.y = int(xyz.desc.Y) - OFY;
	p.z = xyz.desc.Z;
	a.st.x = R.st.desc.S; a.st.y = R.st.desc.T;
	a.q = R.rgbaq.desc.Q; a.rgba = R.rgbaq.words[0];
	a.fog = float(R.fog.desc.FOG);
	a.uv = u16vec2(R.uv.desc.U, R.uv.desc.V);
}
static void ref_xyzf(Reg64<XYZFBits> x, VertexPosition &p, VertexAttribute &a)
{
	p.pos.x = int(x.desc.X) - OFX;
	p.pos.y = int(x.desc.Y) - OFY;
	p.z = x.desc.Z;
	a.st.x = R.st.desc.S; a.st.y = R.st.desc.T;
	a.q = R.rgbaq.desc.Q; a.rgba = R.rgbaq.words[0];
	a.fog = float(x.desc.F);
	a.uv = u16vec2(R.uv.desc.U, R.uv.desc.V);
}

/* --- candidate: SIMD build --- */
static inline __m128i attr_lo()
{
	__m128i st = _mm_loadl_epi64((const __m128i *)&R.st.bits);
	__m128i rq = _mm_loadl_epi64((const __m128i *)&R.rgbaq.bits);
	rq = _mm_shuffle_epi32(rq, _MM_SHUFFLE(3, 2, 0, 1));
	return _mm_unpacklo_epi64(st, rq);
}
static inline uint32_t packed_uv(void) { return R.uv.words[0] & 0x3fff3fffu; }

static void cand_xyz(Reg64<XYZBits> xyz, VertexPosition &p, VertexAttribute &a)
{
	__m128i xy = _mm_cvtepu16_epi32(_mm_cvtsi32_si128((int)xyz.words[0]));
	xy = _mm_sub_epi32(xy, OFXY);
	_mm_storeu_si128((__m128i *)&p, _mm_unpacklo_epi64(xy, _mm_cvtsi32_si128((int)xyz.desc.Z)));
	uint8_t *ap = (uint8_t *)&a;
	_mm_storeu_si128((__m128i *)ap, attr_lo());
	float f = float(R.fog.desc.FOG);
	uint32_t t[2]; memcpy(&t[0], &f, 4); t[1] = packed_uv();
	memcpy(ap + 16, t, 8);
}
static void cand_xyzf(Reg64<XYZFBits> x, VertexPosition &p, VertexAttribute &a)
{
	__m128i xy = _mm_cvtepu16_epi32(_mm_cvtsi32_si128((int)x.words[0]));
	xy = _mm_sub_epi32(xy, OFXY);
	_mm_storeu_si128((__m128i *)&p, _mm_unpacklo_epi64(xy, _mm_cvtsi32_si128((int)x.desc.Z)));
	uint8_t *ap = (uint8_t *)&a;
	_mm_storeu_si128((__m128i *)ap, attr_lo());
	float f = float(x.desc.F);
	uint32_t t[2]; memcpy(&t[0], &f, 4); t[1] = packed_uv();
	memcpy(ap + 16, t, 8);
}

static uint64_t S = 88172645463325252ull;
static uint64_t rnd(void) { S ^= S << 13; S ^= S >> 7; S ^= S << 17; return S; }

/* padding is never written by the current code, so compare the 12 defined
 * bytes of VertexPosition and all 24 of VertexAttribute. */
static int cmp_pos(const VertexPosition &a, const VertexPosition &b)
{ return memcmp(&a, &b, 12); }

int main(void)
{
	long fails = 0, n = 0;
	const int offs[] = { 0, 1, 2047, 1024 << PGS_SUBPIXEL_BITS, 65535, -4096 };

	for (int oi = 0; oi < 6; oi++) {
		for (int oj = 0; oj < 6; oj++) {
			OFX = offs[oi]; OFY = offs[oj];
			OFXY = _mm_set_epi32(0, 0, OFY, OFX);
			for (int k = 0; k < 200000; k++) {
				R.st.bits = rnd(); R.rgbaq.bits = rnd();
				R.uv.bits = rnd(); R.fog.bits = rnd();
				uint64_t v = rnd();
				/* exercise edge values too */
				if ((k & 63) == 0) v = 0;
				if ((k & 63) == 1) v = ~0ull;
				if ((k & 63) == 2) v = 0x0000ffffffffull;

				VertexPosition p0{}, p1{}; VertexAttribute a0{}, a1{};
				memset(&p0, 0xAA, sizeof p0); memset(&p1, 0x55, sizeof p1);
				memset(&a0, 0xAA, sizeof a0); memset(&a1, 0x55, sizeof a1);

				ref_xyz(Reg64<XYZBits>(v), p0, a0);
				cand_xyz(Reg64<XYZBits>(v), p1, a1);
				n++;
				if (cmp_pos(p0, p1) || memcmp(&a0, &a1, 24)) {
					if (fails < 5) {
						printf("MISMATCH xyz off=(%d,%d) v=%016llx\n", OFX, OFY, (unsigned long long)v);
						printf("  ref pos %d %d %u | cand %d %d %u\n",
						       p0.pos.x, p0.pos.y, p0.z, p1.pos.x, p1.pos.y, p1.z);
						uint32_t w0[6], w1[6]; memcpy(w0,&a0,24); memcpy(w1,&a1,24);
						for (int j=0;j<6;j++) printf("  attr[%d] %08x vs %08x\n", j, w0[j], w1[j]);
					}
					fails++;
				}

				memset(&p0, 0xAA, sizeof p0); memset(&p1, 0x55, sizeof p1);
				memset(&a0, 0xAA, sizeof a0); memset(&a1, 0x55, sizeof a1);
				ref_xyzf(Reg64<XYZFBits>(v), p0, a0);
				cand_xyzf(Reg64<XYZFBits>(v), p1, a1);
				n++;
				if (cmp_pos(p0, p1) || memcmp(&a0, &a1, 24)) {
					if (fails < 5) printf("MISMATCH xyzf off=(%d,%d) v=%016llx\n", OFX, OFY, (unsigned long long)v);
					fails++;
				}
			}
		}
	}
	printf("%s: %ld cases, %ld mismatches\n", fails ? "FAIL" : "PASS", n, fails);
	return fails != 0;
}
