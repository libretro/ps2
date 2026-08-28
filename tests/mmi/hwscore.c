/*  Score MMI multiply-accumulate semantics against console captures.
 *
 *  Everything else under tests/ checks that two of our own engines agree
 *  with each other. That is worth having, but it cannot catch the case
 *  where both agree and both are wrong -- which is exactly what happened
 *  to PMADDW and PMSUBW: the interpreter carried two claimed hardware
 *  errata (a divide by 0xffffffff standing in for >> 32, and a
 *  conditional +0x70000000 on lane 0) that the x86 recompiler never
 *  implemented, so the two engines disagreed for years and the
 *  interpreter was the one that did not match a PS2.
 *
 *  This scores candidate semantics against real console output. The
 *  vectors and the expected results come from ps2autotests
 *  (tests/cpu/ee_simd/muldiv.expected), captured by running the test ELF
 *  on hardware. Only the pmaddw and pmsubw blocks are read; the operand
 *  sequence is transcribed from muldiv.cpp's TEST_RRRHL_FUNC macro.
 *
 *  Usage:
 *    tests/mmi/hwscore <path-to-muldiv.expected>
 *
 *  Exit status is non-zero if the current semantics fail any case.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t  s32; typedef int64_t  s64;

typedef struct { u32 UL[4]; } Q;

/* The harness reloads HI/LO from this before every case. */
static const u64 C_HILO_HI  = 0x0123456789ABCDEFull;
static const u64 C_HILO_LO  = 0x123456789ABCDEF0ull;
static const u64 C_HILO_HI1 = 0x23456789ABCDEF01ull;
static const u64 C_HILO_LO1 = 0x456789ABCDEF0123ull;

/* Current semantics, kept in step with _PMADDW / _PMSUBW in pcsx2/MMI.cpp. */
static void lane(int dd, int ss, const Q* rs, const Q* rt, Q* HI, Q* LO,
                 Q* rd, int sub)
{
	const s64 product = (s64)(s32)rs->UL[ss] * (s64)(s32)rt->UL[ss];
	const u64 acc     = (u64)LO->UL[ss] | ((u64)HI->UL[ss] << 32);
	const u64 result  = sub ? (acc - (u64)product) : (acc + (u64)product);
	const s64 nl = (s64)(s32)(u32)result;
	const s64 nh = (s64)(s32)(u32)(result >> 32);

	LO->UL[dd * 2] = (u32)nl; LO->UL[dd * 2 + 1] = (u32)((u64)nl >> 32);
	HI->UL[dd * 2] = (u32)nh; HI->UL[dd * 2 + 1] = (u32)((u64)nh >> 32);
	rd->UL[dd * 2] = LO->UL[dd * 2];
	rd->UL[dd * 2 + 1] = HI->UL[dd * 2];
}

