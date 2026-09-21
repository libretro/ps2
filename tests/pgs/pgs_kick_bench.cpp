/* paraLLEl-GS vertex-kick micro-benchmark.
 *
 * Replicates GSInterface's vertex queue and kick bodies verbatim (V0) and
 * compares candidate rewrites. Struct layouts and register bit layouts come
 * from the real headers, so the memory traffic is the real traffic.
 */
#include <cstdint>
typedef uint64_t VkDeviceAddress;
#include "muglm/muglm_impl.hpp"
#include "shaders/data_structures.h"
#include "gs_registers.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <ctime>
#include <algorithm>
#include <smmintrin.h>

using namespace ParallelGS;
using namespace muglm;

static const int PGS_SUB = PGS_SUBPIXEL_BITS;

/* The register state a kick reads, in the same shape GSInterface holds it. */
struct Regs {
	Reg64<STBits>    st;
	Reg64<RGBAQBits> rgbaq;
	Reg64<UVBits>    uv;
	Reg64<FOGBits>   fog;
};
static Regs g_regs;
static int  g_ofx, g_ofy;
static __m128i g_ofxy;   /* {ofx, ofy, 0, 0}, rebuilt only on XYOFFSET write */

/* ---------------- V0: current code, copied verbatim ---------------- */
struct QueueV0 {
	enum { MaxEntries = 3 };
	alignas(16) VertexPosition pos[MaxEntries];
	alignas(16) VertexAttribute attr[MaxEntries];
	unsigned count = 0;

	inline void shift()
	{
		if (count == 3) {
			pos[0] = pos[1];  attr[0] = attr[1];
			pos[1] = pos[2];  attr[1] = attr[2];
			count = 2;
		}
	}
	inline void kick_xyz(Reg64<XYZBits> xyz)
	{
		shift();
		auto &pos_ = pos[count];
		auto &attr_ = attr[count];
		pos_.pos.x = int(xyz.desc.X) - g_ofx;
		pos_.pos.y = int(xyz.desc.Y) - g_ofy;
		pos_.z = xyz.desc.Z;
		attr_.st.x = g_regs.st.desc.S;
		attr_.st.y = g_regs.st.desc.T;
		attr_.q = g_regs.rgbaq.desc.Q;
		attr_.rgba = g_regs.rgbaq.words[0];
		attr_.fog = float(g_regs.fog.desc.FOG);
		attr_.uv = u16vec2(g_regs.uv.desc.U, g_regs.uv.desc.V);
		count++;
	}
	inline void kick_xyzf(Reg64<XYZFBits> xyzf)
	{
		shift();
		auto &pos_ = pos[count];
		auto &attr_ = attr[count];
		pos_.pos.x = int(xyzf.desc.X) - g_ofx;
		pos_.pos.y = int(xyzf.desc.Y) - g_ofy;
		pos_.z = xyzf.desc.Z;
		attr_.st.x = g_regs.st.desc.S;
		attr_.st.y = g_regs.st.desc.T;
		attr_.q = g_regs.rgbaq.desc.Q;
		attr_.rgba = g_regs.rgbaq.words[0];
		attr_.fog = float(xyzf.desc.F);
		attr_.uv = u16vec2(g_regs.uv.desc.U, g_regs.uv.desc.V);
		count++;
	}
	/* read sites, matching drawing_kick_append */
	inline const VertexPosition &rpos(unsigned i) const { return pos[i]; }
	inline const VertexAttribute &rattr(unsigned i) const { return attr[i]; }
	inline void maintain_fan()  { pos[1] = pos[2]; attr[1] = attr[2]; count = 2; }
	inline void maintain_list() { count = 0; }
};

/* ---------------- V1: ring queue, no shift ---------------- */
struct QueueV1 {
	alignas(16) VertexPosition pos[4];
	alignas(16) VertexAttribute attr[4];
	unsigned count = 0;   /* how many valid, capped at 3 */
	unsigned head = 0;    /* index of oldest valid entry */

