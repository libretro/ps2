/* Cycle cost of the three add/sub emissions, measured on the emitted
 * code itself: the off path (clamp both operands, addps), the old on
 * path (fused mask, addps), and the shipping on path (fused mask, exact
 * double-bit stage). Each is emitted through the C89 emitter into a
 * loop that runs the sequence back to back on register-resident data,
 * so the number is the sequence's throughput cost, the quantity that
 * scales with VU op density. Latency chains and memory traffic in real
 * blocks blur these numbers toward each other, so treat the ratios as
 * the ceiling of the difference, not the floor. */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include "common/Pcsx2Defs.h"
u8* x86Ptr;
#include "emitter/c89ops.h"

typedef uint32_t u32;

#define G4(n, v) alignas(16) static u32 n[4] = {v, v, v, v}
G4(glob_signbit, 0x80000000u);
G4(glob_kff,     0x000000ffu);
G4(glob_zero,    0x00000000u);
G4(glob_mantm,   0x007fffffu);
G4(glob_k896,    0x00000380u);
G4(glob_dblhm,   0x000fffffu);
G4(glob_k7ff,    0x000007ffu);
G4(glob_one,     0x00000001u);
G4(glob_absclip, 0x7fffffffu);
G4(glob_exponent,0x7f800000u);
G4(glob_addm24,  0x00000018u);
G4(glob_addmall, 0xffffffffu);
G4(glob_maxvals, 0x7f7fffffu);
G4(glob_minvals, 0xff7fffffu);

alignas(16) static u32 io_a[4] = {0x3f800000u, 0x40490fdbu, 0xc2c80000u, 0x00800001u};
alignas(16) static u32 io_b[4] = {0x3f000000u, 0xc0490fdbu, 0x42c80000u, 0x80800002u};
alignas(16) static u32 io_r[4];

typedef void (*bench_fn)(void);

enum { PATH_OFF, PATH_MASK_RN, PATH_MASK_EXACT };

static void emit_exact_stage(void)
{
	/* X=0 Y=1 tA=2 tB=3 tC=4 tQ=5 tE=6 tZ=7 -- the shipping stage. */
	xe_movaps_xx(6, 0); xe_psrld_xi(6, 23); xe_pand_xm(6, glob_kff);
	xe_movaps_xx(7, 6); xe_pcmpeqd_xm(7, glob_zero);
	xe_movaps_xx(3, 0); xe_pand_xm(3, glob_mantm); xe_psrld_xi(3, 3);
	xe_paddd_xm(6, glob_k896); xe_pslld_xi(6, 20); xe_por_xx(3, 6);
	xe_movaps_xx(2, 0); xe_pslld_xi(2, 29);
	xe_movaps_xx(6, 7); xe_pandn_xx(6, 3); xe_movaps_xx(3, 6);
	xe_movaps_xx(6, 0); xe_pand_xm(6, glob_signbit); xe_por_xx(3, 6);
	xe_movaps_xx(6, 7); xe_pandn_xx(6, 2); xe_movaps_xx(2, 6);
	xe_movaps_xx(4, 2);
	xe_punpckldq_xx(2, 3);
	xe_punpckhdq_xx(4, 3);
	xe_movaps_xx(6, 1); xe_psrld_xi(6, 23); xe_pand_xm(6, glob_kff);
	xe_movaps_xx(7, 6); xe_pcmpeqd_xm(7, glob_zero);
	xe_movaps_xx(5, 1); xe_pand_xm(5, glob_mantm); xe_psrld_xi(5, 3);
	xe_paddd_xm(6, glob_k896); xe_pslld_xi(6, 20); xe_por_xx(5, 6);
	xe_movaps_xx(3, 1); xe_pslld_xi(3, 29);
	xe_movaps_xx(6, 7); xe_pandn_xx(6, 5); xe_movaps_xx(5, 6);
	xe_movaps_xx(6, 1); xe_pand_xm(6, glob_signbit); xe_por_xx(5, 6);
	xe_movaps_xx(6, 7); xe_pandn_xx(6, 3); xe_movaps_xx(3, 6);
	xe_movaps_xx(6, 3);
	xe_punpckldq_xx(3, 5);
	xe_punpckhdq_xx(6, 5);
	xe_addpd_xx(2, 3);
	xe_addpd_xx(4, 6);
	xe_movaps_xx(3, 2);
	xe_shufps_xxi(3, 4, 0xDD);
	xe_shufps_xxi(2, 4, 0x88);
	xe_movaps_xx(4, 3); xe_pand_xm(4, glob_signbit);
	xe_movaps_xx(5, 3); xe_psrld_xi(5, 20); xe_pand_xm(5, glob_k7ff);
	xe_psubd_xm(5, glob_k896);
	xe_pand_xm(3, glob_dblhm); xe_pslld_xi(3, 3);
	xe_psrld_xi(2, 29); xe_por_xx(3, 2);
	xe_movaps_xx(2, 5); xe_pslld_xi(2, 23); xe_por_xx(2, 3); xe_por_xx(2, 4);
	xe_movaps_xm(6, glob_one); xe_pcmpgtd_xx(6, 5);
	xe_movaps_xx(3, 5); xe_pcmpgtd_xm(3, glob_kff);
	xe_movaps_xx(7, 6); xe_pandn_xx(7, 2);
	xe_pand_xx(6, 4); xe_por_xx(7, 6);
	xe_movaps_xx(6, 4); xe_por_xm(6, glob_absclip); xe_pand_xx(6, 3);
	xe_pandn_xx(3, 7); xe_por_xx(3, 6);
	xe_movaps_xx(0, 3);
}

