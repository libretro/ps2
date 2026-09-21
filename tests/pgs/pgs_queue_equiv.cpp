/* Queue-equivalence test: the ring-buffer vertex queue must deliver exactly
 * the same sequence of (position, attribute) triples to drawing_kick_append
 * as the current shift-based queue, for every primitive topology, across a
 * long randomized stream of kicks and maintenance calls. */
#include <cstdint>
typedef uint64_t VkDeviceAddress;
#include "muglm/muglm_impl.hpp"
#include "shaders/data_structures.h"
#include "gs_registers.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <smmintrin.h>
using namespace ParallelGS;
using namespace muglm;

struct Regs { Reg64<STBits> st; Reg64<RGBAQBits> rgbaq; Reg64<UVBits> uv; Reg64<FOGBits> fog; };
static Regs R; static int OFX, OFY; static __m128i OFXY;

static inline void build_attr(VertexAttribute &a, float fog)
{
	a.st.x = R.st.desc.S; a.st.y = R.st.desc.T;
	a.q = R.rgbaq.desc.Q; a.rgba = R.rgbaq.words[0];
	a.fog = fog;
	a.uv = u16vec2(R.uv.desc.U, R.uv.desc.V);
}

/* ---- current ---- */
struct QCur {
	VertexPosition pos[3]; VertexAttribute attr[3]; unsigned count = 0;
	void shift() { if (count==3){ pos[0]=pos[1];attr[0]=attr[1];pos[1]=pos[2];attr[1]=attr[2];count=2; } }
	void kick(Reg64<XYZFBits> x, bool use_f) {
		shift();
		auto &p = pos[count]; auto &a = attr[count];
		p.pos.x = int(x.desc.X) - OFX; p.pos.y = int(x.desc.Y) - OFY; p.z = x.desc.Z;
		build_attr(a, use_f ? float(x.desc.F) : float(R.fog.desc.FOG));
		count++;
	}
	const VertexPosition &rp(unsigned i) const { return pos[i]; }
	const VertexAttribute &ra(unsigned i) const { return attr[i]; }
	void m_fan()  { pos[1]=pos[2]; attr[1]=attr[2]; count=2; }
	void m_list() { count=0; }
};

/* ---- candidate: ring queue + SIMD position build ---- */
struct QRing {
	alignas(16) VertexPosition pos[4]; alignas(16) VertexAttribute attr[4];
	unsigned count = 0, head = 0;
	unsigned slot(unsigned i) const { return (head + i) & 3; }
	void advance() { if (count==3){ head=(head+1)&3; count=2; } }
	void kick(Reg64<XYZFBits> x, bool use_f) {
		advance();
		unsigned s = slot(count);
		__m128i xy = _mm_cvtepu16_epi32(_mm_cvtsi32_si128((int)x.words[0]));
		xy = _mm_sub_epi32(xy, OFXY);
		_mm_store_si128((__m128i *)&pos[s],
		                _mm_unpacklo_epi64(xy, _mm_cvtsi32_si128((int)x.desc.Z)));
		build_attr(attr[s], use_f ? float(x.desc.F) : float(R.fog.desc.FOG));
		count++;
	}
	const VertexPosition &rp(unsigned i) const { return pos[slot(i)]; }
	const VertexAttribute &ra(unsigned i) const { return attr[slot(i)]; }
	/* fan keeps the ORIGIN (oldest) and drops the middle -- head must not move */
	void m_fan()  { pos[slot(1)] = pos[slot(2)]; attr[slot(1)] = attr[slot(2)]; count = 2; }
	void m_list() { count = 0; head = 0; }
};

static uint64_t S = 0x243F6A8885A308D3ull;
static uint64_t rnd(){ S^=S<<13; S^=S>>7; S^=S<<17; return S; }

struct Out { uint8_t pos[3][12]; uint8_t attr[3][24]; };

template <typename Q>
static void drive(std::vector<Out> &out, int prim, int nverts, int steps, uint64_t seed)
{
	S = seed; Q q; out.clear();
	const bool fan  = (prim == 2);
	const bool list = (prim == 0);
	for (int i = 0; i < steps; i++) {
		R.st.bits = rnd(); R.rgbaq.bits = rnd(); R.uv.bits = rnd(); R.fog.bits = rnd();
		uint64_t v = rnd();
		bool use_f = (i & 1) != 0;
		q.kick(Reg64<XYZFBits>(v), use_f);
		if ((int)q.count >= nverts) {
			Out o{};
			for (int k = 0; k < nverts; k++) {
				unsigned idx = (nverts == 3) ? (2 - k) : (q.count - 1 - k);
				memcpy(o.pos[k], &q.rp(idx), 12);
				memcpy(o.attr[k], &q.ra(idx), 24);
			}
			out.push_back(o);
			if (fan) q.m_fan(); else if (list) q.m_list();
		}
	}
}

int main(void)
{
	const char *names[] = { "list (tri/sprite/line-list)", "strip", "fan" };
	int fails = 0;
	const int offs[] = { 0, 1024 << PGS_SUBPIXEL_BITS, -2048, 65535 };

	for (int oi = 0; oi < 4; oi++) {
		OFX = offs[oi]; OFY = offs[(oi + 1) & 3];
		OFXY = _mm_set_epi32(0, 0, OFY, OFX);
		for (int prim = 0; prim < 3; prim++) {
			for (int nv = 1; nv <= 3; nv++) {
				if (prim == 2 && nv != 3) continue;   /* fan is triangles only */
				for (uint64_t seed = 1; seed <= 40; seed++) {
					std::vector<Out> a, b;
					drive<QCur>(a, prim, nv, 5000, seed * 0x9E3779B97F4A7C15ull | 1);
					drive<QRing>(b, prim, nv, 5000, seed * 0x9E3779B97F4A7C15ull | 1);
					if (a.size() != b.size()) {
						printf("FAIL %s nv=%d: %zu vs %zu primitives\n",
						       names[prim], nv, a.size(), b.size());
						fails++; continue;
					}
					for (size_t i = 0; i < a.size(); i++) {
						if (memcmp(&a[i], &b[i], sizeof(Out)) != 0) {
							if (fails < 5)
								printf("FAIL %s nv=%d off=(%d,%d) seed=%llu prim #%zu\n",
								       names[prim], nv, OFX, OFY,
								       (unsigned long long)seed, i);
							fails++; break;
						}
					}
				}
			}
		}
	}
	printf("%s: queue equivalence, %d failures\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