	inline unsigned slot(unsigned i) const { return (head + i) & 3; }
	inline void advance()
	{
		if (count == 3) { head = (head + 1) & 3; count = 2; }
	}
	inline void kick_xyz(Reg64<XYZBits> xyz)
	{
		advance();
		auto &pos_ = pos[slot(count)];
		auto &attr_ = attr[slot(count)];
		pos_.pos.x = int(xyz.desc.X) - g_ofx;
		pos_.pos.y = int(xyz.desc.Y) - g_ofy;
		pos_.z = xyz.desc.Z;
		attr_.st.x = g_regs.st.desc.S;
		attr_.st.y = g_regs.st.desc.T;
		attr_.q = g_regs.rgbaq.desc.Q;
		attr_.rgba = g_regs.rgbaq.words[0];
		attr_.fog = float(g_regs.fog.desc.FOG);
		attr_.uv = u16vec2(g_regs.uv.desc.U, g_regs.uv.desc.V);
		count++;
	}
	inline void kick_xyzf(Reg64<XYZFBits> xyzf)
	{
		advance();
		auto &pos_ = pos[slot(count)];
		auto &attr_ = attr[slot(count)];
		pos_.pos.x = int(xyzf.desc.X) - g_ofx;
		pos_.pos.y = int(xyzf.desc.Y) - g_ofy;
		pos_.z = xyzf.desc.Z;
		attr_.st.x = g_regs.st.desc.S;
		attr_.st.y = g_regs.st.desc.T;
		attr_.q = g_regs.rgbaq.desc.Q;
		attr_.rgba = g_regs.rgbaq.words[0];
		attr_.fog = float(xyzf.desc.F);
		attr_.uv = u16vec2(g_regs.uv.desc.U, g_regs.uv.desc.V);
		count++;
	}
	inline const VertexPosition &rpos(unsigned i) const { return pos[slot(i)]; }
	inline const VertexAttribute &rattr(unsigned i) const { return attr[slot(i)]; }
	inline void maintain_fan()  { head = (head + 1) & 3; count = 2; }
	inline void maintain_list() { count = 0; head = 0; }
};

/* ---------------- V2: ring queue + SIMD build ---------------- */
struct QueueV2 {
	alignas(16) VertexPosition pos[4];
	alignas(16) VertexAttribute attr[4];
	unsigned count = 0;
	unsigned head = 0;

	inline unsigned slot(unsigned i) const { return (head + i) & 3; }
	inline void advance() { if (count == 3) { head = (head + 1) & 3; count = 2; } }

	/* attr bytes 0..15 = {S,T,Q,RGBA}: st register verbatim, then rgbaq
	 * with its two words swapped. bytes 16..23 = {fog, packed uv}. */
	static inline __m128i attr_lo()
	{
		__m128i st  = _mm_loadl_epi64((const __m128i *)&g_regs.st.bits);
		__m128i rq  = _mm_loadl_epi64((const __m128i *)&g_regs.rgbaq.bits);
		rq = _mm_shuffle_epi32(rq, _MM_SHUFFLE(3, 2, 0, 1)); /* {RGBA,Q}->{Q,RGBA} */
		return _mm_unpacklo_epi64(st, rq);
	}
	static inline uint32_t packed_uv()
	{
		return g_regs.uv.words[0] & 0x3fff3fffu;
	}
	inline void kick_xyz(Reg64<XYZBits> xyz)
	{
		advance();
		unsigned s = slot(count);
		/* pos: {X,Y} are two u16 at bits 0..31; widen, subtract offset,
		 * then place Z and write all 16 bytes at once. */
		__m128i xy = _mm_cvtsi32_si128((int)xyz.words[0]);
		xy = _mm_cvtepu16_epi32(xy);
		xy = _mm_sub_epi32(xy, g_ofxy);
		__m128i p = _mm_unpacklo_epi64(xy, _mm_cvtsi32_si128((int)xyz.desc.Z));
		_mm_store_si128((__m128i *)&pos[s], p);

		uint8_t *a = (uint8_t *)&attr[s];
		_mm_storeu_si128((__m128i *)a, attr_lo());
		float f = float(g_regs.fog.desc.FOG);
		uint32_t tail[2]; memcpy(&tail[0], &f, 4); tail[1] = packed_uv();
		memcpy(a + 16, tail, 8);
		count++;
	}
	inline void kick_xyzf(Reg64<XYZFBits> xyzf)
	{
		advance();
		unsigned s = slot(count);
		__m128i xy = _mm_cvtsi32_si128((int)xyzf.words[0]);
		xy = _mm_cvtepu16_epi32(xy);
		xy = _mm_sub_epi32(xy, g_ofxy);
		__m128i p = _mm_unpacklo_epi64(xy, _mm_cvtsi32_si128((int)xyzf.desc.Z));
		_mm_store_si128((__m128i *)&pos[s], p);

		uint8_t *a = (uint8_t *)&attr[s];
		_mm_storeu_si128((__m128i *)a, attr_lo());
		float f = float(xyzf.desc.F);
		uint32_t tail[2]; memcpy(&tail[0], &f, 4); tail[1] = packed_uv();
		memcpy(a + 16, tail, 8);
		count++;
	}
	inline const VertexPosition &rpos(unsigned i) const { return pos[slot(i)]; }
	inline const VertexAttribute &rattr(unsigned i) const { return attr[slot(i)]; }
	inline void maintain_fan()  { head = (head + 1) & 3; count = 2; }
	inline void maintain_list() { count = 0; head = 0; }
};

