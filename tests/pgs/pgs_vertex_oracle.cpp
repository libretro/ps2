/* Byte-exactness oracle for the C89 vertex kernels: every byte the kernels
 * write must equal what the muglm field-by-field bodies write. */
#include "common.h"
#include "pgs_vertex_kernels.h"
#include <cstdio>
#include <cstring>
#include <cstddef>

static Regs R; static int OFX, OFY;

static void ref_xyz(uint64_t v, VertexPosition &p, VertexAttribute &a)
{
	Reg64<XYZBits> x(v);
	p.pos.x = int(x.desc.X) - OFX; p.pos.y = int(x.desc.Y) - OFY; p.z = x.desc.Z;
	a.st.x = R.st.desc.S; a.st.y = R.st.desc.T;
	a.q = R.rgbaq.desc.Q; a.rgba = R.rgbaq.words[0];
	a.fog = float(R.fog.desc.FOG);
	a.uv.x = (uint16_t)R.uv.desc.U; a.uv.y = (uint16_t)R.uv.desc.V;
}
static void ref_xyzf(uint64_t v, VertexPosition &p, VertexAttribute &a)
{
	Reg64<XYZFBits> x(v);
	p.pos.x = int(x.desc.X) - OFX; p.pos.y = int(x.desc.Y) - OFY; p.z = x.desc.Z;
	a.st.x = R.st.desc.S; a.st.y = R.st.desc.T;
	a.q = R.rgbaq.desc.Q; a.rgba = R.rgbaq.words[0];
	a.fog = float(x.desc.F);
	a.uv.x = (uint16_t)R.uv.desc.U; a.uv.y = (uint16_t)R.uv.desc.V;
}
static pgs_kick_regs gather()
{
	pgs_kick_regs g;
	g.st = R.st.bits; g.rgbaq = R.rgbaq.bits;
	g.uv = R.uv.words[0]; g.fog = R.fog.words[1] >> 24;
	g.ofx = OFX; g.ofy = OFY;
	return g;
}

static uint64_t S = 0x243F6A8885A308D3ull;
static uint64_t rnd(){ S^=S<<13; S^=S>>7; S^=S<<17; return S; }


/* ---- pair kernels: the vector paths must equal the scalar contract ---- */
static int pair_oracle(void)
{
	long n = 0, f = 0;
	uint64_t S2 = 0x9E3779B97F4A7C15ull;
	int k;
	for (k = 0; k < 4000000; k++)
	{
		int32_t a[2], b[2], c[2], lo[2], hi[2], rlo[2], rhi[2], sl[2], sh[2];
		int use2;
		S2 ^= S2 << 13; S2 ^= S2 >> 7; S2 ^= S2 << 17;
		a[0] = (int32_t)S2; a[1] = (int32_t)(S2 >> 32);
		S2 ^= S2 << 13; S2 ^= S2 >> 7; S2 ^= S2 << 17;
		b[0] = (int32_t)S2; b[1] = (int32_t)(S2 >> 32);
		S2 ^= S2 << 13; S2 ^= S2 >> 7; S2 ^= S2 << 17;
		c[0] = (int32_t)S2; c[1] = (int32_t)(S2 >> 32);
		S2 ^= S2 << 13; S2 ^= S2 >> 7; S2 ^= S2 << 17;
		sl[0] = (int32_t)S2; sl[1] = (int32_t)(S2 >> 32);
		S2 ^= S2 << 13; S2 ^= S2 >> 7; S2 ^= S2 << 17;
		sh[0] = (int32_t)S2; sh[1] = (int32_t)(S2 >> 32);
		if ((k & 15) == 0) { a[0] = INT32_MIN; b[1] = INT32_MAX; }
		if ((k & 15) == 1) { c[0] = INT32_MAX; c[1] = INT32_MIN; }
		use2 = k & 1;

		pgs_pair_min_max3(a, b, c, use2, lo, hi);
		/* scalar reference, written out */
		rlo[0] = a[0] < b[0] ? a[0] : b[0]; rlo[1] = a[1] < b[1] ? a[1] : b[1];
		rhi[0] = a[0] > b[0] ? a[0] : b[0]; rhi[1] = a[1] > b[1] ? a[1] : b[1];
		if (use2) {
			rlo[0] = rlo[0] < c[0] ? rlo[0] : c[0]; rlo[1] = rlo[1] < c[1] ? rlo[1] : c[1];
			rhi[0] = rhi[0] > c[0] ? rhi[0] : c[0]; rhi[1] = rhi[1] > c[1] ? rhi[1] : c[1];
		}
		n++;
		if (memcmp(lo, rlo, 8) || memcmp(hi, rhi, 8)) f++;

		pgs_pair_clamp(sl, sh, lo, hi);
		rlo[0] = rlo[0] > sl[0] ? rlo[0] : sl[0]; rlo[1] = rlo[1] > sl[1] ? rlo[1] : sl[1];
		rhi[0] = rhi[0] < sh[0] ? rhi[0] : sh[0]; rhi[1] = rhi[1] < sh[1] ? rhi[1] : sh[1];
		if (memcmp(lo, rlo, 8) || memcmp(hi, rhi, 8)) f++;

		/* equality helpers against the muglm form they replace */
		{
			/* reference: compare the two lanes one at a time */
			int ref = (a[0] == b[0] && a[1] == b[1]) ? 1 : 0;
			if (pgs_ivec2_eq(a, b) != ref) f++;
			if (pgs_ivec2_eq(a, a) != 1) f++;
		}
		{
			uint16_t u0[2], u1[2];
			u0[0] = (uint16_t)a[0]; u0[1] = (uint16_t)a[1];
			u1[0] = (uint16_t)b[0]; u1[1] = (uint16_t)b[1];
			int ref = (u0[0] == u1[0] && u0[1] == u1[1]) ? 1 : 0;
			if (pgs_u16vec2_eq(u0, u1) != ref) f++;
			if (pgs_u16vec2_eq(u0, u0) != 1) f++;
		}
	}
	printf("%s: pair kernels, %ld cases, %ld mismatches\n", f ? "FAIL" : "PASS", n, f);
	return f != 0;
}

