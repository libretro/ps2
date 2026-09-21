/* The GTE, C against the C++ it was converted from, in one process.
 *
 * Both objects are linked in together, the old one with its symbols
 * prefixed, so the two run alternately within each trial and the best time
 * for each is kept. On a shared single-core container that is the only
 * comparison worth making: two separate runs measure the machine's mood as
 * much as the code.
 *
 * The work is the same on both sides -- the same op sequence over the same
 * seeded register file -- so the times are comparable. Each side's FLAG
 * word is accumulated and printed; a difference there means the two are not
 * doing the same arithmetic and the timing means nothing.
 *
 * What this exits on is the flag word, not the clock. Wall-clock here
 * resolves to about two percent and longer runs are noisier rather than
 * quieter, so a timing gate would flap without saying anything. The times
 * are printed to be read; build.sh gates on the memory-op count beside
 * them, which is deterministic and is what tracks wall-clock in this
 * tree.
 *
 * Built by tests/iop/build.sh --bench.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "R3000A.h"
#include "IopMem.h"
#include "IopGte.h"

/* The converted unit's state, and the pre-conversion one's under its
 * prefix. Both declare the same object; objcopy renamed one of them. */
extern "C" {
void gteRTPS(void); void gteRTPT(void); void gteMVMVA(void);
void gteNCDS(void); void gteNCDT(void); void gteNCCS(void);
void gteNCCT(void); void gteNCS(void);  void gteNCT(void);
void gteCC(void);   void gteCDP(void);  void gteDCPL(void);
void gteDPCS(void); void gteDPCT(void); void gteINTPL(void);
void gteSQR(void);  void gteOP(void);   void gteGPF(void);
void gteGPL(void);  void gteNCLIP(void);
void gteAVSZ3(void); void gteAVSZ4(void);
}

extern psxRegisters old_psxRegs;
extern "C" {
void old_gteRTPS(void); void old_gteRTPT(void); void old_gteMVMVA(void);
void old_gteNCDS(void); void old_gteNCDT(void); void old_gteNCCS(void);
void old_gteNCCT(void); void old_gteNCS(void);  void old_gteNCT(void);
void old_gteCC(void);   void old_gteCDP(void);  void old_gteDCPL(void);
void old_gteDPCS(void); void old_gteDPCT(void); void old_gteINTPL(void);
void old_gteSQR(void);  void old_gteOP(void);   void old_gteGPF(void);
void old_gteGPL(void);  void old_gteNCLIP(void);
void old_gteAVSZ3(void); void old_gteAVSZ4(void);
}

typedef void (*gte_fn)(void);

static gte_fn new_ops[] = {
	gteRTPS, gteRTPT, gteMVMVA, gteNCDS, gteNCDT, gteNCCS, gteNCCT,
	gteNCS, gteNCT, gteCC, gteCDP, gteDCPL, gteDPCS, gteDPCT, gteINTPL,
	gteSQR, gteOP, gteGPF, gteGPL, gteNCLIP, gteAVSZ3, gteAVSZ4
};
static gte_fn old_ops[] = {
	old_gteRTPS, old_gteRTPT, old_gteMVMVA, old_gteNCDS, old_gteNCDT,
	old_gteNCCS, old_gteNCCT, old_gteNCS, old_gteNCT, old_gteCC,
	old_gteCDP, old_gteDCPL, old_gteDPCS, old_gteDPCT, old_gteINTPL,
	old_gteSQR, old_gteOP, old_gteGPF, old_gteGPL, old_gteNCLIP,
	old_gteAVSZ3, old_gteAVSZ4
};

enum { NOPS = sizeof(new_ops) / sizeof(new_ops[0]) };

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* A register file that exercises the saturating paths rather than sitting
 * in the middle of every range. */
static void seed(psxRegisters *r, unsigned s)
{
	unsigned i;
	for (i = 0; i < 32; i++)
	{
		s ^= s << 13; s ^= s >> 17; s ^= s << 5;
		r->CP2D.r[i] = s;
		s ^= s << 13; s ^= s >> 17; s ^= s << 5;
		r->CP2C.r[i] = s;
	}
	r->code = 0;
}

static u32 run(psxRegisters *regs, gte_fn *ops, int reps, double *secs)
{
	u32 flagsum = 0;
	double t0;
	int r, i;

	seed(regs, 0xc0ffeeu);
	t0 = now();
	for (r = 0; r < reps; r++)
	{
		for (i = 0; i < NOPS; i++)
		{
			regs->code = (u32)(0x4a000000u | (unsigned)i);
			ops[i]();
			flagsum += regs->CP2C.r[31];
		}
	}
	*secs = now() - t0;
	return flagsum;
}

int main(int argc, char **argv)
{
	int trials = argc > 1 ? atoi(argv[1]) : 15;
	int reps   = argc > 2 ? atoi(argv[2]) : 40000;
	double bo = 1e9, bn = 1e9, t;
	u32 fo = 0, fn = 0;
	int i;

	for (i = 0; i < trials; i++)
	{
		fo = run(&old_psxRegs, old_ops, reps, &t); if (t < bo) bo = t;
		fn = run(&psxRegs,     new_ops, reps, &t); if (t < bn) bn = t;
	}

	printf("ops         : %d x %d\n", NOPS, reps);
	printf("flag accum  : C++ %08x  C %08x  %s\n", fo, fn,
	       fo == fn ? "identical" : "DIFFERENT -- timing is meaningless");
	printf("C++         : %.4f s\n", bo);
	printf("C           : %.4f s\n", bn);
	printf("ratio       : %.3fx (+-2%% on this machine, so read it as a band)\n",
	       bo / bn);
	printf("%s: the two agree on every flag word\n", fo == fn ? "PASS" : "FAIL");
	return fo == fn ? 0 : 1;
}
