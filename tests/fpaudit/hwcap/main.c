/* fpaudit hardware captures -- one ELF, three tests, hex over printf.
 *
 * Build (stock PS2SDK):   make
 * Run:                    ps2client execee host:hwcap.elf   (or uLE/USB)
 * Deliver:                paste every [HW] line back verbatim.
 *
 * Test 1: EE CFC1 readout construction after raw CTC1 stores.
 * Test 2: VU0-macro fused MADD/MSUB overflow rule (COP2).
 * Test 3: VU1 EFU truth vectors (EFU exists on VU1 only) -- a tiny
 *         micro is uploaded through VU1 mem, run per input, and P is
 *         read back from the VU1 register file mapping.
 *
 * Decision tables for every outcome: ../HARDWARE-CAPTURES.txt
 */

#include <tamtypes.h>
#include <kernel.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Test 1: CFC1 round-trip                                          */
/* ---------------------------------------------------------------- */
static u32 cfc1_roundtrip(u32 p)
{
	u32 out;
	__asm__ volatile(
		"ctc1 %1, $31\n\t"
		"nop\n\tnop\n\tnop\n\tnop\n\t"
		"cfc1 %0, $31\n\t"
		: "=r"(out) : "r"(p));
	return out;
}

static void test1(void)
{
	static const u32 pats[6] = {
		0x00000000, 0x00800000, 0x0001FFFF,
		0xFFFFFFFF, 0x01000001, 0x0083C078
	};
	int i;
	for (i = 0; i < 6; i++)
		printf("[HW] T1 ctc1=%08x cfc1=%08x\n",
		       pats[i], cfc1_roundtrip(pats[i]));
}

/* ---------------------------------------------------------------- */
/* Test 2: fused MADD/MSUB on VU0 macro (COP2)                      */
/* Seeds VF1/VF2/ACC through QMTC1-class moves, runs the fused op,  */
/* reads VF3.x back. ACC is seeded via MULAw ACC,VF4,VF0 (VF0.w=1). */
/* ---------------------------------------------------------------- */
static u32 vu0_fused(u32 fs, u32 ft, u32 acc, int is_msub)
{
	u32 out;
	u128 vfs, vft, vacc;
	u32* p;
	p = (u32*)&vfs;  p[0]=p[1]=p[2]=p[3]=fs;
	p = (u32*)&vft;  p[0]=p[1]=p[2]=p[3]=ft;
	p = (u32*)&vacc; p[0]=p[1]=p[2]=p[3]=acc;
	__asm__ volatile(
		"lqc2  $vf1, 0(%1)\n\t"
		"lqc2  $vf2, 0(%2)\n\t"
		"lqc2  $vf4, 0(%3)\n\t"
		"vmulaw.xyzw $ACC, $vf4, $vf0\n\t"   /* ACC := VF4 * 1.0 */
		"vnop\n\tvnop\n\tvnop\n\tvnop\n\t"
		"beqz  %4, 1f\n\tnop\n\t"
		"vmsub.xyzw $vf3, $vf1, $vf2\n\t"
		"b 2f\n\tnop\n\t"
		"1:\n\t"
		"vmadd.xyzw $vf3, $vf1, $vf2\n\t"
		"2:\n\t"
		"vnop\n\tvnop\n\tvnop\n\tvnop\n\t"
		"qmfc2 %0, $vf3\n\t"
		: "=r"(out)
		: "r"(&vfs), "r"(&vft), "r"(&vacc), "r"(is_msub)
		: "memory");
	return out; /* lane x */
}

static void test2(void)
{
	/* Case A: FMAX*FMAX with ACC=-FMAX */
	printf("[HW] T2A madd fs=7F7FFFFF ft=7F7FFFFF acc=FF7FFFFF -> %08x (model 7FFFFFFF)\n",
	       vu0_fused(0x7F7FFFFF, 0x7F7FFFFF, 0xFF7FFFFF, 0));
	printf("[HW] T2A msub fs=7F7FFFFF ft=7F7FFFFF acc=FF7FFFFF -> %08x (model FFFFFFFF)\n",
	       vu0_fused(0x7F7FFFFF, 0x7F7FFFFF, 0xFF7FFFFF, 1));
	/* Case B: exp-255 input, computed mantissa expected, no all-ones */
	printf("[HW] T2B madd fs=7F800000 ft=3F800000 acc=00000000 -> %08x (model 7F800000-class)\n",
	       vu0_fused(0x7F800000, 0x3F800000, 0x00000000, 0));
}

/* ---------------------------------------------------------------- */
/* Test 3: VU1 EFU truth vectors                                    */
/* Micro shape per op: [LNOP | EFUOP VF01] pads, [LNOP|NOP+E], stop.*/
/* Inputs land in VF01 via VU1 regs writes while VU1 is stopped     */
/* (0x1100A000 register file window: VF01 at +0x10); P is read from */
/* the control area after the run and WAITP-equivalent settling.    */
/* ---------------------------------------------------------------- */
#define VU1_MICRO ((volatile u32*)0x11008000)
#define VU1_VF    ((volatile u32*)0x1100A000)
#define VU1_VI    ((volatile u32*)0x1100A400)   /* control regs area  */
#define VU1_P_IDX (26*4)                        /* P mirror slot; if
	your mapping differs, read P via a second micro that does
	WAITP; MFP VF03,P; E and dump VF03 instead -- both paths
	are in run_efu below, mfp path is the default. */

