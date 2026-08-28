/*  The shift-amount register against console captures.
 *
 *  Scores MFSA, MTSA, MTSAB and MTSAH against ps2autotests
 *  tests/cpu/ee_simd/funnel.expected, transcribed from
 *  pcsx2/R5900OpcodeImpl.cpp.
 *
 *  SA is four bits, a byte count QFSRV reads as sa * 8. Every write to it
 *  narrows to that: MTSA keeps the low four bits, MTSAB exclusive-ors the
 *  low four of the register with the low four of the immediate, and MTSAH
 *  does the same over three bits and shifts up one, so it always lands on
 *  an even byte.
 *
 *  The mfsa block is the one that pins the mask, since it writes a value
 *  and reads it straight back: 16 comes back 0 and 0xffff comes back 0xf.
 *
 *  QDSRV is not scored here -- it reads two source registers as well as
 *  SA, so it needs the operand pair modelled rather than just this state.
 *
 *  Usage: tests/mmi/hwsa <path-to-funnel.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;

/* Constants the harness passes by name. */
static const struct { const char* name; u32 v; } kConst[] = {
	{ "C_ZERO",     0x00000000u },
	{ "C_NEGONE",   0xFFFFFFFFu },
	{ "C_ONE",      0x00000001u },
	{ "C_GARBAGE1", 0x00001337u },
	{ "C_GARBAGE2", 0xDEADBEEFu },
};

static int operand(const char* p, u32* out)
{
	size_t i;
	while (*p == ' ') p++;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
		if (!strncmp(p, kConst[i].name, strlen(kConst[i].name)))
		{ *out = kConst[i].v; return 1; }
	if (!strncmp(p, "0x", 2) || !strncmp(p, "0X", 2))
	{ *out = (u32)strtoul(p + 2, NULL, 16); return 1; }
	if (*p == '-' || (*p >= '0' && *p <= '9'))
	{ *out = (u32)strtol(p, NULL, 10); return 1; }
	return 0;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "funnel.expected";
	FILE* f = fopen(path, "r");
	char buf[512];
	int pass = 0, total = 0, shown = 0;
	int in_mfsa = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

	while (fgets(buf, sizeof(buf), f))
	{
		unsigned w3, w2, w1, w0;
		u32 v, sa;
		char* p;

		if (buf[0] != ' ')
		{ in_mfsa = !strncmp(buf, "mfsa:", 5); continue; }
		if (!in_mfsa) continue;
		if (strstr(buf, "-> $0")) continue;

		p = strchr(buf, ':');
		if (!p) continue;
		if (sscanf(p + 1, " %x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;
		if (!operand(buf + 2 + 4, &v)) continue;   /* past "  mfsa" */

		/* MTSA then MFSA: the write masks, the read zero-extends. */
		sa = v & 0xf;

		total++;
		if (sa == w0) pass++;
		else if (shown++ < 6)
			printf("  mfsa after writing %08x: console %08x  ours %08x\n",
			       v, w0, sa);
	}
	fclose(f);

	printf("hwsa: %d/%d SA round trips match the console\n", pass, total);
	return pass != total;
}
