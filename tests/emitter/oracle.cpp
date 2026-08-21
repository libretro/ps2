/* Differential oracle: for every (instruction, operand) combination in the
 * matrix, emit with the real C++ emitter and with the C89 core AT THE SAME
 * ADDRESS, then compare bytes. Same address matters because RIP-relative
 * displacements depend on the emit cursor.
 *
 * Any divergence is a bug in the C89 core, by definition -- the C++ emitter
 * is the reference. Exit status is non-zero if anything diverges.
 */
#include "common/emitter/x86emitter.h"
#include "tests/emitter/reference/legacy_internal.h"
extern "C" {
#include "common/emitter/c89emit.h"
}
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <sys/mman.h>

using namespace x86Emitter;

static u8* g_buf;
static long g_cases = 0, g_fail = 0;
static std::vector<std::string> g_failures;

/* Emit one case both ways at the same address and compare. */
template <typename CppFn, typename C89Fn>
static void check(const char* what, CppFn cpp, C89Fn c89)
{
	u8 a[32], b[32];
	size_t na, nb;

	x86Ptr = (u8*)(g_buf);
	cpp();
	na = (size_t)(x86Ptr - g_buf);
	memcpy(a, g_buf, na);

	memset(g_buf, 0xcc, 32);
	{
		uint8_t* p = (uint8_t*)g_buf;
		p = c89(p);
		nb = (size_t)((u8*)p - g_buf);
		memcpy(b, g_buf, nb);
	}

	g_cases++;
	if (na != nb || memcmp(a, b, na) != 0)
	{
		g_fail++;
		if (g_failures.size() < 25)
		{
			char line[512];
			int o = snprintf(line, sizeof(line), "%-40s cpp[%zu]:", what, na);
			for (size_t i = 0; i < na && o < 300; i++)
				o += snprintf(line + o, sizeof(line) - o, " %02x", a[i]);
			o += snprintf(line + o, sizeof(line) - o, "  c89[%zu]:", nb);
			for (size_t i = 0; i < nb && o < 480; i++)
				o += snprintf(line + o, sizeof(line) - o, " %02x", b[i]);
			g_failures.push_back(line);
		}
	}
}