/* ---------------- V3: ring + SIMD position only ---------------- */
struct QueueV3 {
	alignas(16) VertexPosition pos[4];
	alignas(16) VertexAttribute attr[4];
	unsigned count = 0;
	unsigned head = 0;

	inline unsigned slot(unsigned i) const { return (head + i) & 3; }
	inline void advance() { if (count == 3) { head = (head + 1) & 3; count = 2; } }

	/* attr bytes 0..15 = {S,T,Q,RGBA}: st register verbatim, then rgbaq
	 * with its two words swapped. bytes 16..23 = {fog, packed uv}. */
	static inline __m128i attr_lo()
	{
		__m128i st  = _mm_loadl_epi64((const __m128i *)&g_regs.st.bits);
		__m128i rq  = _mm_loadl_epi64((const __m128i *)&g_regs.rgbaq.bits);
		rq = _mm_shuffle_epi32(rq, _MM_SHUFFLE(3, 2, 0, 1)); /* {RGBA,Q}->{Q,RGBA} */
		return _mm_unpacklo_epi64(st, rq);
	}
	static inline uint32_t packed_uv()
	{
		return g_regs.uv.words[0] & 0x3fff3fffu;
	}
	inline void kick_xyz(Reg64<XYZBits> xyz)
	{
		advance();
		unsigned s = slot(count);
		/* pos: {X,Y} are two u16 at bits 0..31; widen, subtract offset,
		 * then place Z and write all 16 bytes at once. */
		__m128i xy = _mm_cvtsi32_si128((int)xyz.words[0]);
		xy = _mm_cvtepu16_epi32(xy);
		xy = _mm_sub_epi32(xy, g_ofxy);
		__m128i p = _mm_unpacklo_epi64(xy, _mm_cvtsi32_si128((int)xyz.desc.Z));
		_mm_store_si128((__m128i *)&pos[s], p);

		auto &attr_ = attr[s];
		attr_.st.x = g_regs.st.desc.S; attr_.st.y = g_regs.st.desc.T;
		attr_.q = g_regs.rgbaq.desc.Q; attr_.rgba = g_regs.rgbaq.words[0];
		attr_.fog = float(g_regs.fog.desc.FOG);
		attr_.uv = u16vec2(g_regs.uv.desc.U, g_regs.uv.desc.V);
		count++;
	}
	inline void kick_xyzf(Reg64<XYZFBits> xyzf)
	{
		advance();
		unsigned s = slot(count);
		__m128i xy = _mm_cvtsi32_si128((int)xyzf.words[0]);
		xy = _mm_cvtepu16_epi32(xy);
		xy = _mm_sub_epi32(xy, g_ofxy);
		__m128i p = _mm_unpacklo_epi64(xy, _mm_cvtsi32_si128((int)xyzf.desc.Z));
		_mm_store_si128((__m128i *)&pos[s], p);

		auto &attr_ = attr[s];
		attr_.st.x = g_regs.st.desc.S; attr_.st.y = g_regs.st.desc.T;
		attr_.q = g_regs.rgbaq.desc.Q; attr_.rgba = g_regs.rgbaq.words[0];
		attr_.fog = float(xyzf.desc.F);
		attr_.uv = u16vec2(g_regs.uv.desc.U, g_regs.uv.desc.V);
		count++;
	}
	inline const VertexPosition &rpos(unsigned i) const { return pos[slot(i)]; }
	inline const VertexAttribute &rattr(unsigned i) const { return attr[slot(i)]; }
	inline void maintain_fan()  { head = (head + 1) & 3; count = 2; }
	inline void maintain_list() { count = 0; head = 0; }
};

