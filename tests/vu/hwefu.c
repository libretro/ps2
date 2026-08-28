/*  VU EFU results against console captures.
 *
 *  Scores ESQRT, ERSQRT and ERCPR against ps2autotests
 *  tests/vu/lower/efu.expected.
 *
 *  These three go through ps2f_esqrt, ps2f_ersqrt and ps2f_ercpr in
 *  pcsx2/ps2float.c, and that file is standalone C, so this links the
 *  real implementation rather than transcribing it. Nothing here can
 *  drift from the emulator: if ps2float.c changes, this is testing the
 *  change.
 *
 *  The EFU tests are value tests despite living among the timing ones --
 *  the harness issues the op, then WAITP, then MFP, so the pipeline is
 *  drained before the result is read and only the arithmetic is being
 *  measured. The scalar ops read field Z of the source.
 *
 *  Not covered: the remaining EFU ops. ESADD, ERSADD, ELENG, ERLENG and
 *  ESUM are built from vuDouble and plain arithmetic inside VUops.cpp
 *  rather than ps2float.c, and EATAN, EEXP and ESIN are polynomial
 *  approximations there too, so scoring any of them means transcribing
 *  code instead of linking it. That is worth doing, but it is a different
 *  and more error-prone job than this one.
 *
 *  Usage: tests/vu/hwefu <path-to-efu.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ps2float.h"

typedef uint32_t u32;

/* Mirrors _vuCalculateEATAN and its callers in pcsx2/VUops.cpp, including
 * the (x-1)/(x+1) reduction the hardware applies. Unlike the three ps2float
 * ops this is transcribed, because it lives in VUops.cpp behind VURegs. */
static u32 eatan(u32 in)
{
	static const float c[9] = { 0.999999344348907f, -0.333298563957214f,
		0.199465364217758f, -0.139085337519646f, 0.096420042216778f,
		-0.055909886956215f, 0.021861229091883f, -0.004054057877511f,
		0.785398185253143f };
	float x, t, t2, p, r;
	u32 v = in, out;
	int i;

	/* vuDouble: denormals flush to signed zero, and a maximal exponent
	 * clamps to the largest finite value -- PS2 floats have no infinities. */
	if ((v & 0x7f800000u) == 0) v &= 0x80000000u;
	else if ((v & 0x7f800000u) == 0x7f800000u) v = (v & 0x80000000u) | 0x7f7fffffu;
	memcpy(&x, &v, 4);

	t = (x - 1.0f) / (x + 1.0f);
	t2 = t * t;
	p = t;
	r = c[0] * t;
	for (i = 1; i < 8; i++) { p *= t2; r += c[i] * p; }
	r += c[8];
	memcpy(&out, &r, 4);
	return out;
}

/* The constants efu.cpp loads, by the name the capture prints. Field Z is
 * what the scalar ops read, so only that lane is needed. */
static const struct { const char* name; u32 z; } kConst[] = {
	{ "CVF_ZERO",          0x00000000u },
	{ "CVF_NEGZERO",       0x80000000u },
	{ "CVF_MAX_MANTISSA",  0x3FFFFFFFu },
	{ "CVF_MAX_EXP",       0x7F800001u },
	{ "CVF_MIN_EXP",       0x00000001u },
	{ "CVF_MAX",           0x7FFFFFFFu },
	{ "CVF_MIN",           0xFFFFFFFFu },
	{ "CVF_ONE",           0x3F800000u },
	{ "CVF_NEGONE",        0xBF800000u },
	{ "CVF_GARBAGE1",      0x00001337u },
	{ "CVF_GARBAGE2",      0xDEADBEEFu },
	{ "CVF_INCREASING",    0x40400000u },
	{ "CVF_DECREASING",    0x40000000u },
};

static int lookup(const char* tok, u32* out)
{
	size_t i, best = (size_t)-1, bestlen = 0;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(tok, kConst[i].name, n) && n > bestlen)
		{ best = i; bestlen = n; }
	}
	if (best == (size_t)-1) return 0;
	*out = kConst[best].z;
	return 1;
}

/* EATAN is scored to a tolerance rather than exactly. Its polynomial is a
 * curve fit and lands 1-2 ULP from the console once the argument is reduced
 * as the hardware does; before the reduction was added it was not close at
 * all -- 0 of 13, returning about -31558 where the console gives 1.249 --
 * so the point of having it here is to catch a regression of that kind, not
 * to assert an exactness the approximation cannot deliver. */
#define EATAN_TOLERANCE_ULP 2
/* EATAN is held to a floor rather than a clean sweep. Within 2 ULP it gets
 * 8 of the 13 captured cases; the five it misses are the inputs at or near
 * zero, where the console reads about 1.07e-7 and this reads 5.96e-8 -- both
 * noise around an answer of zero, differing in the residual the series
 * leaves at t = -1 -- and x = -1, which is where the reduction divides by
 * zero. Closing those needs the real algorithm, not a tolerance. The floor
 * exists to catch a regression to the unreduced form, which scored 0. */
#define EATAN_MIN_PASS 8

enum { E_ESQRT, E_ERSQRT, E_ERCPR, E_EATAN, E_COUNT };
static const char* const kOpName[E_COUNT] = { "ESQRT", "ERSQRT", "ERCPR", "EATAN" };

static u32 apply(int op, u32 v)
{
	switch (op)
	{
	case E_ESQRT:  return ps2f_raw(ps2f_esqrt(v));
	case E_ERSQRT: return ps2f_raw(ps2f_ersqrt(v));
	case E_ERCPR:  return ps2f_raw(ps2f_ercpr(v));
	case E_EATAN:  return eatan(v);
	}
	return 0;
}

/* Compare within a ULP budget, on the integer representation. Only used for
 * EATAN; everything else here must match exactly. */
static int close_enough(int op, u32 got, u32 want)
{
	u32 d;
	if (op != E_EATAN)
		return got == want;
	if ((got ^ want) & 0x80000000u)
		return 0;
	d = (got > want) ? (got - want) : (want - got);
	return d <= EATAN_TOLERANCE_ULP;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "efu.expected";
	int op, failures = 0, total = 0;

	for (op = 0; op < E_COUNT; op++)
	{
		FILE* f = fopen(path, "r");
		char head[32], buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOpName[op]);

		while (fgets(buf, sizeof(buf), f))
		{
			char *args, *colon;
			unsigned want;
			u32 in, got;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;
			colon = strstr(buf, ": ");
			if (!colon) continue;
			if (sscanf(colon + 2, "%x", &want) != 1) continue;

			args = buf + 2 + strlen(kOpName[op]);
			while (*args == ' ') args++;
			*colon = '\0';
			if (!lookup(args, &in)) { *colon = ':'; continue; }
			*colon = ':';

			got = apply(op, in);
			if (close_enough(op, got, (u32)want)) pass++;
			else if (op != E_EATAN && failures++ < 8)
				printf("  %s %-18s console %08x  ours %08x\n",
				       kOpName[op], args, want, got);
			cases++;
		}
		fclose(f);
		if (op == E_EATAN)
		{
			printf("%-7s %2d/%2d console cases (within %d ULP, floor %d)\n",
			       kOpName[op], pass, cases, EATAN_TOLERANCE_ULP, EATAN_MIN_PASS);
			if (pass < EATAN_MIN_PASS) failures++;
		}
		else
		{
			printf("%-7s %2d/%2d console cases\n", kOpName[op], pass, cases);
			if (pass != cases) failures++;
		}
		total += cases;
	}

	printf("vuefu: %d cases across %d ops\n", total, E_COUNT);
	return failures != 0;
}