static void emit_fused_mask(void)
{
	/* a=0 b=1 tD=2 tM=3 tS=4 -- transcribed from mVUmaskAddSubFused. */
	xe_movaps_xx(2, 0);
	xe_pand_xm(2, glob_exponent);
	xe_psrld_xi(2, 23);
	xe_movaps_xx(4, 1);
	xe_pand_xm(4, glob_exponent);
	xe_psrld_xi(4, 23);
	xe_psubd_xx(2, 4);
	xe_pabsd_xx(4, 2);
	xe_psubd_xm(4, glob_one);
	xe_movaps_xm(3, glob_addm24);
	xe_pcmpgtd_xx(3, 4);
	xe_por_xm(3, glob_signbit);
	xe_pslld_xi(4, 23);
	xe_paddd_xm(4, glob_one); /* one here is the integer 1; the tree's
	                             mVUglob.one is 0x3f800000 -- the float
	                             1.0 the cvttps2dq trick needs. Bench
	                             uses the float value below. */
	xe_cvttps2dq_xx(4, 4);
	xe_psubd_xm(4, glob_one);
	xe_pxor_xm(4, glob_addmall);
	xe_por_xm(4, glob_signbit);
	xe_pand_xx(3, 4);
	xe_movaps_xx(4, 2);
	xe_pcmpgtd_xm(4, glob_addmall);
	xe_por_xx(4, 3);
	xe_pand_xx(0, 4);
	xe_movaps_xx(4, 3);
	xe_movaps_xx(3, 2);
	xe_movaps_xm(2, glob_one);
	xe_pcmpgtd_xx(2, 3);
	xe_por_xx(2, 4);
	xe_movaps_xx(3, 1);
	xe_pand_xx(3, 2);
	xe_movaps_xx(2, 3);
}

static void emit_clamp(int reg)
{
	xe_minss_xm(reg, glob_maxvals);
	xe_maxss_xm(reg, glob_minvals);
}

static bench_fn emit_bench(uint8_t* buf, int path, int inner)
{
	int i;
	x86Ptr = (u8*)buf;
	xe_movaps_xm(0, io_a);
	xe_movaps_xm(1, io_b);
	for (i = 0; i < inner; i++)
	{
		switch (path)
		{
			case PATH_OFF:
				emit_clamp(0); emit_clamp(1);
				xe_addps_xx(0, 1);
				break;
			case PATH_MASK_RN:
				emit_fused_mask();
				xe_addps_xx(0, 2);
				break;
			case PATH_MASK_EXACT:
				emit_fused_mask();
				xe_movaps_xx(1, 2);
				emit_exact_stage();
				break;
		}
		/* feed the result back so the chain is dependent like real code */
		xe_pand_xm(0, glob_absclip);
		xe_movaps_xm(1, io_b);
	}
	xe_movaps_mx(io_r, 0);
	xe_ret();
	return (bench_fn)buf;
}

static double bench(bench_fn f, long calls)
{
	struct timespec t0, t1;
	long c;
	f(); /* warm */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (c = 0; c < calls; c++) f();
	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
}

int main(void)
{
	const int   inner = 256;
	const long  calls = 200000; /* 51.2M sequence executions per path */
	const char* names[3] = {"off  (clamp+addps)   ", "old  (mask+RN addps) ", "new  (mask+exact)    "};
	double secs[3];
	int p;
	uint8_t* buf = (uint8_t*)mmap(0, 1 << 20, PROT_READ|PROT_WRITE|PROT_EXEC,
	                              MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT, -1, 0);
	if (buf == MAP_FAILED) { fprintf(stderr, "mmap failed\n"); return 1; }

	/* the float-1.0 constant the mask's cvttps2dq trick needs */
	glob_one[0] = glob_one[1] = glob_one[2] = glob_one[3] = 1;

	for (p = 0; p < 3; p++)
	{
		bench_fn f = emit_bench(buf + (p << 16), p, inner);
		secs[p] = bench(f, calls);
	}
	printf("throughput, %ld x %d dependent sequence executions per path:\n",
	       calls, inner);
	for (p = 0; p < 3; p++)
		printf("  %s %7.3f s   %6.2f ns/op   x%.2f vs off\n",
		       names[p], secs[p], secs[p] / (double)(calls * (long)inner) * 1e9,
		       secs[p] / secs[0]);
	return 0;
}