/* ---------------- V4: ring + SIMD attribute only ---------------- */
struct QueueV4 {
	alignas(16) VertexPosition pos[4];
	alignas(16) VertexAttribute attr[4];
	unsigned count = 0;
	unsigned head = 0;

	inline unsigned slot(unsigned i) const { return (head + i) & 3; }
	inline void advance() { if (count == 3) { head = (head + 1) & 3; count = 2; } }

	/* attr bytes 0..15 = {S,T,Q,RGBA}: st register verbatim, then rgbaq
	 * with its two words swapped. bytes 16..23 = {fog, packed uv}. */
	static inline __m128i attr_lo()
	{
		__m128i st  = _mm_loadl_epi64((const __m128i *)&g_regs.st.bits);
		__m128i rq  = _mm_loadl_epi64((const __m128i *)&g_regs.rgbaq.bits);
		rq = _mm_shuffle_epi32(rq, _MM_SHUFFLE(3, 2, 0, 1)); /* {RGBA,Q}->{Q,RGBA} */
		return _mm_unpacklo_epi64(st, rq);
	}
	static inline uint32_t packed_uv()
	{
		return g_regs.uv.words[0] & 0x3fff3fffu;
	}
	inline void kick_xyz(Reg64<XYZBits> xyz)
	{
		advance();
		unsigned s = slot(count);
		/* pos: {X,Y} are two u16 at bits 0..31; widen, subtract offset,
		 * then place Z and write all 16 bytes at once. */
		auto &pos_ = pos[s];
		pos_.pos.x = int(xyz.desc.X) - g_ofx;
		pos_.pos.y = int(xyz.desc.Y) - g_ofy;
		pos_.z = xyz.desc.Z;

		uint8_t *a = (uint8_t *)&attr[s];
		_mm_storeu_si128((__m128i *)a, attr_lo());
		float f = float(g_regs.fog.desc.FOG);
		uint32_t tail[2]; memcpy(&tail[0], &f, 4); tail[1] = packed_uv();
		memcpy(a + 16, tail, 8);
		count++;
	}
	inline void kick_xyzf(Reg64<XYZFBits> xyzf)
	{
		advance();
		unsigned s = slot(count);
		auto &pos_ = pos[s];
		pos_.pos.x = int(xyzf.desc.X) - g_ofx;
		pos_.pos.y = int(xyzf.desc.Y) - g_ofy;
		pos_.z = xyzf.desc.Z;

		uint8_t *a = (uint8_t *)&attr[s];
		_mm_storeu_si128((__m128i *)a, attr_lo());
		float f = float(xyzf.desc.F);
		uint32_t tail[2]; memcpy(&tail[0], &f, 4); tail[1] = packed_uv();
		memcpy(a + 16, tail, 8);
		count++;
	}
	inline const VertexPosition &rpos(unsigned i) const { return pos[slot(i)]; }
	inline const VertexAttribute &rattr(unsigned i) const { return attr[slot(i)]; }
	inline void maintain_fan()  { head = (head + 1) & 3; count = 2; }
	inline void maintain_list() { count = 0; head = 0; }
};

/* ---------------- V5: ORIGINAL shift queue + SIMD attribute ---------------- */
struct QueueV5 {
	alignas(16) VertexPosition pos[3];
	alignas(16) VertexAttribute attr[3];
	unsigned count = 0;

	inline unsigned slot(unsigned i) const { return i; }
	inline void advance() {
		if (count == 3) {
			pos[0] = pos[1]; attr[0] = attr[1];
			pos[1] = pos[2]; attr[1] = attr[2];
			count = 2;
		}
	}