/* The two rules the CPU-side consumers of Q rest on.
 *
 * Neither can be checked by comparing against the plain C++ spelling,
 * because the plain spelling is what they exist to replace: int32_t(v)
 * is undefined for NaN and outside the destination range, and the two
 * architectures the core ships on disagree -- x86 answers INT32_MIN for
 * both +inf and NaN, aarch64 answers INT32_MAX and 0. So the reference
 * here is the rule itself, stated independently, and the lane runs on
 * both architectures so the two have to produce the same numbers. */
static long degenerate_oracle(void)
{
	static const float interesting[] = {
		0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1e-30f, -1e-30f,
		2147483520.0f, 2147483648.0f, 4294967296.0f, 1e30f,
		-2147483648.0f, -2147483904.0f, -1e30f,
		16777216.0f, -16777216.0f, 8388608.5f, 0.99999994f, -0.99999994f,
	};
	const int n = (int)(sizeof(interesting) / sizeof(interesting[0]));
	long bad = 0;
	int i;

	/* pgs_q_is_usable: false for either zero, for either infinity and
	 * for any NaN; true for every other float. */
	for (i = 0; i < n; i++)
	{
		const float q = interesting[i];
		const int want = !(q == 0.0f);
		if (pgs_q_is_usable(q) != want)
		{
			printf("Q USABLE MISMATCH %.9g: got %d want %d\n",
			       (double)q, pgs_q_is_usable(q), want);
			bad++;
		}
	}
	{
		const float inf = 1.0f / 0.0f;
		const float nan = 0.0f / 0.0f;
		if (pgs_q_is_usable(inf) || pgs_q_is_usable(-inf) || pgs_q_is_usable(nan))
			{ printf("Q USABLE accepted a non-finite\n"); bad++; }
	}

	/* pgs_f32_to_i32_sat: NaN is zero, anything at or above 2^31 is
	 * INT32_MAX, anything below -2^31 is INT32_MIN, and inside the
	 * range it truncates toward zero exactly as the cast does. */
	for (i = 0; i < n; i++)
	{
		const float v = interesting[i];
		int32_t want;
		if (v >= 2147483648.0f)       want = 2147483647;
		else if (v < -2147483648.0f)  want = -2147483647 - 1;
		else                          want = (int32_t)v;
		if (pgs_f32_to_i32_sat(v) != want)
		{
			printf("SAT MISMATCH %.9g: got %d want %d\n",
			       (double)v, pgs_f32_to_i32_sat(v), want);
			bad++;
		}
	}
	{
		const float inf = 1.0f / 0.0f;
		const float nan = 0.0f / 0.0f;
		if (pgs_f32_to_i32_sat(nan) != 0)          { printf("SAT NaN not 0\n"); bad++; }
		if (pgs_f32_to_i32_sat(inf) != 2147483647) { printf("SAT +inf not INT32_MAX\n"); bad++; }
		if (pgs_f32_to_i32_sat(-inf) != -2147483647 - 1) { printf("SAT -inf not INT32_MIN\n"); bad++; }
	}

	/* And the shape the consumers actually hit: S/Q with a Q that is
	 * not usable must never reach the conversion at all, but if it did
	 * the answer still has to be the same on every host. */
	{
		const float zero = 0.0f;
		float st;
		for (st = -2.0f; st <= 2.0f; st += 0.5f)
		{
			const int32_t got = pgs_f32_to_i32_sat((st / zero) * 1024.0f);
			const int32_t want = st > 0.0f ? 2147483647 : (st < 0.0f ? -2147483647 - 1 : 0);
			if (got != want)
			{
				printf("S/Q MISMATCH st=%.9g: got %d want %d\n",
				       (double)st, got, want);
				bad++;
			}
		}
	}

	printf("%s: degenerate Q rules\n", bad ? "FAIL" : "PASS");
	return bad;
}

