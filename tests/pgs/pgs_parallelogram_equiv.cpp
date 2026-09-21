/* Parallelogram-detection equivalence lane.
 *
 * The two gs_util predicates are being moved off muglm's vector operators
 * onto plain scalar C. The scalar form must agree with the muglm form for
 * every input, including the float path, where notEqual and greaterThan keep
 * IEEE semantics (a NaN compares unequal to itself, so a bit compare would
 * not do).
 */
#include "common.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>

/* ---- the muglm forms, as they were ---- */
static bool mg_pos3_match(const VertexPosition *pos, const muglm::ivec3 &order,
                          const VertexPosition *last_pos, const muglm::ivec3 &last_order)
{
	auto &pos0 = pos[order.x]; auto &pos1 = pos[order.y]; auto &pos2 = pos[order.z];
	auto pos3 = pos1.pos + pos2.pos - pos0.pos;
	auto &lp0 = last_pos[last_order.x];
	auto &lp1 = last_pos[last_order.y];
	auto &lp2 = last_pos[last_order.z];
	return !(any(notEqual(pos3, lp0.pos)) ||
	         any(notEqual(pos1.pos, lp1.pos)) ||
	         any(notEqual(pos2.pos, lp2.pos)));
}
static bool mg_uv3_match(const VertexAttribute &a0, const VertexAttribute &a1,
                         const VertexAttribute &a2, const VertexAttribute &l0,
                         const VertexAttribute &l1, const VertexAttribute &l2)
{
	muglm::u16vec2 uv3 = a1.uv + a2.uv - a0.uv;
	return !(any(notEqual(uv3, l0.uv)) ||
	         any(notEqual(a1.uv, l1.uv)) ||
	         any(notEqual(a2.uv, l2.uv)));
}
static bool mg_st_match(const VertexAttribute &a0, const VertexAttribute &a1,
                        const VertexAttribute &a2, const VertexAttribute &l0,
                        const VertexAttribute &l1, const VertexAttribute &l2)
{
	muglm::vec2 st3_error = muglm::abs((a2.st + a1.st - a0.st - l0.st) / a0.q);
	return !(any(notEqual(a1.st, l1.st)) ||
	         any(notEqual(a2.st, l2.st)) ||
	         any(greaterThan(st3_error, muglm::vec2(1e-4f))));
}
static int mg_area(const VertexPosition *pos, int &abx, int &aby, int &acx, int &acy,
                   int &bcx, int &bcy)
{
	muglm::ivec2 ab = pos[1].pos - pos[0].pos;
	muglm::ivec2 ac = pos[2].pos - pos[0].pos;
	muglm::ivec2 bc = pos[2].pos - pos[1].pos;
	abx = ab.x; aby = ab.y; acx = ac.x; acy = ac.y; bcx = bc.x; bcy = bc.y;
	return std::abs(ab.x * ac.y - ab.y * ac.x);
}

/* ---- the scalar forms ---- */
static bool sc_pos3_match(const VertexPosition *pos, const int *order,
                          const VertexPosition *last_pos, const int *last_order)
{
	const VertexPosition *p0 = &pos[order[0]], *p1 = &pos[order[1]], *p2 = &pos[order[2]];
	const VertexPosition *l0 = &last_pos[last_order[0]];
	const VertexPosition *l1 = &last_pos[last_order[1]];
	const VertexPosition *l2 = &last_pos[last_order[2]];
	int32_t x3 = p1->pos.x + p2->pos.x - p0->pos.x;
	int32_t y3 = p1->pos.y + p2->pos.y - p0->pos.y;
	if (x3 != l0->pos.x || y3 != l0->pos.y) return false;
	if (p1->pos.x != l1->pos.x || p1->pos.y != l1->pos.y) return false;
	if (p2->pos.x != l2->pos.x || p2->pos.y != l2->pos.y) return false;
	return true;
}
static bool sc_uv3_match(const VertexAttribute &a0, const VertexAttribute &a1,
                         const VertexAttribute &a2, const VertexAttribute &l0,
                         const VertexAttribute &l1, const VertexAttribute &l2)
{
	uint16_t u3 = (uint16_t)(a1.uv.x + a2.uv.x - a0.uv.x);
	uint16_t v3 = (uint16_t)(a1.uv.y + a2.uv.y - a0.uv.y);
	if (u3 != l0.uv.x || v3 != l0.uv.y) return false;
	if (a1.uv.x != l1.uv.x || a1.uv.y != l1.uv.y) return false;
	if (a2.uv.x != l2.uv.x || a2.uv.y != l2.uv.y) return false;
	return true;
}
static bool sc_st_match(const VertexAttribute &a0, const VertexAttribute &a1,
                        const VertexAttribute &a2, const VertexAttribute &l0,
                        const VertexAttribute &l1, const VertexAttribute &l2)
{
	float ex = std::fabs((a2.st.x + a1.st.x - a0.st.x - l0.st.x) / a0.q);
	float ey = std::fabs((a2.st.y + a1.st.y - a0.st.y - l0.st.y) / a0.q);
	if (a1.st.x != l1.st.x || a1.st.y != l1.st.y) return false;
	if (a2.st.x != l2.st.x || a2.st.y != l2.st.y) return false;
	if (ex > 1e-4f || ey > 1e-4f) return false;
	return true;
}
static int sc_area(const VertexPosition *pos, int &abx, int &aby, int &acx, int &acy,
                   int &bcx, int &bcy)
{
	abx = pos[1].pos.x - pos[0].pos.x; aby = pos[1].pos.y - pos[0].pos.y;
	acx = pos[2].pos.x - pos[0].pos.x; acy = pos[2].pos.y - pos[0].pos.y;
	bcx = pos[2].pos.x - pos[1].pos.x; bcy = pos[2].pos.y - pos[1].pos.y;
	return std::abs(abx * acy - aby * acx);
}