	/* attr bytes 0..15 = {S,T,Q,RGBA}: st register verbatim, then rgbaq
	 * with its two words swapped. bytes 16..23 = {fog, packed uv}. */
	static inline __m128i attr_lo()
	{
		__m128i st  = _mm_loadl_epi64((const __m128i *)&g_regs.st.bits);
		__m128i rq  = _mm_loadl_epi64((const __m128i *)&g_regs.rgbaq.bits);
		rq = _mm_shuffle_epi32(rq, _MM_SHUFFLE(3, 2, 0, 1)); /* {RGBA,Q}->{Q,RGBA} */
		return _mm_unpacklo_epi64(st, rq);
	}
	static inline uint32_t packed_uv()
	{
		return g_regs.uv.words[0] & 0x3fff3fffu;
	}
	inline void kick_xyz(Reg64<XYZBits> xyz)
	{
		advance();
		unsigned s = slot(count);
		/* pos: {X,Y} are two u16 at bits 0..31; widen, subtract offset,
		 * then place Z and write all 16 bytes at once. */
		auto &pos_ = pos[s];
		pos_.pos.x = int(xyz.desc.X) - g_ofx;
		pos_.pos.y = int(xyz.desc.Y) - g_ofy;
		pos_.z = xyz.desc.Z;

		uint8_t *a = (uint8_t *)&attr[s];
		_mm_storeu_si128((__m128i *)a, attr_lo());
		float f = float(g_regs.fog.desc.FOG);
		uint32_t tail[2]; memcpy(&tail[0], &f, 4); tail[1] = packed_uv();
		memcpy(a + 16, tail, 8);
		count++;
	}
	inline void kick_xyzf(Reg64<XYZFBits> xyzf)
	{
		advance();
		unsigned s = slot(count);
		auto &pos_ = pos[s];
		pos_.pos.x = int(xyzf.desc.X) - g_ofx;
		pos_.pos.y = int(xyzf.desc.Y) - g_ofy;
		pos_.z = xyzf.desc.Z;

		uint8_t *a = (uint8_t *)&attr[s];
		_mm_storeu_si128((__m128i *)a, attr_lo());
		float f = float(xyzf.desc.F);
		uint32_t tail[2]; memcpy(&tail[0], &f, 4); tail[1] = packed_uv();
		memcpy(a + 16, tail, 8);
		count++;
	}
	inline const VertexPosition &rpos(unsigned i) const { return pos[slot(i)]; }
	inline const VertexAttribute &rattr(unsigned i) const { return attr[slot(i)]; }
	inline void maintain_fan()  { pos[1] = pos[2]; attr[1] = attr[2]; count = 2; }
	inline void maintain_list() { count = 0; }
};

/* ---------------- workload ---------------- */
struct Rec { uint64_t st, rgbaq, uv, fog, xyz; };

static std::vector<Rec> make_workload(size_t n, unsigned seed)
{
	std::vector<Rec> v(n);
	uint64_t s = seed * 6364136223846793005ull + 1442695040888963407ull;
	auto nxt = [&]() { s = s * 6364136223846793005ull + 1442695040888963407ull; return s >> 16; };
	for (size_t i = 0; i < n; i++) {
		v[i].st = nxt(); v[i].rgbaq = nxt(); v[i].uv = nxt();
		v[i].fog = nxt(); v[i].xyz = nxt();
	}
	return v;
}

/* Integer-only consume, so the kick + queue dominate rather than float
 * conversions in the harness itself. Reads the same bytes drawing_kick_append
 * reads (all 3 positions and attributes of the assembled primitive). */
template <typename Q>
static inline uint64_t consume(const Q &q)
{
	uint64_t s = 0;
	for (unsigned k = 0; k < 3; k++) {
		const auto &p = q.rpos(2 - k);
		const auto &a = q.rattr(2 - k);
		s += uint64_t(uint32_t(p.pos.x)) + uint64_t(uint32_t(p.pos.y)) + p.z;
		uint32_t w[6]; memcpy(w, &a, 24);
		for (int j = 0; j < 6; j++) s += w[j];
	}
	return s;
}

template <typename Q, bool XYZF, bool LIST>
static uint64_t run(const std::vector<Rec> &w, int iters)
{
	Q q;
	uint64_t sink = 0;
	for (int it = 0; it < iters; it++) {
		q.maintain_list();
		for (size_t i = 0; i < w.size(); i++) {
			g_regs.st.bits = w[i].st;
			g_regs.rgbaq.bits = w[i].rgbaq;
			g_regs.uv.bits = w[i].uv;
			g_regs.fog.bits = w[i].fog;
			if (XYZF) q.kick_xyzf(Reg64<XYZFBits>(w[i].xyz));
			else      q.kick_xyz(Reg64<XYZBits>(w[i].xyz));
			if (q.count >= 3) {
				sink += consume(q);
				if (LIST) q.maintain_list();
			}
		}
	}
	return sink;
}