static Q imm(u32 v)   /* lui/ori then pcpyld: both doublewords sign-extended */
{
	Q q; const u32 s = (v & 0x80000000u) ? 0xffffffffu : 0u;
	q.UL[0] = v; q.UL[1] = s; q.UL[2] = v; q.UL[3] = s; return q;
}
static Q lit(u32 a, u32 b, u32 c, u32 d)
{ Q q; q.UL[0]=a; q.UL[1]=b; q.UL[2]=c; q.UL[3]=d; return q; }

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "muldiv.expected";
	static const u32 imms[13][2] = {
		{0,0},{0,1},{1,0},{1,1},{2,2},{53,2},{53,0},
		{0xFFFFFFFFu,1},{1,0xFFFFFFFFu},{0xFFFFFFFFu,0xFFFFFFFFu},
		{0x80000000u,0xFFFFFFFFu},{0x80000000u,0},{0x12341234u,0x56785678u} };
	Q rsv[32], rtv[32];
	int nc = 0, i, op, failures = 0;

	for (i = 0; i < 13; i++)
	{ rsv[nc] = imm(imms[i][0]); rtv[nc] = imm(imms[i][1]); nc++; }
	{
		const Q Z      = lit(0,0,0,0);
		const Q ONE    = lit(1,0,0,0);
		const Q NEG    = lit(0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu);
		const Q S16MAX = lit(0x7FFFu,0,0,0);
		const Q S16MIN = lit(0xFFFF8000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu);
		const Q S32MAX = lit(0x7FFFFFFFu,0,0,0);
		const Q S32MIN = lit(0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu);
		const Q S64MAX = lit(0xFFFFFFFFu,0x7FFFFFFFu,0,0);
		const Q S64MIN = lit(0,0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu);
		const Q G1     = lit(0x1337u,0x1338u,0x1339u,0x133Au);
		const Q G2     = lit(0xDEADBEEFu,0xDEADBEEEu,0xDEADBEEDu,0xDEADBEECu);
		const Q TWO    = lit(2,2,2,2);
		const Q ONES   = lit(1,1,1,1);
		const Q S8A    = lit(0x80808080u,0xFFFFFFFFu,0x12345678u,0x7F7F7F7Fu);
		const Q S8B    = lit(0x80FF127Fu,0xFF807F34u,0x567F80FFu,0x7F78FF80u);
		const Q S16A   = lit(0x80008000u,0xFFFFFFFFu,0x12345678u,0x7FFF7FFFu);
		const Q S16B   = lit(0x8000FFFFu,0x12347FFFu,0xFFFF8000u,0x7FFF5678u);
		const Q A32    = lit(0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu);
		const Q B32    = lit(0xFFFFFFFFu,0x80000000u,0x7FFFFFFFu,0x12345678u);
		const Q C32    = lit(0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu);
		const Q D32    = lit(0x7FFFFFFFu,0x12345678u,0xFFFFFFFFu,0x80000000u);
		const Q E32    = lit(0x00000000u,0x7FFFFFFFu,0xFFFFFFFFu,0x80000000u);
		const Q F32    = lit(0xFFFFFFFFu,0x80000000u,0x00000000u,0x7FFFFFFFu);
		const Q ps[19][2] = { {Z,ONE},{ONE,ONE},{ONE,NEG},{S16MAX,S16MAX},
			{S16MIN,S16MIN},{S32MAX,S32MAX},{S32MIN,S32MIN},{S64MAX,S64MAX},
			{S64MIN,S64MIN},{G1,G2},{TWO,ONES},{S8A,S8B},{S16A,S16B},
			{A32,B32},{B32,A32},{C32,D32},{D32,C32},{E32,F32},{F32,E32} };
		for (i = 0; i < 19; i++) { rsv[nc] = ps[i][0]; rtv[nc] = ps[i][1]; nc++; }
	}

	for (op = 0; op < 2; op++)
	{
		const char* name = op ? "pmsubw" : "pmaddw";
		char head[32], buf[512];
		FILE* f = fopen(path, "r");
		int inblock = 0, idx = 0, pass = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", name);

		while (fgets(buf, sizeof(buf), f))
		{
			char* p;
			unsigned w3, w2, w1, w0;
			unsigned long long h0, h1, l0, l1;
			Q HI, LO, rd;
			char want[256], got[256];

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head))) inblock = 1; continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;
			p = strstr(buf, ": ");
			if (!p || idx >= nc) continue;
			if (sscanf(p + 2, "%x %x %x %x H: %llx %llx L: %llx %llx",
			           &w3, &w2, &w1, &w0, &h0, &h1, &l0, &l1) != 8)
				continue;

			HI.UL[0] = (u32)C_HILO_HI;  HI.UL[1] = (u32)(C_HILO_HI  >> 32);
			HI.UL[2] = (u32)C_HILO_HI1; HI.UL[3] = (u32)(C_HILO_HI1 >> 32);
			LO.UL[0] = (u32)C_HILO_LO;  LO.UL[1] = (u32)(C_HILO_LO  >> 32);
			LO.UL[2] = (u32)C_HILO_LO1; LO.UL[3] = (u32)(C_HILO_LO1 >> 32);
			rd = lit(0x1337u, 0x1338u, 0x1339u, 0x133Au);

			lane(0, 0, &rsv[idx], &rtv[idx], &HI, &LO, &rd, op);
			lane(1, 2, &rsv[idx], &rtv[idx], &HI, &LO, &rd, op);

			snprintf(want, sizeof(want),
				"%08x %08x %08x %08x H: %016llx %016llx L: %016llx %016llx",
				w3, w2, w1, w0, h0, h1, l0, l1);
			snprintf(got, sizeof(got),
				"%08x %08x %08x %08x H: %016llx %016llx L: %016llx %016llx",
				rd.UL[3], rd.UL[2], rd.UL[1], rd.UL[0],
				(unsigned long long)((u64)HI.UL[0] | ((u64)HI.UL[1] << 32)),
				(unsigned long long)((u64)HI.UL[2] | ((u64)HI.UL[3] << 32)),
				(unsigned long long)((u64)LO.UL[0] | ((u64)LO.UL[1] << 32)),
				(unsigned long long)((u64)LO.UL[2] | ((u64)LO.UL[3] << 32)));

			if (!strcmp(want, got))
				pass++;
			else
			{
				if (failures++ < 8)
					printf("  %s case %d:\n    console %s\n    ours    %s\n",
					       name, idx, want, got);
			}
			idx++;
		}
		fclose(f);
		printf("%s: %d/%d console cases\n", name, pass, idx);
		if (pass != idx)
			failures++;
	}

	if (failures == 0)
		printf("hwscore: MMI multiply-accumulate matches console on every case\n");
	return failures != 0;
}