static uint64_t X = 0x243F6A8885A308D3ull;
static uint32_t rnd(){ X ^= X<<13; X ^= X>>7; X ^= X<<17; return (uint32_t)(X>>16); }
static float rndf(int k)
{
	switch (k & 7) {
	case 0: return 0.0f;
	case 1: return -0.0f;
	case 2: return NAN;
	case 3: return INFINITY;
	case 4: return 1e-5f;
	case 5: return 1e-3f;
	default: { float f; uint32_t b = rnd(); memcpy(&f, &b, 4); return f; }
	}
}

int main(void)
{
	long n = 0, f = 0;
	int i;
	for (i = 0; i < 3000000; i++) {
		VertexPosition p[3], lp[3];
		VertexAttribute a[3], la[3];
		int order[3], lorder[3];
		int k;
		for (k = 0; k < 3; k++) {
			p[k].pos.x = (int32_t)rnd(); p[k].pos.y = (int32_t)rnd();
			lp[k].pos.x = (int32_t)rnd(); lp[k].pos.y = (int32_t)rnd();
			a[k].uv.x = (uint16_t)rnd(); a[k].uv.y = (uint16_t)rnd();
			la[k].uv.x = (uint16_t)rnd(); la[k].uv.y = (uint16_t)rnd();
			a[k].st.x = rndf(i + k); a[k].st.y = rndf(i + k + 1);
			la[k].st.x = rndf(i + k + 2); la[k].st.y = rndf(i + k + 3);
			a[k].q = rndf(i + k + 4); la[k].q = rndf(i + k + 5);
		}
		/* make the equal case reachable */
		if ((i & 3) == 0) { memcpy(lp, p, sizeof lp); memcpy(la, a, sizeof la); }
		for (k = 0; k < 3; k++) { order[k] = (int)(rnd() % 3); lorder[k] = (int)(rnd() % 3); }

		{
			muglm::ivec3 mo(order[0], order[1], order[2]);
			muglm::ivec3 mlo(lorder[0], lorder[1], lorder[2]);
			if (mg_pos3_match(p, mo, lp, mlo) != sc_pos3_match(p, order, lp, lorder)) f++;
		}
		if (mg_uv3_match(a[0],a[1],a[2],la[0],la[1],la[2]) !=
		    sc_uv3_match(a[0],a[1],a[2],la[0],la[1],la[2])) f++;
		if (mg_st_match(a[0],a[1],a[2],la[0],la[1],la[2]) !=
		    sc_st_match(a[0],a[1],a[2],la[0],la[1],la[2])) f++;
		{
			int b1[6], b2[6];
			int r1 = mg_area(p, b1[0],b1[1],b1[2],b1[3],b1[4],b1[5]);
			int r2 = sc_area(p, b2[0],b2[1],b2[2],b2[3],b2[4],b2[5]);
			if (r1 != r2 || memcmp(b1, b2, sizeof b1)) f++;
		}
		n += 4;
	}
	printf("%s: parallelogram scalar-vs-muglm, %ld comparisons, %ld failures\n",
	       f ? "FAIL" : "PASS", n, f);
	return f != 0;
}
