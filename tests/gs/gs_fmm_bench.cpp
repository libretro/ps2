/* Is fusing FindMinMax into vertex kick worth it on x86-64?
 *
 * yaps2's GV-6 accumulates the draw's min/max at index-emission time instead
 * of re-walking the index list at flush. Their implementation is aarch64-only
 * because it leans on FMIN/FMAX propagating NaN, which SSE min/max does not --
 * so on x86 the fused path never runs and the legacy walk is kept everywhere.
 *
 * The NaN behaviour is fixable on SSE (accumulate a sticky unordered mask
 * alongside the min/max, which is one cmpps + orps per pair). The question
 * this answers is whether it would be worth doing: the structural claim -- a
 * second walk over the index list, with strip vertices visited up to three
 * times, and a divide per vertex pair -- is architecture-independent, so it
 * should be measurable here.
 *
 * Two loops are timed over the same draw:
 *
 *   legacy  walk the index list, gather each vertex, min/max, divide by Q
 *   fused   accumulate over each vertex once as it is kicked, no index
 *           indirection and no gather, plus a per-draw finish
 *
 * The fused number is the cost that would be *added* to the kick loop; the
 * legacy number is what would be removed at flush. The comparison is
 * meaningful only if the fused side also carries the sticky-NaN work that x86
 * would need, so it does.
 *
 * Build: sh tests/gs/build.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "GS/GSVertexKick.h"

#define VERTS 4096
#define ITERATIONS 4000

/* Same footprint and layout as GSVertex: two 128-bit lanes. */
typedef struct
{
	GSVector4i m0; /* ST + RGBA + Q */
	GSVector4i m1; /* XY + UV + Z/F */
} BenchVertex;

static u64 g_seed = 0xc3d2e1f09b8a7645ull;

static u32 Rand32(void)
{
	g_seed ^= g_seed << 13;
	g_seed ^= g_seed >> 7;
	g_seed ^= g_seed << 17;
	return (u32)(g_seed >> 16);
}

