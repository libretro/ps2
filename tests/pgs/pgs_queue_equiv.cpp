/* Queue-equivalence lane.
 *
 * The ring-buffer vertex queue must deliver exactly the same sequence of
 * (position, attribute) triples to drawing_kick_append as the copy-based
 * shift queue it replaces, for every primitive topology, across topology
 * changes mid-stream and across the invalid-kick flush.
 *
 * Both models are driven by one shared script, so any divergence in the
 * primitive stream is the queue and nothing else.
 */
#include "common.h"
#include "pgs_vertex_kernels.h"
#include <cstdio>
#include <cstring>
#include <vector>

static Regs R; static int OFX, OFY;

static pgs_kick_regs gather()
{
	pgs_kick_regs g;
	g.st = R.st.bits; g.rgbaq = R.rgbaq.bits;
	g.uv = R.uv.words[0]; g.fog = R.fog.words[1] >> 24;
	g.ofx = OFX; g.ofy = OFY;
	return g;
}

/* The seven topologies, in (num_vertices, list, fan) form. */
struct Topo { const char *name; unsigned nv; bool list; bool fan; };
static const Topo TOPOS[] = {
	{ "pointlist",     1, true,  false },
	{ "linelist",      2, true,  false },
	{ "linestrip",     2, false, false },
	{ "trianglelist",  3, true,  false },
	{ "trianglestrip", 3, false, false },
	{ "trianglefan",   3, false, true  },
	{ "sprite",        2, true,  false },
};

/* ---- model A: the shift queue, as it was ---- */
struct QShift {
	VertexPosition pos[3]; VertexAttribute attr[3]; unsigned count = 0;
	void kick(uint64_t v, bool f) {
		pgs_kick_regs g = gather();
		uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
		if (count == 3) {
			pos[0]=pos[1]; attr[0]=attr[1]; pos[1]=pos[2]; attr[1]=attr[2]; count=2;
		}
		pgs_build_position(&g, lo, f ? (hi & 0xffffffu) : hi, &pos[count]);
		pgs_build_attribute(&g, f ? (float)(hi >> 24) : (float)g.fog, &attr[count]);
		count++;
	}
	unsigned idx(unsigned i) const { return i; }
	void maintain(const Topo &t) {
		if (t.fan)       { pos[1]=pos[2]; attr[1]=attr[2]; count=2; }
		else if (t.list) { count = 0; }
	}
	void flush_invalid() { count = 0; }
	void reset() { count = 0; }
};

/* ---- model B: the ring queue, as shipped ---- */
struct QRing {
	enum { MaxEntries = 4 };
	VertexPosition pos[MaxEntries]; VertexAttribute attr[MaxEntries];
	unsigned count = 0, head = 0;
	unsigned slot(unsigned i) const { return (head + i) & (MaxEntries - 1); }
	void kick(uint64_t v, bool f) {
		pgs_kick_regs g = gather();
		uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
		if (count == 3) { head = slot(1); count = 2; }
		pgs_build_position(&g, lo, f ? (hi & 0xffffffu) : hi, &pos[slot(count)]);
		pgs_build_attribute(&g, f ? (float)(hi >> 24) : (float)g.fog, &attr[slot(count)]);
		count++;
	}
	unsigned idx(unsigned i) const { return slot(i); }
	void maintain(const Topo &t) {
		if (t.fan)       { pos[slot(1)]=pos[slot(2)]; attr[slot(1)]=attr[slot(2)]; count=2; }
		else if (t.list) { count = 0; }
	}
	void flush_invalid() { count = 0; }
	void reset() { count = 0; head = 0; }
};

struct Out { unsigned nv; uint8_t pos[3][12]; uint8_t attr[3][24]; };

/* One shared script: (topology index, vertex word, xyzf flag, adc, invalid). */
struct Step { uint8_t topo, f, adc, invalid; uint64_t st, rgbaq, uv, fog, xyz; };

static std::vector<Step> make_script(uint64_t seed, int n)
{
	std::vector<Step> s(n);
	uint64_t x = seed | 1;
	auto nx = [&]{ x^=x<<13; x^=x>>7; x^=x<<17; return x; };
	uint8_t topo = 0;
	int i;
	for (i = 0; i < n; i++) {
		uint64_t r = nx();
		/* change topology now and then, the way a PRIM write would */
		if ((r & 0x3f) == 0) topo = (uint8_t)(nx() % 7);
		s[i].topo = topo;
		s[i].f = (uint8_t)(r & 1);
		s[i].adc = (uint8_t)((r >> 8) & 1 ? ((r >> 9) & 1) : 0);
		s[i].invalid = (uint8_t)((r & 0x1ff) == 0x1ff);
		s[i].st = nx(); s[i].rgbaq = nx(); s[i].uv = nx();
		s[i].fog = nx(); s[i].xyz = nx();
	}
	return s;
}

template <typename Q>
static void drive(std::vector<Out> &out, const std::vector<Step> &script)
{
	Q q; size_t i;
	q.reset(); out.clear();
	for (i = 0; i < script.size(); i++) {
		const Step &s = script[i];
		const Topo &t = TOPOS[s.topo];
		R.st.bits = s.st; R.rgbaq.bits = s.rgbaq;
		R.uv.bits = s.uv; R.fog.bits = s.fog;

		if (s.invalid) { q.flush_invalid(); continue; }

		q.kick(s.xyz, s.f != 0);

		if (q.count < t.nv) continue;
		if (!s.adc) {
			Out o{}; unsigned k;
			o.nv = t.nv;
			for (k = 0; k < t.nv; k++) {
				unsigned j = (t.nv == 3) ? (2 - k) : (q.count - 1 - k);
				memcpy(o.pos[k], &q.pos[q.idx(j)], 12);
				memcpy(o.attr[k], &q.attr[q.idx(j)], 24);
			}
			out.push_back(o);
		}
		q.maintain(t);
	}
}

int main(void)
{
	static const int offs[] = { 0, 1024 << PGS_SUBPIXEL_BITS, -2048, 65535 };
	long fails = 0, prims = 0;
	int oi; uint64_t seed;

	for (oi = 0; oi < 4; oi++) {
		OFX = offs[oi]; OFY = offs[(oi + 1) & 3];
		for (seed = 1; seed <= 300; seed++) {
			std::vector<Out> a, b; size_t i;
			std::vector<Step> script = make_script(seed * 0x9E3779B97F4A7C15ull, 20000);
			drive<QShift>(a, script);
			drive<QRing>(b, script);
			prims += (long)a.size();
			if (a.size() != b.size()) {
				if (fails < 5)
					printf("FAIL seed=%llu: %zu vs %zu primitives\n",
					       (unsigned long long)seed, a.size(), b.size());
				fails++; continue;
			}
			for (i = 0; i < a.size(); i++) {
				if (memcmp(&a[i], &b[i], sizeof(Out)) != 0) {
					if (fails < 5)
						printf("FAIL off=(%d,%d) seed=%llu primitive #%zu (nv=%u)\n",
						       OFX, OFY, (unsigned long long)seed, i, a[i].nv);
					fails++; break;
				}
			}
		}
	}
	printf("%s: queue equivalence, %ld primitives compared, %ld failures\n",
	       fails ? "FAIL" : "PASS", prims, fails);
	return fails != 0;
}
