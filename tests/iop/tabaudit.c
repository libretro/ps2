/*  IOP opcode table audit.
 *
 *  Every other test in tests/iop calls the op implementations directly,
 *  which leaves one thing unchecked: whether the dispatch tables in
 *  pcsx2/R3000AOpcodeTables.cpp actually point each encoding at the right
 *  implementation. A swapped pair there passes every one of those tests
 *  and breaks every game.
 *
 *  This reads the tables out of the source and compares them against the
 *  MIPS I encoding, which is fixed and published, so there is a right
 *  answer to compare to rather than a plausible-looking one.
 *
 *  Entries the tree deliberately leaves unimplemented are checked too, as
 *  psxNULL: an encoding that should be NULL but is not, or the reverse,
 *  is exactly as wrong as a mismatched pair.
 *
 *  Run from anywhere; it takes the path to R3000AOpcodeTables.cpp.
 *
 *  Usage: tests/iop/tabaudit <path-to-R3000AOpcodeTables.cpp>
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* MIPS I primary opcodes, 0..63. NULL here means "not a MIPS I encoding
 * the IOP implements"; the R3000A has no COP1 and no 64-bit ops. */
static const char* const kBSC[64] = {
	"psxSPECIAL","psxREGIMM","psxJ","psxJAL","psxBEQ","psxBNE","psxBLEZ","psxBGTZ",
	"psxADDI","psxADDIU","psxSLTI","psxSLTIU","psxANDI","psxORI","psxXORI","psxLUI",
	"psxCOP0","psxNULL","psxCOP2","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxLB","psxLH","psxLWL","psxLW","psxLBU","psxLHU","psxLWR","psxNULL",
	"psxSB","psxSH","psxSWL","psxSW","psxNULL","psxNULL","psxSWR","psxNULL",
	"psxNULL","psxNULL","gteLWC2","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","gteSWC2","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL"
};

/* SPECIAL function field, 0..63. */
static const char* const kSPC[64] = {
	"psxSLL","psxNULL","psxSRL","psxSRA","psxSLLV","psxNULL","psxSRLV","psxSRAV",
	"psxJR","psxJALR","psxNULL","psxNULL","psxSYSCALL","psxBREAK","psxNULL","psxNULL",
	"psxMFHI","psxMTHI","psxMFLO","psxMTLO","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxMULT","psxMULTU","psxDIV","psxDIVU","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxADD","psxADDU","psxSUB","psxSUBU","psxAND","psxOR","psxXOR","psxNOR",
	"psxNULL","psxNULL","psxSLT","psxSLTU","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL"
};

/* REGIMM rt field, 0..31: the conditional branches on rs. */
static const char* const kREG[32] = {
	"psxBLTZ","psxBGEZ","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxBLTZAL","psxBGEZAL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL"
};

/* Pull one table's initialiser out of the file and split it on commas. */
static int read_table(const char* src, const char* decl, char out[][32], int n)
{
	const char* p = strstr(src, decl);
	const char* end;
	int count = 0;
	if (!p) return -1;
	p = strchr(p, '{');
	if (!p) return -1;
	end = strchr(p, '}');
	if (!end) return -1;
	p++;
	while (p < end && count < n)
	{
		const char* q;
		int len = 0;
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')) p++;
		if (p >= end) break;
		if (p[0] == '/' && p[1] == '/')            /* line comment */
		{ while (p < end && *p != '\n') p++; continue; }
		q = p;
		while (q < end && *q != ',' && *q != ' ' && *q != '\t' && *q != '\n') q++;
		len = (int)(q - p);
		if (len > 31) len = 31;
		memcpy(out[count], p, len);
		out[count][len] = '\0';
		count++;
		p = q;
	}
	return count;
}

static int check(const char* what, char got[][32], int got_n,
                 const char* const* want, int want_n)
{
	int i, bad = 0;
	if (got_n != want_n)
	{
		printf("  %s: read %d entries, expected %d\n", what, got_n, want_n);
		return 1;
	}
	for (i = 0; i < want_n; i++)
		if (strcmp(got[i], want[i]))
		{
			printf("  %s[%d]: table has %s, MIPS I says %s\n",
			       what, i, got[i], want[i]);
			bad++;
		}
	printf("%-10s %d/%d entries match the MIPS I encoding\n",
	       what, want_n - bad, want_n);
	return bad;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "R3000AOpcodeTables.cpp";
	FILE* f = fopen(path, "rb");
	char* src;
	long len;
	static char bsc[64][32], spc[64][32], reg[32][32];
	int bad = 0, n;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
	src = (char*)malloc((size_t)len + 1);
	if (fread(src, 1, (size_t)len, f) != (size_t)len) { fclose(f); return 2; }
	src[len] = '\0';
	fclose(f);

	n = read_table(src, "psxBSC[64])", bsc, 64);
	bad += (n < 0) ? (printf("  could not read psxBSC\n"), 1)
	               : check("psxBSC", bsc, n, kBSC, 64);
	n = read_table(src, "psxSPC[64])", spc, 64);
	bad += (n < 0) ? (printf("  could not read psxSPC\n"), 1)
	               : check("psxSPC", spc, n, kSPC, 64);
	n = read_table(src, "psxREG[32])", reg, 32);
	bad += (n < 0) ? (printf("  could not read psxREG\n"), 1)
	               : check("psxREG", reg, n, kREG, 32);

	free(src);
	printf("iop tabaudit: %s\n", bad ? "MISMATCHES FOUND" : "dispatch matches the encoding");
	return bad != 0;
}