static double NowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void)
{
	static BenchVertex verts[VERTS];
	static u16 index[VERTS * 3];
	GSVector4 tmin, tmax;
	GSVector4i pmin, pmax, cmin, cmax;
	double t0, t1, legacy_time, fused_time;
	unsigned long sink = 0;
	int i;
	long it;
	int index_count;
	int shape;

	for (i = 0; i < VERTS; i++)
	{
		float s = (float)((int)(Rand32() % 2000) - 1000) * 0.01f;
		float t = (float)((int)(Rand32() % 2000) - 1000) * 0.01f;
		float q = 1.0f + (float)(Rand32() % 100) * 0.01f;
		GSVector4 stq = GSVector4(s, t, q, 0.0f);
		verts[i].m0 = GSVector4i::cast(stq);
		verts[i].m1 = GSVector4i((int)(Rand32() % 8000), (int)(Rand32() % 8000),
		                         (int)Rand32(), (int)Rand32());
	}

	for (shape = 0; shape < 2; shape++)
	{
	if (shape == 0)
	{
		/* Triangle strip: each primitive re-references two previous vertices,
		 * so the index list visits each vertex about three times. That
		 * redundancy is the largest part of what the fused form avoids. */
		index_count = 0;
		for (i = 2; i < VERTS; i++)
		{
			index[index_count++] = (u16)(i - 2);
			index[index_count++] = (u16)(i - 1);
			index[index_count++] = (u16)i;
		}
	}
	else
	{
		/* Triangle list: every index is a distinct vertex, so the walk and the
		 * fused accumulate touch the same number of vertices and only the
		 * consolidated divide remains as a win. This is the pessimistic
		 * shape, and the honest bound on the payoff. */
		index_count = 0;
		for (i = 0; i + 3 <= VERTS; i += 3)
		{
			index[index_count++] = (u16)i;
			index[index_count++] = (u16)(i + 1);
			index[index_count++] = (u16)(i + 2);
		}
	}

	/* ---- legacy: walk the index list at flush ------------------------- */
	t0 = NowSeconds();
	for (it = 0; it < ITERATIONS; it++)
	{
		tmin = GSVector4(FLT_MAX); tmax = GSVector4(-FLT_MAX);
		pmin = GSVector4i::xffffffff(); pmax = GSVector4i::zero();
		cmin = GSVector4i::xffffffff(); cmax = GSVector4i::zero();

		for (i = 0; i < index_count; i += 2)
		{
			const BenchVertex& v0 = verts[index[i]];
			const BenchVertex& v1 = verts[index[i + 1 < index_count ? i + 1 : i]];
			GSVector4 stq0 = GSVector4::cast(v0.m0);
			GSVector4 stq1 = GSVector4::cast(v1.m0);
			GSVector4 q = stq0.zzzz(stq1);
			GSVector4 st = stq0.xyxy(stq1) / q;   /* the divide per vertex pair */

			tmin = tmin._min(st);
			tmax = tmax._max(st);
			pmin = pmin.min_i32(v0.m1.min_i32(v1.m1));
			pmax = pmax.max_i32(v0.m1.max_i32(v1.m1));
			cmin = cmin.min_u8(v0.m0.min_u8(v1.m0));
			cmax = cmax.max_u8(v0.m0.max_u8(v1.m0));
		}
		sink += (unsigned long)pmin.extract32<0>() ^ (unsigned long)tmax.extract32<0>();
	}
	t1 = NowSeconds();
	legacy_time = t1 - t0;

	/* ---- fused: accumulate once per vertex at kick time --------------- */
	t0 = NowSeconds();
	for (it = 0; it < ITERATIONS; it++)
	{
		GSVector4 raw_min = GSVector4(FLT_MAX), raw_max = GSVector4(-FLT_MAX);
		GSVector4 nan_sticky = GSVector4::zero();

		pmin = GSVector4i::xffffffff(); pmax = GSVector4i::zero();
		cmin = GSVector4i::xffffffff(); cmax = GSVector4i::zero();

		/* One accumulate per vertex kicked: a strip kicks prims+2 vertices,
		 * a list kicks three per prim. This asymmetry is the whole point --
		 * the walk pays for index redundancy, the fused form does not. */
		const int fused_verts = (shape == 0) ? VERTS : index_count;
		for (i = 0; i < fused_verts; i++)
		{
			const BenchVertex& v = verts[i];
			GSVector4 stq = GSVector4::cast(v.m0);

			/* Sticky unordered mask: what SSE needs to reproduce the legacy
			 * NaN reporting that ARM gets free from FMIN/FMAX propagation. */
			nan_sticky = nan_sticky | (stq == stq).andnot(GSVector4::xffffffff());

			raw_min = raw_min._min(stq);
			raw_max = raw_max._max(stq);
			pmin = pmin.min_i32(v.m1);
			pmax = pmax.max_i32(v.m1);
			cmin = cmin.min_u8(v.m0);
			cmax = cmax.max_u8(v.m0);
		}

		/* Finish: one divide for the whole draw, not one per pair. */
		{
			GSVector4 q = raw_min.zzzz();
			tmin = raw_min / q;
			tmax = raw_max / q;
		}
		sink += (unsigned long)pmin.extract32<0>() ^ (unsigned long)tmax.extract32<0>();
		sink += (unsigned long)nan_sticky.extract32<0>();
	}
	t1 = NowSeconds();
	fused_time = t1 - t0;

	{
		const double prims = (double)(index_count / 3) * (double)ITERATIONS;
		printf("\n%s: %d indices, %d prims/draw, %d iterations\n",
		       shape == 0 ? "triangle strip (3x vertex reuse)" : "triangle list (no reuse)",
		       index_count, index_count / 3, ITERATIONS);
		printf("  legacy walk at flush   %6.2f ns/prim\n", legacy_time / prims * 1e9);
		printf("  fused accumulate       %6.2f ns/prim\n", fused_time / prims * 1e9);
		printf("  net                    %+6.2f ns/prim (%.2fx)\n",
		       (fused_time - legacy_time) / prims * 1e9, legacy_time / fused_time);
	}
	}

	if (sink == 0x123456789abcdefull)
		printf("unreachable %lu\n", sink);
	return 0;
}