static double now_s()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}

typedef uint64_t (*RunFn)(const std::vector<Rec> &, int);

struct Variant { const char *name; RunFn fn; std::vector<double> samples; };

static uint64_t g_sink;

/* Interleaved A/B/C: one timed pass of every variant per round, so thermal
 * and scheduler drift hits all variants equally. Report the median. */
static void bench_group(const char *title, std::vector<Variant> &vs,
                        const std::vector<Rec> &w, int iters, int rounds)
{
	for (auto &v : vs) { v.samples.clear(); g_sink += v.fn(w, 2); } /* warm */
	for (int r = 0; r < rounds; r++) {
		for (auto &v : vs) {
			double t0 = now_s();
			g_sink += v.fn(w, iters);
			double t1 = now_s();
			v.samples.push_back((t1 - t0) * 1e9 / (double(w.size()) * double(iters)));
		}
	}
	printf("\n%s   (%d rounds, median ns/vertex)\n", title, rounds);
	double base = 0;
	for (auto &v : vs) {
		std::sort(v.samples.begin(), v.samples.end());
		double med = v.samples[v.samples.size() / 2];
		double lo = v.samples.front(), hi = v.samples.back();
		if (base == 0) base = med;
		printf("  %-22s %7.3f  [%.3f..%.3f]  %6.1f%%\n",
		       v.name, med, lo, hi, 100.0 * (med - base) / base);
	}
}

int main(int argc, char **argv)
{
	size_t n = 4096;
	int iters  = argc > 1 ? atoi(argv[1]) : 1500;
	int rounds = argc > 2 ? atoi(argv[2]) : 15;
	auto w = make_workload(n, 12345);
	g_ofx = 1024 << PGS_SUB; g_ofy = 1024 << PGS_SUB;
	g_ofxy = _mm_set_epi32(0, 0, g_ofy, g_ofx);

	std::vector<Variant> strip_xyz = {
		{ "V0 current",     run<QueueV0, false, false>, {} },
		{ "V1 ring queue",  run<QueueV1, false, false>, {} },
		{ "V2 ring + SIMD both", run<QueueV2, false, false>, {} },
		{ "V3 ring + SIMD pos",   run<QueueV3, false, false>, {} },
		{ "V4 ring + SIMD attr",  run<QueueV4, false, false>, {} },
		{ "V5 shift + SIMD attr", run<QueueV5, false, false>, {} },
	};
	std::vector<Variant> strip_xyzf = {
		{ "V0 current",     run<QueueV0, true, false>, {} },
		{ "V1 ring queue",  run<QueueV1, true, false>, {} },
		{ "V2 ring + SIMD both", run<QueueV2, true, false>, {} },
		{ "V3 ring + SIMD pos",   run<QueueV3, true, false>, {} },
		{ "V4 ring + SIMD attr",  run<QueueV4, true, false>, {} },
		{ "V5 shift + SIMD attr", run<QueueV5, true, false>, {} },
	};
	std::vector<Variant> list_xyz = {
		{ "V0 current",     run<QueueV0, false, true>, {} },
		{ "V1 ring queue",  run<QueueV1, false, true>, {} },
		{ "V2 ring + SIMD both", run<QueueV2, false, true>, {} },
		{ "V3 ring + SIMD pos",   run<QueueV3, false, true>, {} },
		{ "V4 ring + SIMD attr",  run<QueueV4, false, true>, {} },
		{ "V5 shift + SIMD attr", run<QueueV5, false, true>, {} },
	};

	bench_group("triangle strip, XYZ2  (queue shift fires every vertex)", strip_xyz, w, iters, rounds);
	bench_group("triangle strip, XYZF2", strip_xyzf, w, iters, rounds);
	bench_group("triangle list / sprite, XYZ2  (no shift)", list_xyz, w, iters, rounds);

	if (g_sink == 0x123456789ull) printf("impossible\n");
	return 0;
}
