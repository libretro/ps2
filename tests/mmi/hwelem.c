/*  Score MMI element operations against console captures.
 *
 *  Companion to hwscore.c, which covers the multiply and divide family in
 *  muldiv.expected. This covers the element operations recorded in
 *  shuffle.expected, arithmetic.expected and logic.expected: the
 *  permutes, the saturating absolutes, PADSBH and the variable word
 *  shifts.
 *
 *  Operands are read from the capture lines themselves rather than
 *  transcribed from the test source. Every line names what it ran, either
 *  as literal integers ("pinth 0, 1:") or as constant names ("pinth
 *  C_ZERO, C_ONE:"), so a name table plus the printed integers is enough
 *  to reconstruct the inputs. hwscore.c does transcribe operand order and
 *  that cost real time: the two harness families there use different
 *  immediate lists, and driving both from one table shifted every divide
 *  case by one and reported correct semantics as broken. Reading the
 *  operands out of the capture cannot drift from the file it is scoring.
 *
 *  Usage:
 *    tests/mmi/hwelem <dir-with-the-.expected-files>
 *
 *  Exit status is non-zero if any case disagrees with the console.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t  s32; typedef int16_t s16; typedef uint16_t u16;

typedef struct { u32 UL[4]; } Q;

static Q lit(u32 a, u32 b, u32 c, u32 d)
{ Q q; q.UL[0]=a; q.UL[1]=b; q.UL[2]=c; q.UL[3]=d; return q; }

/* The named constants, from tests/cpu/ee_simd/shared.h. */
static const struct { const char* name; Q v; } kConst[] = {
	{ "C_ZERO",          {{0,0,0,0}} },
	{ "C_S16_MAX",       {{0x7FFFu,0,0,0}} },
	{ "C_S16_MIN",       {{0xFFFF8000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_S32_MAX",       {{0x7FFFFFFFu,0,0,0}} },
	{ "C_S32_MIN",       {{0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_S64_MAX",       {{0xFFFFFFFFu,0x7FFFFFFFu,0,0}} },
	{ "C_S64_MIN",       {{0,0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_NEGONE",        {{0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_ONEONEONEONE",  {{1,1,1,1}} },
	{ "C_ONE",           {{1,0,0,0}} },
	{ "C_GARBAGE1",      {{0x1337u,0x1338u,0x1339u,0x133Au}} },
	{ "C_GARBAGE2",      {{0xDEADBEEFu,0xDEADBEEEu,0xDEADBEEDu,0xDEADBEECu}} },
	{ "C_P_S8_A",        {{0x80808080u,0xFFFFFFFFu,0x12345678u,0x7F7F7F7Fu}} },
	{ "C_P_S8_B",        {{0x80FF127Fu,0xFF807F34u,0x567F80FFu,0x7F78FF80u}} },
	{ "C_P_S16_A",       {{0x80008000u,0xFFFFFFFFu,0x12345678u,0x7FFF7FFFu}} },
	{ "C_P_S16_B",       {{0x8000FFFFu,0x12347FFFu,0xFFFF8000u,0x7FFF5678u}} },
	{ "C_P_S32_A",       {{0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu}} },
	{ "C_P_S32_B",       {{0xFFFFFFFFu,0x80000000u,0x7FFFFFFFu,0x12345678u}} },
	{ "C_P_S32_C",       {{0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu}} },
	{ "C_P_S32_D",       {{0x7FFFFFFFu,0x12345678u,0xFFFFFFFFu,0x80000000u}} },
	{ "C_P_S32_E",       {{0x00000000u,0x7FFFFFFFu,0xFFFFFFFFu,0x80000000u}} },
	{ "C_P_S32_F",       {{0xFFFFFFFFu,0x80000000u,0x00000000u,0x7FFFFFFFu}} },
	{ "C_TWOTWOTWOTWO",  {{2,2,2,2}} },
};

/* SET_U32<i>: lui/ori then pcpyld, so both doublewords are the sign-extended
 * immediate. Longest name first so C_ONE does not shadow C_ONEONEONEONE. */
static int operand(const char* tok, Q* out)
{
	size_t i, best = (size_t)-1, bestlen = 0;
	char* end;
	long long v;

	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(tok, kConst[i].name, n) && n > bestlen)
		{ best = i; bestlen = n; }
	}
	if (best != (size_t)-1) { *out = kConst[best].v; return 1; }

	v = strtoll(tok, &end, 10);
	if (end == tok) return 0;
	{
		const u32 w = (u32)v;
		const u32 s = (w & 0x80000000u) ? 0xFFFFFFFFu : 0u;
		*out = lit(w, s, w, s);
	}
	return 1;
}

/* Semantics, transcribed from pcsx2/MMI.cpp. */
static void pabsw(const Q* rt, Q* rd)
{ int n; for (n = 0; n < 4; n++) {
	if (rt->UL[n] == 0x80000000u) rd->UL[n] = 0x7FFFFFFFu;
	else { const s32 v = (s32)rt->UL[n]; rd->UL[n] = (u32)(v < 0 ? -v : v); } } }

static void pabsh(const Q* rt, Q* rd)
{ int n; for (n = 0; n < 8; n++) {
	const u16 u = (u16)(rt->UL[n/2] >> ((n & 1) * 16));
	u16 r;
	if (u == 0x8000u) r = 0x7FFFu;
	else { const s16 v = (s16)u; r = (u16)(v < 0 ? -v : v); }
	rd->UL[n/2] = (rd->UL[n/2] & ~(0xFFFFu << ((n & 1) * 16)))
	            | ((u32)r << ((n & 1) * 16)); } }

static void padsbh(const Q* rs, const Q* rt, Q* rd)
{ int n; for (n = 0; n < 8; n++) {
	const u16 a = (u16)(rs->UL[n/2] >> ((n & 1) * 16));
	const u16 b = (u16)(rt->UL[n/2] >> ((n & 1) * 16));
	const u16 r = (u16)(n < 4 ? (a - b) : (a + b));
	rd->UL[n/2] = (rd->UL[n/2] & ~(0xFFFFu << ((n & 1) * 16)))
	            | ((u32)r << ((n & 1) * 16)); } }

static void shiftv(const Q* rs, const Q* rt, Q* rd, int kind)
{ int i; for (i = 0; i < 2; i++) {
	const int ss = i * 2;
	const u32 amt = rs->UL[ss] & 0x1F;
	u32 r;
	if (kind == 0) r = rt->UL[ss] << amt;
	else if (kind == 1) r = rt->UL[ss] >> amt;
	else r = (u32)((s32)rt->UL[ss] >> amt);
	rd->UL[ss] = r;
	rd->UL[ss+1] = (r & 0x80000000u) ? 0xFFFFFFFFu : 0u; } }

static void permute(const Q* rs, const Q* rt, Q* rd, const int* idx, int count, int word)
{
	u32 t[8]; int i;
	for (i = 0; i < count; i++)
	{
		const int e = idx[i];
		const Q* src = (e >= 8) ? rs : rt;
		const int j = (e >= 8) ? e - 8 : e;
		t[i] = word ? src->UL[j] : (u32)(u16)(src->UL[j/2] >> ((j & 1) * 16));
	}
	for (i = 0; i < count; i++)
	{
		if (word) rd->UL[i] = t[i];
		else rd->UL[i/2] = (rd->UL[i/2] & ~(0xFFFFu << ((i & 1) * 16)))
		                 | (t[i] << ((i & 1) * 16));
	}
}

static void pext5(const Q* rt, Q* rd)
{ int n; for (n = 0; n < 4; n++) {
	const u32 v = rt->UL[n];
	rd->UL[n] = ((v & 0x0000001Fu) << 3) | ((v & 0x000003E0u) << 6)
	          | ((v & 0x00007C00u) << 9) | ((v & 0x00008000u) << 16); } }

enum { A_PABSW, A_PABSH, A_PADSBH, A_PSLLVW, A_PSRLVW, A_PSRAVW,
       A_PEXT5, A_PEXEW, A_PEXCW, A_PROT3W, A_PEXEH, A_PEXCH,
       A_PINTH, A_PINTEH, A_COUNT };
/* `swap` marks the ops whose printed operand order is the reverse of the
 * architectural one. The variable shifts assemble as PSLLVW rd, rt, rs --
 * value first, shift amount second -- while the test source names its C
 * variables rs, rt and prints them in that order, so the first operand on
 * the line is the value. PINTH and PADSBH take their operands in the
 * architectural order and need no swap. */
static const struct { const char* name; const char* file; int two; int swap; } kOps[A_COUNT] = {
	{ "pabsw",  "arithmetic", 0, 0 }, { "pabsh",  "arithmetic", 0, 0 },
	{ "padsbh", "arithmetic", 1, 0 },
	{ "psllvw", "logic", 1, 1 }, { "psrlvw", "logic", 1, 1 },
	{ "psravw", "logic", 1, 1 },
	{ "pext5",  "shuffle", 0, 0 },
	{ "pexew",  "shuffle", 0, 0 }, { "pexcw",  "shuffle", 0, 0 },
	{ "prot3w", "shuffle", 0, 0 }, { "pexeh",  "shuffle", 0, 0 },
	{ "pexch",  "shuffle", 0, 0 },
	{ "pinth",  "shuffle", 1, 0 }, { "pinteh", "shuffle", 1, 0 },
};

static void apply(int op, const Q* rs, const Q* rt, Q* rd)
{
	static const int kPexew[4]  = { 2, 1, 0, 3 };
	static const int kPexcw[4]  = { 0, 2, 1, 3 };
	static const int kProt3w[4] = { 1, 2, 0, 3 };
	static const int kPexeh[8]  = { 2, 1, 0, 3, 6, 5, 4, 7 };
	static const int kPexch[8]  = { 0, 2, 1, 3, 4, 6, 5, 7 };
	static const int kPinth[8]  = { 0, 8+4, 1, 8+5, 2, 8+6, 3, 8+7 };
	static const int kPinteh[8] = { 0, 8+0, 2, 8+2, 4, 8+4, 6, 8+6 };

	switch (op)
	{
	case A_PABSW:  pabsw(rt, rd); break;
	case A_PABSH:  pabsh(rt, rd); break;
	case A_PADSBH: padsbh(rs, rt, rd); break;
	case A_PSLLVW: shiftv(rs, rt, rd, 0); break;
	case A_PSRLVW: shiftv(rs, rt, rd, 1); break;
	case A_PSRAVW: shiftv(rs, rt, rd, 2); break;
	case A_PEXT5:  pext5(rt, rd); break;
	case A_PEXEW:  permute(rs, rt, rd, kPexew, 4, 1); break;
	case A_PEXCW:  permute(rs, rt, rd, kPexcw, 4, 1); break;
	case A_PROT3W: permute(rs, rt, rd, kProt3w, 4, 1); break;
	case A_PEXEH:  permute(rs, rt, rd, kPexeh, 8, 0); break;
	case A_PEXCH:  permute(rs, rt, rd, kPexch, 8, 0); break;
	case A_PINTH:  permute(rs, rt, rd, kPinth, 8, 0); break;
	case A_PINTEH: permute(rs, rt, rd, kPinteh, 8, 0); break;
	}
}

int main(int argc, char** argv)
{
	const char* dir = (argc > 1) ? argv[1] : ".";
	int op, failures = 0, totalcases = 0;

	for (op = 0; op < A_COUNT; op++)
	{
		char path[512], head[64], buf[512];
		FILE* f;
		int inblock = 0, pass = 0, cases = 0;

		snprintf(path, sizeof(path), "%s/%s.expected", dir, kOps[op].file);
		f = fopen(path, "r");
		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOps[op].name);

		while (fgets(buf, sizeof(buf), f))
		{
			char* colon; char* args; Q rs, rt, rd; unsigned w3,w2,w1,w0;
			char want[128], got[128];

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head))) inblock = 1; continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;
			colon = strstr(buf, ": ");
			if (!colon) continue;
			if (sscanf(colon + 2, "%x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;

			/* "  <op> <a>[, <b>]: ..." -- skip the two leading spaces and the
			 * mnemonic, then read one or two operands. */
			args = buf + 2 + strlen(kOps[op].name);
			while (*args == ' ') args++;
			*colon = '\0';
			rs = lit(0,0,0,0);
			if (kOps[op].two)
			{
				char* comma = strchr(args, ',');
				if (!comma) { *colon = ':'; continue; }
				*comma = '\0';
				if (!operand(args, &rs)) { *colon = ':'; continue; }
				args = comma + 1;
				while (*args == ' ') args++;
			}
			if (!operand(args, &rt)) { *colon = ':'; continue; }
			*colon = ':';
			if (kOps[op].swap) { const Q t = rs; rs = rt; rt = t; }

			rd = lit(0x1337u, 0x1338u, 0x1339u, 0x133Au);
			apply(op, &rs, &rt, &rd);

			snprintf(want, sizeof(want), "%08x %08x %08x %08x", w3, w2, w1, w0);
			snprintf(got,  sizeof(got),  "%08x %08x %08x %08x",
			         rd.UL[3], rd.UL[2], rd.UL[1], rd.UL[0]);
			if (!strcmp(want, got)) pass++;
			else if (failures++ < 8)
				printf("  %s case %d:\n    console %s\n    ours    %s\n",
				       kOps[op].name, cases, want, got);
			cases++;
		}
		fclose(f);
		printf("%-8s %2d/%2d console cases\n", kOps[op].name, pass, cases);
		totalcases += cases;
		if (pass != cases) failures++;
	}

	printf("hwelem: %d cases across %d ops\n", totalcases, A_COUNT);
	return failures != 0;
}
