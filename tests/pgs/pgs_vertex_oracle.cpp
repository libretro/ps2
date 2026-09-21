/* Byte-exactness oracle for the C89 vertex kernels: every byte the kernels
 * write must equal what the muglm field-by-field bodies write. */
#include "common.h"
#include "pgs_vertex_kernels.h"
#include <cstdio>
#include <cstring>

static Regs R; static int OFX, OFY;

static void ref_xyz(uint64_t v, VertexPosition &p, VertexAttribute &a)
{
	Reg64<XYZBits> x(v);
	p.pos.x = int(x.desc.X) - OFX; p.pos.y = int(x.desc.Y) - OFY; p.z = x.desc.Z;
	a.st.x = R.st.desc.S; a.st.y = R.st.desc.T;
	a.q = R.rgbaq.desc.Q; a.rgba = R.rgbaq.words[0];
	a.fog = float(R.fog.desc.FOG);
	a.uv = u16vec2(R.uv.desc.U, R.uv.desc.V);
}
static void ref_xyzf(uint64_t v, VertexPosition &p, VertexAttribute &a)
{
	Reg64<XYZFBits> x(v);
	p.pos.x = int(x.desc.X) - OFX; p.pos.y = int(x.desc.Y) - OFY; p.z = x.desc.Z;
	a.st.x = R.st.desc.S; a.st.y = R.st.desc.T;
	a.q = R.rgbaq.desc.Q; a.rgba = R.rgbaq.words[0];
	a.fog = float(x.desc.F);
	a.uv = u16vec2(R.uv.desc.U, R.uv.desc.V);
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
	printf("%s: %ld cases  pos_mismatch=%ld attr_mismatch=%ld pad_nonzero=%ld\n",
	       (f_pos||f_attr||f_pad) ? "FAIL" : "PASS", n, f_pos, f_attr, f_pad);
	return (f_pos||f_attr||f_pad) != 0;
}