int main()
{
	/* Fixed address so RIP deltas are reproducible. */
	g_buf = (u8*)mmap((void*)0x200000000ull, 1 << 20, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (g_buf == MAP_FAILED) { perror("mmap"); return 2; }

	/* Address operands: near the emit buffer (RIP-reachable) and far from it
	 * (forces the absolute SIB form), plus unaligned and negative-delta cases. */
	static u64 nearmem[8];
	const void* addrs[] = {
		(const void*)0x200001000ull,          /* just after the buffer: RIP-reachable */
		(const void*)0x1fffff000ull,          /* just before: negative RIP delta */
		(const void*)0x000000000040f000ull,   /* low, > 2GB away: absolute SIB */
		(const void*)0x00007fff00000000ull,   /* high canonical: absolute SIB */
		(const void*)nearmem,
		(const void*)0x200000004ull,          /* tiny positive delta */
	};
	const int naddr = (int)(sizeof(addrs) / sizeof(addrs[0]));

	/* Immediates spanning the s8/imm32 boundary in both directions. */
	const s32 imms[] = { 0, 1, 0x7f, 0x80, -1, -0x80, -0x81, 0x100, 0x1fffff, (s32)0x7fffffff };
	const int nimm = (int)(sizeof(imms) / sizeof(imms[0]));

	char nm[128];

	/* ---- mov r32/r64, [abs]  and  mov [abs], r32/r64 ---- */
	for (int w = 0; w <= 1; w++)
		for (int r = 0; r < 16; r++)
			for (int a = 0; a < naddr; a++)
			{
				const void* ad = addrs[a];
				snprintf(nm, sizeof(nm), "MOV_R_M w%d r%d a%d", w, r, a);
				check(nm,
					[&]{ if (w) xMOV(xRegister64(r), ptrNative[ad]);
					     else    xMOV(xRegister32(r), ptr32[ad]); },
					[&](uint8_t* p){ E_MOV_R_M(p, w, r, ad); return p; });

				snprintf(nm, sizeof(nm), "MOV_M_R w%d r%d a%d", w, r, a);
				check(nm,
					[&]{ if (w) xMOV(ptrNative[ad], xRegister64(r));
					     else    xMOV(ptr32[ad], xRegister32(r)); },
					[&](uint8_t* p){ E_MOV_M_R(p, w, r, ad); return p; });
			}

	/* ---- group1 reg,reg ---- */
	struct G1 { int op; const char* n; };
	const G1 g1[] = { {0,"ADD"}, {1,"OR"}, {4,"AND"}, {5,"SUB"}, {6,"XOR"}, {7,"CMP"} };
	for (int gi = 0; gi < 6; gi++)
		for (int w = 0; w <= 1; w++)
			for (int d = 0; d < 16; d++)
				for (int s = 0; s < 16; s++)
				{
					const int op = g1[gi].op;
					snprintf(nm, sizeof(nm), "%s_RR w%d d%d s%d", g1[gi].n, w, d, s);
					check(nm,
						[&]{
							if (w) {
								switch (op) {
								case 0: xADD(xRegister64(d), xRegister64(s)); break;
								case 1: xOR (xRegister64(d), xRegister64(s)); break;
								case 4: xAND(xRegister64(d), xRegister64(s)); break;
								case 5: xSUB(xRegister64(d), xRegister64(s)); break;
								case 6: xXOR(xRegister64(d), xRegister64(s)); break;
								default:xCMP(xRegister64(d), xRegister64(s)); break; }
							} else {
								switch (op) {
								case 0: xADD(xRegister32(d), xRegister32(s)); break;
								case 1: xOR (xRegister32(d), xRegister32(s)); break;
								case 4: xAND(xRegister32(d), xRegister32(s)); break;
								case 5: xSUB(xRegister32(d), xRegister32(s)); break;
								case 6: xXOR(xRegister32(d), xRegister32(s)); break;
								default:xCMP(xRegister32(d), xRegister32(s)); break; }
							} },
						[&](uint8_t* p){ E_G1_RR(p, w, op, d, s); return p; });
				}

	/* ---- group1 reg, imm ---- */
	for (int gi = 0; gi < 6; gi++)
		for (int w = 0; w <= 1; w++)
			for (int r = 0; r < 16; r++)
				for (int ii = 0; ii < nimm; ii++)
				{
					const int op = g1[gi].op; const s32 im = imms[ii];
					snprintf(nm, sizeof(nm), "%s_RI w%d r%d i%d", g1[gi].n, w, r, ii);
					check(nm,
						[&]{
							if (w) {
								switch (op) {
								case 0: xADD(xRegister64(r), im); break;
								case 1: xOR (xRegister64(r), im); break;
								case 4: xAND(xRegister64(r), im); break;
								case 5: xSUB(xRegister64(r), im); break;
								case 6: xXOR(xRegister64(r), im); break;
								default:xCMP(xRegister64(r), im); break; }
							} else {
								switch (op) {
								case 0: xADD(xRegister32(r), im); break;
								case 1: xOR (xRegister32(r), im); break;
								case 4: xAND(xRegister32(r), im); break;
								case 5: xSUB(xRegister32(r), im); break;
								case 6: xXOR(xRegister32(r), im); break;
								default:xCMP(xRegister32(r), im); break; }
							} },
						[&](uint8_t* p){ E_G1_RI(p, w, op, r, im); return p; });
				}

	/* ---- group1 dword [abs], imm ---- */
	for (int gi = 0; gi < 6; gi++)
		for (int a = 0; a < naddr; a++)
			for (int ii = 0; ii < nimm; ii++)
			{
				const int op = g1[gi].op; const s32 im = imms[ii]; const void* ad = addrs[a];
				snprintf(nm, sizeof(nm), "%s_MI a%d i%d", g1[gi].n, a, ii);
				check(nm,
					[&]{
						switch (op) {
						case 0: xADD(ptr32[ad], im); break;
						case 1: xOR (ptr32[ad], im); break;
						case 4: xAND(ptr32[ad], im); break;
						case 5: xSUB(ptr32[ad], im); break;
						case 6: xXOR(ptr32[ad], im); break;
						default:xCMP(ptr32[ad], im); break; } },
					[&](uint8_t* p){ E_G1_MI(p, op, ad, im); return p; });
			}

	/* ---- mov dword [abs], imm32 ---- */
	for (int a = 0; a < naddr; a++)
		for (int ii = 0; ii < nimm; ii++)
		{
			const void* ad = addrs[a]; const s32 im = imms[ii];
			snprintf(nm, sizeof(nm), "MOV_M_I a%d i%d", a, ii);
			check(nm,
				[&]{ xMOV(ptr32[ad], im); },
				[&](uint8_t* p){ E_MOV_M_I(p, ad, im); return p; });
		}

	/* ---- shl r32, imm8 ---- */
	for (int r = 0; r < 16; r++)
		for (int sh = 1; sh < 32; sh++)
		{
			snprintf(nm, sizeof(nm), "SHL_RI r%d s%d", r, sh);
			check(nm,
				[&]{ xSHL(xRegister32(r), sh); },
				[&](uint8_t* p){ E_SHL_RI(p, r, sh); return p; });
		}

	/* ---- test r32, r32 ---- */
	for (int d = 0; d < 16; d++)
		for (int s = 0; s < 16; s++)
		{
			snprintf(nm, sizeof(nm), "TEST_RR d%d s%d", d, s);
			check(nm,
				[&]{ xTEST(xRegister32(d), xRegister32(s)); },
				[&](uint8_t* p){ E_TEST_RR(p, d, s); return p; });
		}

	/* ---- movss xmm,[abs] / [abs],xmm ---- */
	for (int r = 0; r < 16; r++)
		for (int a = 0; a < naddr; a++)
		{
			const void* ad = addrs[a];
			snprintf(nm, sizeof(nm), "MOVSS_R_M r%d a%d", r, a);
			check(nm,
				[&]{ xMOVSSZX(xRegisterSSE(r), ptr32[ad]); },
				[&](uint8_t* p){ E_MOVSS_R_M(p, r, ad); return p; });
			snprintf(nm, sizeof(nm), "MOVSS_M_R r%d a%d", r, a);
			check(nm,
				[&]{ xMOVSS(ptr32[ad], xRegisterSSE(r)); },
				[&](uint8_t* p){ E_MOVSS_M_R(p, r, ad); return p; });
		}


	/* ================= batch 2 ================= */

	/* group1 r,[abs] and [abs],r */
	for (int gi = 0; gi < 6; gi++)
		for (int w = 0; w <= 1; w++)
			for (int r = 0; r < 16; r++)
				for (int a = 0; a < naddr; a++)
				{
					const int op = g1[gi].op; const void* ad = addrs[a];
					snprintf(nm, sizeof(nm), "%s_RM w%d r%d a%d", g1[gi].n, w, r, a);
					check(nm,
						[&]{ if (w) { switch(op){
							case 0: xADD(xRegister64(r), ptrNative[ad]); break;
							case 1: xOR (xRegister64(r), ptrNative[ad]); break;
							case 4: xAND(xRegister64(r), ptrNative[ad]); break;
							case 5: xSUB(xRegister64(r), ptrNative[ad]); break;
							case 6: xXOR(xRegister64(r), ptrNative[ad]); break;
							default:xCMP(xRegister64(r), ptrNative[ad]); break; } }
						     else { switch(op){
							case 0: xADD(xRegister32(r), ptr32[ad]); break;
							case 1: xOR (xRegister32(r), ptr32[ad]); break;
							case 4: xAND(xRegister32(r), ptr32[ad]); break;
							case 5: xSUB(xRegister32(r), ptr32[ad]); break;
							case 6: xXOR(xRegister32(r), ptr32[ad]); break;
							default:xCMP(xRegister32(r), ptr32[ad]); break; } } },
						[&](uint8_t* p){ E_G1_RM(p, w, op, r, ad); return p; });

					snprintf(nm, sizeof(nm), "%s_MR w%d r%d a%d", g1[gi].n, w, r, a);
					check(nm,
						[&]{ if (w) { switch(op){
							case 0: xADD(ptrNative[ad], xRegister64(r)); break;
							case 1: xOR (ptrNative[ad], xRegister64(r)); break;
							case 4: xAND(ptrNative[ad], xRegister64(r)); break;
							case 5: xSUB(ptrNative[ad], xRegister64(r)); break;
							case 6: xXOR(ptrNative[ad], xRegister64(r)); break;
							default:xCMP(ptrNative[ad], xRegister64(r)); break; } }
						     else { switch(op){
							case 0: xADD(ptr32[ad], xRegister32(r)); break;
							case 1: xOR (ptr32[ad], xRegister32(r)); break;
							case 4: xAND(ptr32[ad], xRegister32(r)); break;
							case 5: xSUB(ptr32[ad], xRegister32(r)); break;
							case 6: xXOR(ptr32[ad], xRegister32(r)); break;
							default:xCMP(ptr32[ad], xRegister32(r)); break; } } },
						[&](uint8_t* p){ E_G1_MR(p, w, op, r, ad); return p; });
				}

	/* group2 shifts: reg,imm and reg,cl */
	{
		struct G2 { int g; const char* n; };
		const G2 g2t[] = { {0,"ROL"},{1,"ROR"},{4,"SHL"},{5,"SHR"},{7,"SAR"} };
		for (int gi = 0; gi < 5; gi++)
			for (int w = 0; w <= 1; w++)
				for (int r = 0; r < 16; r++)
				{
					for (int sh = 0; sh < 33; sh++)
					{
						const int g = g2t[gi].g;
						snprintf(nm, sizeof(nm), "%s_RI w%d r%d s%d", g2t[gi].n, w, r, sh);
						check(nm,
							[&]{ if (w) { switch(g){
								case 0: xROL(xRegister64(r), sh); break;
								case 1: xROR(xRegister64(r), sh); break;
								case 4: xSHL(xRegister64(r), sh); break;
								case 5: xSHR(xRegister64(r), sh); break;
								default:xSAR(xRegister64(r), sh); break; } }
							     else { switch(g){
								case 0: xROL(xRegister32(r), sh); break;
								case 1: xROR(xRegister32(r), sh); break;
								case 4: xSHL(xRegister32(r), sh); break;
								case 5: xSHR(xRegister32(r), sh); break;
								default:xSAR(xRegister32(r), sh); break; } } },
							[&](uint8_t* p){ E_G2_RI(p, w, g, r, sh); return p; });
					}
					{
						const int g = g2t[gi].g;
						snprintf(nm, sizeof(nm), "%s_RCL w%d r%d", g2t[gi].n, w, r);
						check(nm,
							[&]{ if (w) { switch(g){
								case 0: xROL(xRegister64(r), cl); break;
								case 1: xROR(xRegister64(r), cl); break;
								case 4: xSHL(xRegister64(r), cl); break;
								case 5: xSHR(xRegister64(r), cl); break;
								default:xSAR(xRegister64(r), cl); break; } }
							     else { switch(g){
								case 0: xROL(xRegister32(r), cl); break;
								case 1: xROR(xRegister32(r), cl); break;
								case 4: xSHL(xRegister32(r), cl); break;
								case 5: xSHR(xRegister32(r), cl); break;
								default:xSAR(xRegister32(r), cl); break; } } },
							[&](uint8_t* p){ E_G2_RCL(p, w, g, r); return p; });
					}
				}
	}

	/* mov r,r */
	for (int w = 0; w <= 1; w++)
		for (int d = 0; d < 16; d++)
			for (int s = 0; s < 16; s++)
			{
				snprintf(nm, sizeof(nm), "MOV_RR w%d d%d s%d", w, d, s);
				check(nm,
					[&]{ if (w) xMOV(xRegister64(d), xRegister64(s));
					     else    xMOV(xRegister32(d), xRegister32(s)); },
					[&](uint8_t* p){ E_MOV_RR(p, w, d, s); return p; });
			}

	/* movzx / movsx from r8 and r16 */
	for (int sx = 0; sx <= 1; sx++)
		for (int srcw = 0; srcw <= 1; srcw++)
			for (int d = 0; d < 16; d++)
				for (int s = 0; s < 16; s++)
				{
					if (!srcw && s >= 4 && s < 8) continue; /* ah/ch/dh/bh alias */
					snprintf(nm, sizeof(nm), "MOV%cX%d d%d s%d", sx?'S':'Z', srcw?16:8, d, s);
					check(nm,
						[&]{ if (sx) { if (srcw) xMOVSX(xRegister32(d), xRegister16(s));
						               else      xMOVSX(xRegister32(d), xRegister8(s)); }
						     else    { if (srcw) xMOVZX(xRegister32(d), xRegister16(s));
						               else      xMOVZX(xRegister32(d), xRegister8(s)); } },
						[&](uint8_t* p){ E_MOVEXT_RR(p, 0, sx, srcw, d, s); return p; });
				}

	/* group3 NOT/NEG on registers */
	for (int w = 0; w <= 1; w++)
		for (int r = 0; r < 16; r++)
		{
			snprintf(nm, sizeof(nm), "NOT_R w%d r%d", w, r);
			check(nm, [&]{ if (w) xNOT(xRegister64(r)); else xNOT(xRegister32(r)); },
				[&](uint8_t* p){ E_G3_R(p, w, 2, r); return p; });
			snprintf(nm, sizeof(nm), "NEG_R w%d r%d", w, r);
			check(nm, [&]{ if (w) xNEG(xRegister64(r)); else xNEG(xRegister32(r)); },
				[&](uint8_t* p){ E_G3_R(p, w, 3, r); return p; });
		}

	/* inc / dec */
	for (int w = 0; w <= 1; w++)
		for (int r = 0; r < 16; r++)
		{
			snprintf(nm, sizeof(nm), "INC_R w%d r%d", w, r);
			check(nm, [&]{ if (w) xINC(xRegister64(r)); else xINC(xRegister32(r)); },
				[&](uint8_t* p){ E_INCDEC_R(p, w, 0, r); return p; });
			snprintf(nm, sizeof(nm), "DEC_R w%d r%d", w, r);
			check(nm, [&]{ if (w) xDEC(xRegister64(r)); else xDEC(xRegister32(r)); },
				[&](uint8_t* p){ E_INCDEC_R(p, w, 1, r); return p; });
		}

	/* push / pop */
	for (int r = 0; r < 16; r++)
	{
		snprintf(nm, sizeof(nm), "PUSH_R r%d", r);
		check(nm, [&]{ xPUSH(xRegister64(r)); }, [&](uint8_t* p){ E_PUSH_R(p, r); return p; });
		snprintf(nm, sizeof(nm), "POP_R r%d", r);
		check(nm, [&]{ xPOP(xRegister64(r)); }, [&](uint8_t* p){ E_POP_R(p, r); return p; });
	}

	/* test r, imm32 */
	for (int w = 0; w <= 1; w++)
		for (int r = 0; r < 16; r++)
			for (int ii = 0; ii < nimm; ii++)
			{
				const s32 im = imms[ii];
				snprintf(nm, sizeof(nm), "TEST_RI w%d r%d i%d", w, r, ii);
				check(nm,
					[&]{ if (w) xTEST(xRegister64(r), im); else xTEST(xRegister32(r), im); },
					[&](uint8_t* p){ E_TEST_RI(p, w, r, im); return p; });
			}


	/* ============ batch 3: base + index*scale + disp ============ */
	{
		/* Registers to use as base/index: cover low, rsp(4), rbp(5), r12, r13
		 * and the extended file, since those are the encoding corner cases. */
		const int regs[] = { 0, 1, 3, 4, 5, 7, 8, 12, 13, 15 };
		const int nreg = (int)(sizeof(regs)/sizeof(regs[0]));
		const int scales[] = { 0, 1, 2, 3, 4, 5, 8, 9 };
		const int nsc = (int)(sizeof(scales)/sizeof(scales[0]));
		const s32 disps[] = { 0, 1, 0x7f, 0x80, -1, -0x80, -0x81, 0x12345678 };
		const int ndisp = (int)(sizeof(disps)/sizeof(disps[0]));

		/* base only, no index */
		for (int w = 0; w <= 1; w++)
		for (int bi = 0; bi < nreg; bi++)
		for (int di = 0; di < ndisp; di++)
		for (int r = 0; r < 16; r += 5)
		{
			const int b = regs[bi]; const s32 d = disps[di];
			snprintf(nm, sizeof(nm), "MOV_R_MEM base w%d b%d d%d r%d", w, b, di, r);
			check(nm,
				[&]{ if (w) xMOV(xRegister64(r), ptrNative[xAddressReg(b) + d]);
				     else    xMOV(xRegister32(r), ptr32[xAddressReg(b) + d]); },
				[&](uint8_t* p){ struct e_mem m; E_MEM(m, b, E_NOREG, 0, d);
				              E_MOV_R_MEM(p, w, r, m); return p; });
		}

		/* base + index*scale + disp */
		for (int w = 0; w <= 1; w++)
		for (int bi = 0; bi < nreg; bi++)
		for (int xi = 0; xi < nreg; xi++)
		for (int si = 0; si < nsc; si++)
		for (int di = 0; di < ndisp; di += 3)
		{
			const int b = regs[bi]; const int x = regs[xi];
			const int sc = scales[si]; const s32 d = disps[di];
			const int r = 2;
			snprintf(nm, sizeof(nm), "MOV_R_MEM bx w%d b%d x%d s%d d%d", w, b, x, sc, di);
			check(nm,
				[&]{ if (w) xMOV(xRegister64(r), ptrNative[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)]);
				     else    xMOV(xRegister32(r), ptr32[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)]); },
				[&](uint8_t* p){ struct e_mem m; E_MEM(m, b, x, sc, d);
				              E_MOV_R_MEM(p, w, r, m); return p; });

			snprintf(nm, sizeof(nm), "MOV_MEM_R bx w%d b%d x%d s%d d%d", w, b, x, sc, di);
			check(nm,
				[&]{ if (w) xMOV(ptrNative[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)], xRegister64(r));
				     else    xMOV(ptr32[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)], xRegister32(r)); },
				[&](uint8_t* p){ struct e_mem m; E_MEM(m, b, x, sc, d);
				              E_MOV_MEM_R(p, w, r, m); return p; });
		}

		/* index*scale with no base */
		for (int xi = 0; xi < nreg; xi++)
		for (int si = 0; si < nsc; si++)
		for (int di = 0; di < ndisp; di++)
		{
			const int x = regs[xi]; const int sc = scales[si]; const s32 d = disps[di];
			const int r = 6;
			snprintf(nm, sizeof(nm), "MOV_R_MEM idx x%d s%d d%d", x, sc, di);
			check(nm,
				[&]{ xMOV(xRegister32(r), ptr32[xAddressReg(x)*sc + d]); },
				[&](uint8_t* p){ struct e_mem m; E_MEM(m, E_NOREG, x, sc, d);
				              E_MOV_R_MEM(p, 0, r, m); return p; });
		}

		/* group1 against a full memory operand */
		for (int gi = 0; gi < 6; gi++)
		for (int bi = 0; bi < nreg; bi++)
		for (int xi = 0; xi < nreg; xi += 3)
		for (int di = 0; di < ndisp; di += 2)
		{
			const int op = g1[gi].op; const int b = regs[bi]; const int x = regs[xi];
			const s32 d = disps[di]; const int r = 1; const int sc = 4;
			snprintf(nm, sizeof(nm), "%s_R_MEM b%d x%d d%d", g1[gi].n, b, x, di);
			check(nm,
				[&]{ switch(op){
					case 0: xADD(xRegister32(r), ptr32[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)]); break;
					case 1: xOR (xRegister32(r), ptr32[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)]); break;
					case 4: xAND(xRegister32(r), ptr32[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)]); break;
					case 5: xSUB(xRegister32(r), ptr32[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)]); break;
					case 6: xXOR(xRegister32(r), ptr32[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)]); break;
					default:xCMP(xRegister32(r), ptr32[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)]); break; } },
				[&](uint8_t* p){ struct e_mem m; E_MEM(m, b, x, sc, d);
				              E_G1_R_MEM(p, 0, op, r, m); return p; });
		}
	}


	/* ============ batch 4: group3 mem, IMUL family, LEA ============ */
	{
		const int regs4[] = { 0, 1, 3, 4, 5, 7, 8, 12, 13, 15 };
		const int nreg4 = (int)(sizeof(regs4)/sizeof(regs4[0]));
		const int scales4[] = { 0, 1, 2, 4, 8 };
		const int nsc4 = (int)(sizeof(scales4)/sizeof(scales4[0]));
		const s32 disps4[] = { 0, 1, 0x7f, 0x80, -1, -0x80, 0x12345678 };
		const int nd4 = (int)(sizeof(disps4)/sizeof(disps4[0]));

		/* group3 NOT/NEG/MUL/DIV over memory */
		{
			struct G3 { int g; const char* n; };
			const G3 g3t[] = { {2,"NOT"}, {3,"NEG"}, {4,"UMUL"}, {6,"UDIV"} };
			for (int gi = 0; gi < 4; gi++)
			for (int bi = 0; bi < nreg4; bi++)
			for (int di = 0; di < nd4; di++)
			{
				const int g = g3t[gi].g; const int b = regs4[bi]; const s32 d = disps4[di];
				snprintf(nm, sizeof(nm), "%s_MEM b%d d%d", g3t[gi].n, b, di);
				check(nm,
					[&]{ switch(g){
						case 2: xNOT (ptr32[xAddressReg(b) + d]); break;
						case 3: xNEG (ptr32[xAddressReg(b) + d]); break;
						case 4: xUMUL(ptr32[xAddressReg(b) + d]); break;
						default:xUDIV(ptr32[xAddressReg(b) + d]); break; } },
					[&](uint8_t* p){ struct e_mem m; E_MEM(m, b, E_NOREG, 0, d);
					              E_G3_MEM(p, 0, g, m); return p; });
			}
		}

		/* two-operand IMUL, reg,reg and reg,mem */
		for (int d0 = 0; d0 < 16; d0++)
		for (int s0 = 0; s0 < 16; s0++)
		{
			snprintf(nm, sizeof(nm), "IMUL_RR d%d s%d", d0, s0);
			check(nm, [&]{ xMUL(xRegister32(d0), xRegister32(s0)); },
				[&](uint8_t* p){ E_IMUL_RR(p, 0, d0, s0); return p; });
		}
		for (int bi = 0; bi < nreg4; bi++)
		for (int di = 0; di < nd4; di++)
		{
			const int b = regs4[bi]; const s32 d = disps4[di]; const int r = 3;
			snprintf(nm, sizeof(nm), "IMUL_R_MEM b%d d%d", b, di);
			check(nm, [&]{ xMUL(xRegister32(r), ptr32[xAddressReg(b) + d]); },
				[&](uint8_t* p){ struct e_mem m; E_MEM(m, b, E_NOREG, 0, d);
				              E_IMUL_R_MEM(p, 0, r, m); return p; });
		}

		/* three-operand IMUL */
		for (int d0 = 0; d0 < 16; d0 += 3)
		for (int s0 = 0; s0 < 16; s0 += 3)
		for (int ii = 0; ii < nimm; ii++)
		{
			const s32 im = imms[ii];
			snprintf(nm, sizeof(nm), "IMUL_RRI d%d s%d i%d", d0, s0, ii);
			check(nm, [&]{ xMUL(xRegister32(d0), xRegister32(s0), im); },
				[&](uint8_t* p){ E_IMUL_RRI(p, 0, d0, s0, im); return p; });
		}
		for (int bi = 0; bi < nreg4; bi++)
		for (int ii = 0; ii < nimm; ii++)
		{
			const int b = regs4[bi]; const s32 im = imms[ii]; const int r = 2; const s32 d = 0x40;
			snprintf(nm, sizeof(nm), "IMUL_RMI b%d i%d", b, ii);
			check(nm, [&]{ xMUL(xRegister32(r), ptr32[xAddressReg(b) + d], im); },
				[&](uint8_t* p){ struct e_mem m; E_MEM(m, b, E_NOREG, 0, d);
				              E_IMUL_RMI(p, 0, r, m, im); return p; });
		}

		/* LEA across every operand shape and both preserve_flags settings */
		for (int pf = 0; pf <= 1; pf++)
		for (int w = 0; w <= 1; w++)
		for (int bi = 0; bi < nreg4; bi++)
		for (int xi = 0; xi < nreg4; xi++)
		for (int si = 0; si < nsc4; si++)
		for (int di = 0; di < nd4; di += 2)
		{
			const int b = regs4[bi]; const int x = regs4[xi];
			const int sc = scales4[si]; const s32 d = disps4[di]; const int r = 1;
			snprintf(nm, sizeof(nm), "LEA pf%d w%d b%d x%d s%d d%d", pf, w, b, x, sc, di);
			check(nm,
				[&]{ if (w) xLEA(xRegister64(r), ptrNative[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)], pf);
				     else    xLEA(xRegister32(r), ptr32[xAddressVoid(xAddressReg(b), xAddressReg(x), sc, d)], pf); },
				[&](uint8_t* p){ struct e_mem m; E_MEM(m, b, x, sc, d);
				              E_LEA(p, w, pf, r, m); return p; });
		}
		/* LEA: index only, and displacement only */
		for (int pf = 0; pf <= 1; pf++)
		for (int xi = 0; xi < nreg4; xi++)
		for (int si = 0; si < nsc4; si++)
		for (int di = 0; di < nd4; di++)
		{
			const int x = regs4[xi]; const int sc = scales4[si]; const s32 d = disps4[di]; const int r = 5;
			snprintf(nm, sizeof(nm), "LEA idx pf%d x%d s%d d%d", pf, x, sc, di);
			check(nm,
				[&]{ xLEA(xRegister32(r), ptr32[xAddressReg(x)*sc + d], pf); },
				[&](uint8_t* p){ struct e_mem m; E_MEM(m, E_NOREG, x, sc, d);
				              E_LEA(p, 0, pf, r, m); return p; });
		}
	}


	/* ================= batch 5: jumps and calls ================= */
	{
		/* Targets spanning the s8 boundary in both directions, plus far. */
		const long offs[] = { 0, 2, 3, 0x7f, 0x81, 0x100, -1, -0x7e, -0x80, -0x81, 0x100000 };
		const int noff = (int)(sizeof(offs)/sizeof(offs[0]));
		struct CC { int cc; const char* n; };
		const CC ccs[] = { {0x10,"JMP"}, {0x4,"JE"}, {0x5,"JNE"}, {0xc,"JL"},
		                   {0xd,"JGE"}, {0x2,"JB"}, {0x7,"JA"}, {0x8,"JS"} };
		const int ncc = (int)(sizeof(ccs)/sizeof(ccs[0]));

		for (int ci = 0; ci < ncc; ci++)
		for (int oi = 0; oi < noff; oi++)
		{
			const int cc = ccs[ci].cc;
			const void* tgt = (const void*)((char*)g_buf + offs[oi]);
			snprintf(nm, sizeof(nm), "%s_TO o%d", ccs[ci].n, oi);
			check(nm,
				[&]{ xJccKnownTarget(cc == 0x10 ? Jcc_Unconditional : (JccComparisonType)cc,
				                     tgt, false); },
				[&](uint8_t* p){ E_JCC_TO(p, cc, tgt); return p; });
		}

		/* Forward jumps: emit, then place the target n bytes later. */
		for (int ci = 0; ci < ncc; ci++)
		for (int pad = 0; pad < 6; pad++)
		{
			const int cc = ccs[ci].cc;
			snprintf(nm, sizeof(nm), "%s_FWD8 pad%d", ccs[ci].n, pad);
			check(nm,
				[&]{ xForwardJump8 j(cc == 0x10 ? Jcc_Unconditional : (JccComparisonType)cc);
				     for (int k = 0; k < pad; k++) xNOP();
				     j.SetTarget(); },
				[&](uint8_t* p){ uint8_t* base; int k;
				     E_FWD8(p, cc, base);
				     for (k = 0; k < pad; k++) E_NOP(p);
				     E_FWD8_SET(p, base); return p; });

			snprintf(nm, sizeof(nm), "%s_FWD32 pad%d", ccs[ci].n, pad);
			check(nm,
				[&]{ xForwardJump32 j(cc == 0x10 ? Jcc_Unconditional : (JccComparisonType)cc);
				     for (int k = 0; k < pad; k++) xNOP();
				     j.SetTarget(); },
				[&](uint8_t* p){ uint8_t* base; int k;
				     E_FWD32(p, cc, base);
				     for (k = 0; k < pad; k++) E_NOP(p);
				     E_FWD32_SET(p, base); return p; });
		}

		/* call / jmp through a register, ret, nop */
		for (int r = 0; r < 16; r++)
		{
			snprintf(nm, sizeof(nm), "CALL_R r%d", r);
			check(nm, [&]{ xCALL(xAddressReg(r)); },
				[&](uint8_t* p){ E_CALL_R(p, r); return p; });
			snprintf(nm, sizeof(nm), "JMP_R r%d", r);
			check(nm, [&]{ xJMP(xAddressReg(r)); },
				[&](uint8_t* p){ E_JMP_R(p, r); return p; });
		}
		check("RET", []{ xRET(); }, [](uint8_t* p){ E_RET(p); return p; });
		check("NOP", []{ xNOP(); }, [](uint8_t* p){ E_NOP(p); return p; });
	}


	/* ================= batch 5b: 16-bit forms ================= */
	{
		const s32 imms16[] = { 0, 1, 0x7f, 0x80, -1, -0x80, -0x81, 0x1234, 0x7fff };
		const int ni16 = (int)(sizeof(imms16)/sizeof(imms16[0]));
		for (int d = 0; d < 16; d++)
		for (int s2 = 0; s2 < 16; s2++)
		{
			snprintf(nm, sizeof(nm), "MOV16_RR d%d s%d", d, s2);
			check(nm, [&]{ xMOV(xRegister16(d), xRegister16(s2)); },
				[&](uint8_t* p){ E_MOV16_RR(p, d, s2); return p; });
			snprintf(nm, sizeof(nm), "ADD16_RR d%d s%d", d, s2);
			check(nm, [&]{ xADD(xRegister16(d), xRegister16(s2)); },
				[&](uint8_t* p){ E_G1_16_RR(p, 0, d, s2); return p; });
		}
		for (int r = 0; r < 16; r++)
		for (int a = 0; a < naddr; a++)
		{
			const void* ad = addrs[a];
			snprintf(nm, sizeof(nm), "MOV16_R_M r%d a%d", r, a);
			check(nm, [&]{ xMOV(xRegister16(r), ptr16[ad]); },
				[&](uint8_t* p){ E_MOV16_R_M(p, r, ad); return p; });
			snprintf(nm, sizeof(nm), "MOV16_M_R r%d a%d", r, a);
			check(nm, [&]{ xMOV(ptr16[ad], xRegister16(r)); },
				[&](uint8_t* p){ E_MOV16_M_R(p, r, ad); return p; });
		}
		for (int gi = 0; gi < 6; gi++)
		for (int r = 0; r < 16; r++)
		for (int ii = 0; ii < ni16; ii++)
		{
			const int op = g1[gi].op; const s32 im = imms16[ii];
			snprintf(nm, sizeof(nm), "%s16_RI r%d i%d", g1[gi].n, r, ii);
			check(nm,
				[&]{ switch(op){
					case 0: xADD(xRegister16(r), im); break;
					case 1: xOR (xRegister16(r), im); break;
					case 4: xAND(xRegister16(r), im); break;
					case 5: xSUB(xRegister16(r), im); break;
					case 6: xXOR(xRegister16(r), im); break;
					default:xCMP(xRegister16(r), im); break; } },
				[&](uint8_t* p){ E_G1_16_RI(p, op, r, im); return p; });
		}
	}


	/* ================= batch 6: 8-bit forms ================= */
	{
		/* Every distinct 8-bit register, including both readings of ids 4-7. */
		struct R8 { int id; const char* n; };
		const R8 r8s[] = {
			{0,"al"},{1,"cl"},{2,"dl"},{3,"bl"},
			{4,"ah"},{5,"ch"},{6,"dh"},{7,"bh"},
			{4|0x10,"spl"},{5|0x10,"bpl"},{6|0x10,"sil"},{7|0x10,"dil"},
			{8,"r8b"},{9,"r9b"},{12,"r12b"},{13,"r13b"},{15,"r15b"} };
		const int nr8 = (int)(sizeof(r8s)/sizeof(r8s[0]));
		const s32 imms8[] = { 0, 1, 0x7f, -1, -0x80, 0x55 };
		const int ni8 = (int)(sizeof(imms8)/sizeof(imms8[0]));

		for (int di = 0; di < nr8; di++)
		for (int si = 0; si < nr8; si++)
		{
			const int d = r8s[di].id, s3 = r8s[si].id;
			/* ah/ch/dh/bh cannot coexist with a REX byte; the reference does
			 * not guard this, and such a pair is not constructible in the
			 * recompilers, so skip the illegal combinations. */
			if ((d < 0x10 && d >= 4 && d < 8) && (s3 >= 0x10 || (s3 & 0x0F) > 7)) continue;
			if ((s3 < 0x10 && s3 >= 4 && s3 < 8) && (d >= 0x10 || (d & 0x0F) > 7)) continue;
			snprintf(nm, sizeof(nm), "MOV8_RR %s,%s", r8s[di].n, r8s[si].n);
			check(nm, [&]{ xMOV(xRegister8(d & 0x0F, d >= 0x10), xRegister8(s3 & 0x0F, s3 >= 0x10)); },
				[&](uint8_t* p){ E_MOV8_RR(p, d, s3); return p; });
			snprintf(nm, sizeof(nm), "ADD8_RR %s,%s", r8s[di].n, r8s[si].n);
			check(nm, [&]{ xADD(xRegister8(d & 0x0F, d >= 0x10), xRegister8(s3 & 0x0F, s3 >= 0x10)); },
				[&](uint8_t* p){ E_G1_8_RR(p, 0, d, s3); return p; });
		}

		for (int ri = 0; ri < nr8; ri++)
		for (int a = 0; a < naddr; a++)
		{
			const int r = r8s[ri].id; const void* ad = addrs[a];
			snprintf(nm, sizeof(nm), "MOV8_R_M %s a%d", r8s[ri].n, a);
			check(nm, [&]{ xMOV(xRegister8(r & 0x0F, r >= 0x10), ptr8[ad]); },
				[&](uint8_t* p){ E_MOV8_R_M(p, r, ad); return p; });
			snprintf(nm, sizeof(nm), "MOV8_M_R %s a%d", r8s[ri].n, a);
			check(nm, [&]{ xMOV(ptr8[ad], xRegister8(r & 0x0F, r >= 0x10)); },
				[&](uint8_t* p){ E_MOV8_M_R(p, r, ad); return p; });
		}

		for (int gi = 0; gi < 6; gi++)
		for (int ri = 0; ri < nr8; ri++)
		for (int ii = 0; ii < ni8; ii++)
		{
			const int op = g1[gi].op; const int r = r8s[ri].id; const s32 im = imms8[ii];
			const xRegister8 R(r & 0x0F, r >= 0x10);
			snprintf(nm, sizeof(nm), "%s8_RI %s i%d", g1[gi].n, r8s[ri].n, ii);
			check(nm,
				[&]{ switch(op){
					case 0: xADD(R, im); break;  case 1: xOR (R, im); break;
					case 4: xAND(R, im); break;  case 5: xSUB(R, im); break;
					case 6: xXOR(R, im); break;  default: xCMP(R, im); break; } },
				[&](uint8_t* p){ E_G1_8_RI(p, op, r, im); return p; });
		}

		for (int ri = 0; ri < nr8; ri++)
		{
			const int r = r8s[ri].id;
			snprintf(nm, sizeof(nm), "SETL %s", r8s[ri].n);
			check(nm, [&]{ xSETL(xRegister8(r & 0x0F, r >= 0x10)); },
				[&](uint8_t* p){ E_SETCC_R8(p, 0xc, r); return p; });
		}
	}


	/* ================= batch 7: SSE ================= */
	{
		/* A representative instruction per encoding shape: every prefix
		 * (none / 66 / f2 / f3), two-byte and three-byte opcodes, loads,
		 * stores, and the imm8 forms. Each is checked over all 16 xmm
		 * registers and every address class. */
		struct S2 { u8 pre; u16 op; const char* n; };
		const S2 rr[] = {
			{0x00, 0x58, "ADDPS"}, {0x66, 0x58, "ADDPD"},
			{0xf3, 0x58, "ADDSS"}, {0xf2, 0x58, "ADDSD"},
			{0x00, 0x59, "MULPS"}, {0x66, 0x5c, "SUBPD"},
			{0x00, 0x54, "ANDPS"}, {0x66, 0xef, "PXOR"},
			{0x66, 0x6f, "MOVDQA"},{0xf3, 0x6f, "MOVDQU"},
			{0x00, 0x28, "MOVAPS"},{0x66, 0x10, "MOVUPD"},
			{0x66, 0xd4, "PADDQ"}, {0x66, 0xf4, "PMULUDQ"},
			{0x66, 0x3829,"PCMPEQQ"},   /* three-byte 0F 38 */
			{0x66, 0x3a0b,"ROUNDSD"} }; /* three-byte 0F 3A */
		const int nrr = (int)(sizeof(rr)/sizeof(rr[0]));

		for (int i = 0; i < nrr; i++)
		for (int a2 = 0; a2 < 16; a2++)
		for (int b2 = 0; b2 < 16; b2++)
		{
			const u8 pre = rr[i].pre; const u16 op = rr[i].op;
			snprintf(nm, sizeof(nm), "SSE_RR %s x%d,x%d", rr[i].n, a2, b2);
			check(nm,
				[&]{ xOpWrite0F(pre, op, xRegisterSSE(a2), xRegisterSSE(b2)); },
				[&](uint8_t* p){ E_SSE_RR(p, pre, op, a2, b2); return p; });
		}

		for (int i = 0; i < nrr; i++)
		for (int r = 0; r < 16; r++)
		for (int a = 0; a < naddr; a++)
		{
			const u8 pre = rr[i].pre; const u16 op = rr[i].op; const void* ad = addrs[a];
			snprintf(nm, sizeof(nm), "SSE_R_M %s x%d a%d", rr[i].n, r, a);
			check(nm,
				[&]{ xOpWrite0F(pre, op, xRegisterSSE(r), ptr128[ad]); },
				[&](uint8_t* p){ E_SSE_R_M(p, pre, op, r, ad); return p; });
		}

		/* full addressing under SSE */
		{
			const int regsS[] = { 0, 4, 5, 12, 13, 15 };
			const int nrs = (int)(sizeof(regsS)/sizeof(regsS[0]));
			const int scS[] = { 0, 1, 4, 8 };
			const s32 dS[] = { 0, 0x7f, 0x80, -1 };
			for (int i = 0; i < nrr; i += 5)
			for (int bi = 0; bi < nrs; bi++)
			for (int xi = 0; xi < nrs; xi++)
			for (int si = 0; si < 4; si++)
			for (int di = 0; di < 4; di++)
			{
				const u8 pre = rr[i].pre; const u16 op = rr[i].op;
				const int b3 = regsS[bi], x3 = regsS[xi], sc = scS[si]; const s32 d = dS[di];
				const int r = 3;
				snprintf(nm, sizeof(nm), "SSE_R_MEM %s b%d x%d s%d d%d", rr[i].n, b3, x3, sc, di);
				check(nm,
					[&]{ xOpWrite0F(pre, op, xRegisterSSE(r),
					        ptr128[xAddressVoid(xAddressReg(b3), xAddressReg(x3), sc, d)]); },
					[&](uint8_t* p){ struct e_mem m; E_MEM(m, b3, x3, sc, d);
					              E_SSE_R_MEM(p, pre, op, r, m); return p; });
			}
		}

		/* imm8 forms */
		{
			const S2 imm8s[] = {
				{0x00, 0xc6, "SHUFPS"}, {0x66, 0x70, "PSHUFD"},
				{0x00, 0xc2, "CMPPS"},  {0x66, 0x3a0b, "ROUNDSD"},
				{0x66, 0x3a21, "INSERTPS"} };
			const int nimm8 = (int)(sizeof(imm8s)/sizeof(imm8s[0]));
			const u8 i8s[] = { 0x00, 0x01, 0x55, 0xaa, 0xff };
			for (int i = 0; i < nimm8; i++)
			for (int a2 = 0; a2 < 16; a2 += 3)
			for (int b2 = 0; b2 < 16; b2 += 3)
			for (int k = 0; k < 5; k++)
			{
				const u8 pre = imm8s[i].pre; const u16 op = imm8s[i].op; const u8 im = i8s[k];
				snprintf(nm, sizeof(nm), "SSE_RRI %s x%d,x%d i%d", imm8s[i].n, a2, b2, k);
				check(nm,
					[&]{ xOpWrite0F(pre, op, xRegisterSSE(a2), xRegisterSSE(b2), im); },
					[&](uint8_t* p){ E_SSE_RRI(p, pre, op, a2, b2, im); return p; });
			}
			for (int i = 0; i < nimm8; i++)
			for (int r = 0; r < 16; r += 5)
			for (int a = 0; a < naddr; a++)
			for (int k = 0; k < 5; k += 2)
			{
				const u8 pre = imm8s[i].pre; const u16 op = imm8s[i].op;
				const u8 im = i8s[k]; const void* ad = addrs[a];
				snprintf(nm, sizeof(nm), "SSE_R_MI %s x%d a%d i%d", imm8s[i].n, r, a, k);
				check(nm,
					[&]{ xOpWrite0F(pre, op, xRegisterSSE(r), ptr128[ad], im); },
					[&](uint8_t* p){ E_SSE_R_MI(p, pre, op, r, ad, im); return p; });
			}
		}
	}


	/* ================= batch 8: AVX (two-byte VEX) ================= */
	{
		/* xmm and ymm, all 16 registers, both move opcodes and the
		 * three-operand form, with and without the 256-bit L bit. */
		for (int d = 0; d < 16; d++)
		for (int s4 = 0; s4 < 16; s4++)
		{
			snprintf(nm, sizeof(nm), "VMOVAPS_RR x%d,x%d", d, s4);
			check(nm, [&]{ xVMOVAPS(xRegisterSSE(d), xRegisterSSE(s4)); },
				[&](uint8_t* p){ E_VEX_RR(p, 0x00, 0x28, d, s4, 0); return p; });
			snprintf(nm, sizeof(nm), "VMOVUPS_RR x%d,x%d", d, s4);
			check(nm, [&]{ xVMOVUPS(xRegisterSSE(d), xRegisterSSE(s4)); },
				[&](uint8_t* p){ E_VEX_RR(p, 0x00, 0x10, d, s4, 0); return p; });
		}
		for (int d = 0; d < 16; d++)
		for (int a = 0; a < naddr; a++)
		{
			const void* ad = addrs[a];
			snprintf(nm, sizeof(nm), "VMOVAPS_RM x%d a%d", d, a);
			check(nm, [&]{ xVMOVAPS(xRegisterSSE(d), ptr128[ad]); },
				[&](uint8_t* p){ E_VEX_RM(p, 0x00, 0x28, d, ad, 0); return p; });
			snprintf(nm, sizeof(nm), "VMOVAPS_MR x%d a%d", d, a);
			check(nm, [&]{ xVMOVAPS(ptr128[ad], xRegisterSSE(d)); },
				[&](uint8_t* p){ E_VEX_RM(p, 0x00, 0x29, d, ad, 0); return p; });
		}
		/* three-operand, xmm and ymm */
		for (int d = 0; d < 16; d++)
		for (int s1 = 0; s1 < 16; s1++)
		for (int s2 = 0; s2 < 16; s2 += 5)
		{
			snprintf(nm, sizeof(nm), "VPAND_RRR x%d,x%d,x%d", d, s1, s2);
			check(nm, [&]{ xVPAND(xRegisterSSE(d), xRegisterSSE(s1), xRegisterSSE(s2)); },
				[&](uint8_t* p){ E_VEX_RRR(p, 0x66, 0xDB, d, s1, s2, 0); return p; });
		}
		/* three-operand with memory */
		for (int d = 0; d < 16; d += 3)
		for (int s1 = 0; s1 < 16; s1 += 3)
		for (int a = 0; a < naddr; a++)
		{
			const void* ad = addrs[a];
			snprintf(nm, sizeof(nm), "VPAND_RRM x%d,x%d a%d", d, s1, a);
			check(nm, [&]{ xVPAND(xRegisterSSE(d), xRegisterSSE(s1), ptr128[ad]); },
				[&](uint8_t* p){ E_VEX_RRM(p, 0x66, 0xDB, d, s1, ad, 0); return p; });
		}
	}


	/* ============ batch 9: mov-immediate family ============ */
	{
		const s64 imm64s[] = {
			0, 1, -1, 0x7f, 0x7fffffff, -0x80000000LL,
			0x80000000LL,            /* fits u32 but not s32 */
			0x100000000LL,           /* needs movabs */
			-0x80000001LL,           /* needs movabs */
			0x123456789abcdefLL, (s64)0xffffffffffffffffULL };
		const int ni64 = (int)(sizeof(imm64s)/sizeof(imm64s[0]));

		for (int pf = 0; pf <= 1; pf++)
		for (int w = 0; w <= 1; w++)
		for (int r = 0; r < 16; r++)
		for (int ii = 0; ii < ni64; ii++)
		{
			const s64 im = imm64s[ii];
			if (!w && (im != (s64)(s32)im)) continue; /* not expressible in 32-bit */
			snprintf(nm, sizeof(nm), "MOV_RI pf%d w%d r%d i%d", pf, w, r, ii);
			check(nm,
				[&]{ if (w) xMOV(xRegister64(r), (sptr)im, pf);
				     else    xMOV(xRegister32(r), (sptr)im, pf); },
				[&](uint8_t* p){ E_MOV_RI(p, w, pf, r, (intptr_t)im); return p; });
		}

		for (int pf = 0; pf <= 1; pf++)
		for (int r = 0; r < 16; r++)
		for (int ii = 0; ii < ni64; ii++)
		{
			const s64 im = imm64s[ii];
			snprintf(nm, sizeof(nm), "MOV64_RI pf%d r%d i%d", pf, r, ii);
			check(nm,
				[&]{ xMOV64(xRegister64(r), im, pf); },
				[&](uint8_t* p){ E_MOV64_RI(p, pf, r, im); return p; });
		}

		for (int a = 0; a < naddr; a++)
		for (int ii = 0; ii < nimm; ii++)
		{
			const void* ad = addrs[a]; const s32 im = imms[ii];
			snprintf(nm, sizeof(nm), "MOV_M_I8 a%d i%d", a, ii);
			check(nm, [&]{ xMOV(ptr8[ad], (u8)im); },
				[&](uint8_t* p){ E_MOV_M_I8(p, ad, im); return p; });
			snprintf(nm, sizeof(nm), "MOV_M_I16 a%d i%d", a, ii);
			check(nm, [&]{ xMOV(ptr16[ad], (u16)im); },
				[&](uint8_t* p){ E_MOV_M_I16(p, ad, im); return p; });
			snprintf(nm, sizeof(nm), "MOV_M_I64 a%d i%d", a, ii);
			check(nm, [&]{ xMOV(ptr64[ad], im); },
				[&](uint8_t* p){ E_MOV_M_I64(p, ad, im); return p; });
		}
	}


	/* ============ batch 10: legacy layer + odd forms ============ */
	{
		const int rels[] = { 0, 1, 0x7f, 0x80, 0xff, 0x1234, -1 };
		const int nrel = (int)(sizeof(rels)/sizeof(rels[0]));
		for (int ri = 0; ri < nrel; ri++)
		{
			const int to = rels[ri];
			snprintf(nm, sizeof(nm), "L_JMP8 r%d", ri);
			check(nm, [&]{ JMP8((u8)to); },
				[&](uint8_t* p){ uint8_t* s5; E_L_JMP8(p, (u8)to, s5); (void)s5; return p; });
			snprintf(nm, sizeof(nm), "L_JMP32 r%d", ri);
			check(nm, [&]{ JMP32((uptr)to); },
				[&](uint8_t* p){ uint8_t* s5; E_L_JMP32(p, (u32)to, s5); (void)s5; return p; });
			snprintf(nm, sizeof(nm), "L_JZ8 r%d", ri);
			check(nm, [&]{ JZ8((u8)to); },
				[&](uint8_t* p){ uint8_t* s5; E_L_JCC8(p, 0x4, (u8)to, s5); (void)s5; return p; });
			snprintf(nm, sizeof(nm), "L_JNZ8 r%d", ri);
			check(nm, [&]{ JNZ8((u8)to); },
				[&](uint8_t* p){ uint8_t* s5; E_L_JCC8(p, 0x5, (u8)to, s5); (void)s5; return p; });
			snprintf(nm, sizeof(nm), "L_JZ32 r%d", ri);
			check(nm, [&]{ JZ32((u32)to); },
				[&](uint8_t* p){ uint8_t* s5; E_L_JCC32(p, 0x4, (u32)to, s5); (void)s5; return p; });
			snprintf(nm, sizeof(nm), "L_JNZ32 r%d", ri);
			check(nm, [&]{ JNZ32((u32)to); },
				[&](uint8_t* p){ uint8_t* s5; E_L_JCC32(p, 0x5, (u32)to, s5); (void)s5; return p; });
			snprintf(nm, sizeof(nm), "L_JL8 r%d", ri);
			check(nm, [&]{ JL8((u8)to); },
				[&](uint8_t* p){ uint8_t* s5; E_L_JCC8(p, 0xc, (u8)to, s5); (void)s5; return p; });
			snprintf(nm, sizeof(nm), "L_JGE8 r%d", ri);
			check(nm, [&]{ JGE8((u8)to); },
				[&](uint8_t* p){ uint8_t* s5; E_L_JCC8(p, 0xd, (u8)to, s5); (void)s5; return p; });
		}
		for (int m = 0; m < 4; m++)
		for (int rg = 0; rg < 8; rg++)
		for (int rm = 0; rm < 8; rm++)
		{
			snprintf(nm, sizeof(nm), "L_MODRM %d %d %d", m, rg, rm);
			check(nm, [&]{ ModRM(m, rg, rm); },
				[&](uint8_t* p){ E_L_MODRM(p, m, rg, rm); return p; });
		}
		/* push imm / push,pop mem / inc,dec mem */
		for (int ii = 0; ii < nimm; ii++)
		{
			const s32 im = imms[ii];
			snprintf(nm, sizeof(nm), "PUSH_I i%d", ii);
			check(nm, [&]{ xPUSH((u32)im); },
				[&](uint8_t* p){ E_PUSH_I(p, (uint32_t)im); return p; });
		}
		for (int a = 0; a < naddr; a++)
		{
			const void* ad = addrs[a];
			snprintf(nm, sizeof(nm), "PUSH_M a%d", a);
			check(nm, [&]{ xPUSH(ptrNative[ad]); },
				[&](uint8_t* p){ E_PUSH_M(p, ad); return p; });
			snprintf(nm, sizeof(nm), "POP_M a%d", a);
			check(nm, [&]{ xPOP(ptrNative[ad]); },
				[&](uint8_t* p){ E_POP_M(p, ad); return p; });
			snprintf(nm, sizeof(nm), "INC_M a%d", a);
			check(nm, [&]{ xINC(ptr32[ad]); },
				[&](uint8_t* p){ E_INCDEC_M(p, 0, ad); return p; });
			snprintf(nm, sizeof(nm), "DEC_M a%d", a);
			check(nm, [&]{ xDEC(ptr32[ad]); },
				[&](uint8_t* p){ E_INCDEC_M(p, 1, ad); return p; });
		}
	}


	/* ====== batch 11: every (prefix, opcode) pair the emitter defines ======
	 * Harvested from the {prefix, opcode} initialisers in simd.cpp, avx.cpp,
	 * groups.cpp and the implement/ headers, rather than hand-picked, so the
	 * shape coverage tracks the emitter instead of my sample of it. */
	{
		struct PO { u8 pre; u16 op; };
		const PO allops[] = {
			{0x00, 0x0014},
			{0x00, 0x0015},
			{0x00, 0x002e},
			{0x00, 0x0051},
			{0x00, 0x0054},
			{0x00, 0x0056},
			{0x00, 0x0057},
			{0x00, 0x0058},
			{0x00, 0x0059},
			{0x00, 0x005c},
			{0x00, 0x005d},
			{0x00, 0x005e},
			{0x00, 0x005f},
			{0x66, 0x0038},
			{0x66, 0x0b38},
			{0x66, 0x0c3a},
			{0x66, 0x0d3a},
			{0x66, 0x0e3a},
			{0x66, 0x1038},
			{0x66, 0x0014},
			{0x66, 0x1438},
			{0x66, 0x0015},
			{0x66, 0x1538},
			{0x66, 0x1738},
			{0x66, 0x1c38},
			{0x66, 0x1d38},
			{0x66, 0x1e38},
			{0x66, 0x2838},
			{0x66, 0x2b38},
			{0x66, 0x002e},
			{0x66, 0x3838},
			{0x66, 0x3938},
			{0x66, 0x3a38},
			{0x66, 0x3b38},
			{0x66, 0x3c38},
			{0x66, 0x3d38},
			{0x66, 0x3e38},
			{0x66, 0x3f38},
			{0x66, 0x4038},
			{0x66, 0x403a},
			{0x66, 0x413a},
			{0x66, 0x0054},
			{0x66, 0x0056},
			{0x66, 0x0057},
			{0x66, 0x0058},
			{0x66, 0x0059},
			{0x66, 0x005c},
			{0x66, 0x005d},
			{0x66, 0x005e},
			{0x66, 0x005f},
			{0x66, 0x0060},
			{0x66, 0x0061},
			{0x66, 0x0062},
			{0x66, 0x0063},
			{0x66, 0x0064},
			{0x66, 0x0065},
			{0x66, 0x0066},
			{0x66, 0x0067},
			{0x66, 0x0068},
			{0x66, 0x0069},
			{0x66, 0x006a},
			{0x66, 0x006b},
			{0x66, 0x006c},
			{0x66, 0x006d},
			{0x66, 0x0070},
			{0x66, 0x0074},
			{0x66, 0x0075},
			{0x66, 0x0076},
			{0x66, 0x00db},
			{0x66, 0x00d4},
			{0x66, 0x00d5},
			{0x66, 0x00d8},
			{0x66, 0x00da},
			{0x66, 0x00db},
			{0x66, 0x00dc},
			{0x66, 0x00de},
			{0x66, 0x00df},
			{0x66, 0x00e4},
			{0x66, 0x00e5},
			{0x66, 0x00ea},
			{0x66, 0x00eb},
			{0x66, 0x00ee},
			{0x66, 0x00ef},
			{0x66, 0x00f4},
			{0x66, 0xf438},
			{0x66, 0x00f5},
			{0x66, 0x00fb},
			{0xf2, 0x0051},
			{0xf2, 0x0058},
			{0xf2, 0x0059},
			{0xf2, 0x005c},
			{0xf2, 0x005d},
			{0xf2, 0x005e},
			{0xf2, 0x005f},
			{0xf2, 0x0070},
			{0xf3, 0x0051},
			{0xf3, 0x0058},
			{0xf3, 0x0059},
			{0xf3, 0x005c},
			{0xf3, 0x005d},
			{0xf3, 0x005e},
			{0xf3, 0x005f},
			{0xf3, 0x0070}
		};
		const int nall = (int)(sizeof(allops)/sizeof(allops[0]));
		for (int i = 0; i < nall; i++)
		{
			const u8 pre = allops[i].pre; const u16 op = allops[i].op;
			for (int a2 = 0; a2 < 16; a2 += 5)
			for (int b2 = 0; b2 < 16; b2 += 5)
			{
				snprintf(nm, sizeof(nm), "ALLOP_RR p%02x o%04x x%d,x%d", pre, op, a2, b2);
				check(nm,
					[&]{ xOpWrite0F(pre, op, xRegisterSSE(a2), xRegisterSSE(b2)); },
					[&](uint8_t* p){ E_SSE_RR(p, pre, op, a2, b2); return p; });
			}
			for (int r = 0; r < 16; r += 7)
			for (int a = 0; a < naddr; a++)
			{
				const void* ad = addrs[a];
				snprintf(nm, sizeof(nm), "ALLOP_RM p%02x o%04x x%d a%d", pre, op, r, a);
				check(nm,
					[&]{ xOpWrite0F(pre, op, xRegisterSSE(r), ptr128[ad]); },
					[&](uint8_t* p){ E_SSE_R_M(p, pre, op, r, ad); return p; });
				snprintf(nm, sizeof(nm), "ALLOP_RMI p%02x o%04x x%d a%d", pre, op, r, a);
				check(nm,
					[&]{ xOpWrite0F(pre, op, xRegisterSSE(r), ptr128[ad], (u8)0x5a); },
					[&](uint8_t* p){ E_SSE_R_MI(p, pre, op, r, ad, 0x5a); return p; });
			}
		}
	}

	printf("cases: %ld   divergent: %ld\n", g_cases, g_fail);
	for (size_t i = 0; i < g_failures.size(); i++)
		printf("  %s\n", g_failures[i].c_str());
	if (g_fail > 25) printf("  ... and %ld more\n", g_fail - 25);
	return g_fail ? 1 : 0;
}