static void vu1_upload_and_run(const u32* pair_words, int npairs,
                               const u32 in[4], u32 out_vf03[4])
{
	int i;
	/* stop VU1, upload */
	__asm__ volatile("cfc2.i $0, $28" ::: "memory"); /* sync */
	for (i = 0; i < npairs * 2; i++)
		VU1_MICRO[i] = pair_words[i];
	/* seed VF01 */
	for (i = 0; i < 4; i++)
		VU1_VF[4 + i] = in[i];
	FlushCache(0);
	/* kick: VIF path is sturdier, but the direct CTC2 start works
	 * on a stopped VU1: write TPC=0 and set VPU_STAT VBS1 via
	 * ctc2 to CMSAR1 */
	__asm__ volatile(
		"li   $8, 0\n\t"
		"ctc2 $8, $31\n\t"      /* CMSAR1 = 0 -> starts VU1 */
		::: "$8");
	/* wait for E-bit stop */
	{
		u32 stat;
		do { __asm__ volatile("cfc2 %0, $29" : "=r"(stat)); }
		while (stat & 0x100);   /* VBS1 */
	}
	for (i = 0; i < 4; i++)
		out_vf03[i] = VU1_VF[3*4 + i];
}

/* lower-word EFU encodings (fsf=x, ftf ignored), from the census:
 * T3 tables: op = (0x40<<25)|(fsf<<21)|(idx<<6)|(0x3C|tbl)          */
#define EFU(tbl, idx) ((0x40u<<25)|(0u<<21)|(1u<<11)|((idx)<<6)|(0x3Cu|(tbl)))
#define LNOP 0x8000033Cu
#define UNOP 0x000002FFu
#define WAITP 0x800007BFu   /* if your assembler differs, see manual */
#define MFP_VF03 ((0x40u<<25)|(3u<<16)|(0x19u<<6)|0x3Cu) /* MFP.x vf3,P -- adjust dest/tbl to taste */

struct efuop { const char* name; u32 word; };
static const struct efuop EFUOPS[] = {
	{"ESADD",  EFU(1,28)}, {"ERSADD", EFU(2,28)}, {"ELENG", EFU(3,28)},
	{"ERLENG", EFU(0,29)}, {"ESUM",   EFU(2,29)},
	{"EATAN",  EFU(1,31)}, {"EATANxy",EFU(0,28)}, {"EATANxz",EFU(1,29)},
	{"ERSQRT", EFU(2,30)}, {"ERCPR",  EFU(3,30)}, {"ESIN",   EFU(0,31)},
	{"EEXP",   EFU(2,31)}, {"ESQRT",  EFU(1,30)},
};

static const u32 VECS[8][4] = {
	{0x3F800000,0x40000000,0x40400000,0x3F800000},
	{0x7F7FFFFF,0x3F800000,0x00800000,0x3F800000},
	{0xBF800000,0xC0000000,0x40400000,0x3F800000},
	{0x00000001,0x00000001,0x00000001,0x3F800000},
	{0x5F000000,0x5F000000,0x5F000000,0x3F800000},
	{0x322BCC77,0xB22BCC77,0x40490FDB,0x3F800000},
	{0x7F7FFFFF,0xFF7FFFFF,0x7F7FFFFF,0x3F800000},
	{0x3DCCCCCD,0x3E4CCCCD,0x3E99999A,0x3F800000},
};

static void test3(void)
{
	u32 prog[64], out[4];
	unsigned op, v, i;
	for (op = 0; op < sizeof(EFUOPS)/sizeof(EFUOPS[0]); op++)
	{
		i = 0;
		prog[i++] = EFUOPS[op].word; prog[i++] = UNOP;
		/* generous stall pad, then read P into VF03 and stop */
		while (i < 40) { prog[i++] = LNOP; prog[i++] = UNOP; }
		prog[i++] = WAITP;    prog[i++] = UNOP;
		prog[i++] = MFP_VF03; prog[i++] = UNOP;
		prog[i++] = LNOP;     prog[i++] = UNOP | (1u<<30); /* E */
		prog[i++] = LNOP;     prog[i++] = UNOP;
		for (v = 0; v < 8; v++)
		{
			vu1_upload_and_run(prog, i/2, VECS[v], out);
			printf("[HW] T3 %-8s v%u P=%08x\n",
			       EFUOPS[op].name, v+1, out[0]);
		}
	}
}

int main(void)
{
	printf("[HW] fpaudit hardware captures\n");
	test1();
	test2();
	test3();
	printf("[HW] done -- paste every [HW] line back\n");
	SleepThread();
	return 0;
}