int main()
{
	long n = 0, f_pos = 0, f_attr = 0, f_pad = 0;
	static const int offs[] = { 0, 1, -1, 2047, 1024 << PGS_SUBPIXEL_BITS,
	                            65535, -65536, 0x7fffffff, -2048 };
	int oi, oj, k, variant;

	/* static asserts the kernels' offsets depend on */
	static_assert(sizeof(VertexAttribute) == PGS_ATTR_SIZE, "attr size");
	static_assert(offsetof(VertexAttribute, st)  == PGS_ATTR_ST_OFFSET, "st");
	static_assert(offsetof(VertexAttribute, q)   == PGS_ATTR_Q_OFFSET, "q");
	static_assert(offsetof(VertexAttribute, fog) == PGS_ATTR_FOG_OFFSET, "fog");
	static_assert(sizeof(VertexPosition) == 16, "pos size");

	for (oi = 0; oi < 9; oi++) for (oj = 0; oj < 9; oj++) {
		OFX = offs[oi]; OFY = offs[oj];
		for (k = 0; k < 40000; k++) {
			uint64_t v;
			R.st.bits = rnd(); R.rgbaq.bits = rnd();
			R.uv.bits = rnd(); R.fog.bits = rnd();
			v = rnd();
			if ((k & 31) == 0) v = 0;
			if ((k & 31) == 1) v = ~0ull;
			if ((k & 31) == 2) v = 0x00ffffff0000ffffull;

			for (variant = 0; variant < 2; variant++) {
				VertexPosition p0, p1, p2; VertexAttribute a0, a1;
				pgs_kick_regs g = gather();
				uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
				uint32_t z; float fog;

				memset(&p0, 0xA5, sizeof p0); memset(&p1, 0x5A, sizeof p1);
				memset(&p2, 0x3C, sizeof p2);
				memset(&a0, 0xA5, sizeof a0); memset(&a1, 0x5A, sizeof a1);

				if (variant) { ref_xyzf(v, p0, a0); z = hi & 0xffffffu; fog = (float)(hi >> 24); }
				else         { ref_xyz (v, p0, a0); z = hi;             fog = (float)g.fog;      }

				pgs_build_position(&g, lo, z, &p1);
				pgs_build_position_padded(&g, lo, z, &p2);
				pgs_build_attribute(&g, fog, &a1);
				n++;

				if (memcmp(&p0, &p1, PGS_POS_DEFINED_SIZE)) f_pos++;
				if (memcmp(&p0, &p2, PGS_POS_DEFINED_SIZE)) f_pos++;
				if (p2.padding != 0) f_pad++;
				if (memcmp(&a0, &a1, PGS_ATTR_SIZE)) {
					if (f_attr < 3) {
						uint32_t w0[6], w1[6];
						memcpy(w0,&a0,24); memcpy(w1,&a1,24);
						printf("ATTR MISMATCH off=(%d,%d) v=%016llx var=%d\n",
						       OFX, OFY, (unsigned long long)v, variant);
						for (int j=0;j<6;j++)
							printf("   [%d] ref %08x  c89 %08x%s\n", j, w0[j], w1[j],
							       w0[j]!=w1[j] ? "   <<" : "");
					}
					f_attr++;
				}
			}
		}
	}
	if (pair_oracle()) f_attr++;
	if (degenerate_oracle()) f_attr++;
	printf("%s: %ld cases  pos_mismatch=%ld attr_mismatch=%ld pad_nonzero=%ld\n",
	       (f_pos||f_attr||f_pad) ? "FAIL" : "PASS", n, f_pos, f_attr, f_pad);
	return (f_pos||f_attr||f_pad) != 0;
}
