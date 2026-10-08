/*  EE recompiler register-file width audit.
 *
 *  The arm64 EE recompiler reaches cpuRegs fields through RegsField(),
 *  and the register named in the access sets its width: a w register
 *  moves 32 bits, an x register 64. Nothing ties that choice to the
 *  field's declared type, so a 32-bit access to a u64 field compiles
 *  and runs. A narrow store drops the carry out of the low word -- the
 *  EE clock is a u64, and a block tail that adds its cycles through a
 *  w register sends the clock back 2^32 cycles at the crossing, after
 *  which every timer derives a count from a start cycle in the future.
 *  A wide store to a u32 field overwrites the field after it.
 *
 *  This reads the field types out of the cpuRegisters declaration in
 *  R5900.h and checks every Ldr/Str through RegsField(&cpuRegs.<field>)
 *  in the recompiler against them. Only top-level fields are checked;
 *  accesses into nested members (CP0.n.Status, GPR.r[n]) are skipped.
 *
 *  Usage: tests/ee/cyclewidth <path-to-R5900.h> <path-to-recR5900_arm64.cpp>
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 64
#define NAME_LEN   64

struct field
{
	char name[NAME_LEN];
	int  bits;
};

static struct field fields[MAX_FIELDS];
static int nfields;

static char* read_file(const char* path)
{
	FILE* f = fopen(path, "rb");
	long n;
	char* buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (char*)malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	if (buf)
		buf[n] = '\0';
	fclose(f);
	return buf;
}

static int type_bits(const char* t, size_t n)
{
	if ((n == 3 && !strncmp(t, "u64", 3)) || (n == 3 && !strncmp(t, "s64", 3)))
		return 64;
	if ((n == 3 && !strncmp(t, "u32", 3)) || (n == 3 && !strncmp(t, "s32", 3)) ||
	    (n == 3 && !strncmp(t, "int", 3)))
		return 32;
	return 0;
}

/* Collect "type name[, name...];" declarations of the scalar widths we
 * know, from the body of the cpuRegisters struct. Lines with any other
 * leading type (GPRregs, CP0regs, ...) are nested members and skipped. */
static int parse_fields(const char* hdr)
{
	const char* start = strstr(hdr, "typedef struct cpuRegisters");
	const char* end;
	const char* p;

	if (!start)
		return 0;
	end = strstr(start, "} cpuRegisters;");
	if (!end)
		return 0;
	p = strchr(start, '{');
	if (!p || p > end)
		return 0;
	p++;

	while (p < end)
	{
		const char* line_end = strchr(p, '\n');
		const char* q = p;
		const char* t;
		int bits;

		if (!line_end || line_end > end)
			line_end = end;
		while (q < line_end && isspace((unsigned char)*q))
			q++;
		t = q;
		while (q < line_end && (isalnum((unsigned char)*q) || *q == '_'))
			q++;
		bits = type_bits(t, (size_t)(q - t));
		if (bits)
		{
			/* names up to the ';', stopping at a comment */
			while (q < line_end && *q != ';' && !(q[0] == '/' && q[1] == '/'))
			{
				const char* n;
				while (q < line_end && !(isalpha((unsigned char)*q) || *q == '_'))
				{
					if (*q == ';' || (q[0] == '/' && q[1] == '/'))
						break;
					if (*q == '[')
						while (q < line_end && *q != ']')
							q++;
					q++;
				}
				if (q >= line_end || *q == ';' || (q[0] == '/' && q[1] == '/'))
					break;
				n = q;
				while (q < line_end && (isalnum((unsigned char)*q) || *q == '_'))
					q++;
				if (nfields < MAX_FIELDS && (size_t)(q - n) < NAME_LEN)
				{
					memcpy(fields[nfields].name, n, (size_t)(q - n));
					fields[nfields].name[q - n] = '\0';
					fields[nfields].bits = bits;
					nfields++;
				}
			}
		}
		p = line_end + 1;
	}
	return nfields;
}

static int field_bits(const char* name, size_t n)
{
	int i;
	for (i = 0; i < nfields; i++)
		if (strlen(fields[i].name) == n && !strncmp(fields[i].name, name, n))
			return fields[i].bits;
	return 0;
}

int main(int argc, char** argv)
{
	static const char kNeedle[] = "RegsField(&cpuRegs.";
	char* hdr;
	char* src;
	const char* p;
	int checked = 0, bad = 0, wide_fields = 0, i;

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s <R5900.h> <recR5900_arm64.cpp>\n", argv[0]);
		return 2;
	}
	hdr = read_file(argv[1]);
	src = read_file(argv[2]);
	if (!hdr || !src)
	{
		fprintf(stderr, "cannot read %s\n", !hdr ? argv[1] : argv[2]);
		free(hdr);
		free(src);
		return 2;
	}
	if (!parse_fields(hdr))
	{
		fprintf(stderr, "no cpuRegisters fields found in %s\n", argv[1]);
		free(hdr);
		free(src);
		return 2;
	}
	for (i = 0; i < nfields; i++)
		if (fields[i].bits == 64)
			wide_fields++;

	for (p = strstr(src, kNeedle); p; p = strstr(p + 1, kNeedle))
	{
		const char* name = p + sizeof(kNeedle) - 1;
		const char* ne = name;
		const char* ls = p;
		const char* op;
		const char* r;
		int bits, line = 1;
		const char* c;

		while (isalnum((unsigned char)*ne) || *ne == '_')
			ne++;
		if (*ne != ')') /* nested member: CP0.n.Status, GPR.r[n] */
			continue;
		bits = field_bits(name, (size_t)(ne - name));
		if (!bits)
			continue;

		while (ls > src && ls[-1] != '\n')
			ls--;
		/* in a comment: not code */
		for (c = ls; c + 1 < p; c++)
			if (c[0] == '/' && (c[1] == '/' || c[1] == '*'))
				break;
		if (c + 1 < p)
			continue;
		/* the nearest load or store opening before the operand */
		op = NULL;
		for (c = p; c - ls >= 4 && !op; c--)
			if (!strncmp(c - 4, "Ldr(", 4) || !strncmp(c - 4, "Str(", 4))
				op = c - 4;
		if (!op)
			continue; /* not a load or store: an address computation */
		r = op + 4;
		while (*r == ' ')
			r++;
		for (c = src; c < p; c++)
			if (*c == '\n')
				line++;
		checked++;

		if ((*r == 'w' && bits == 64) || (*r == 'x' && bits == 32))
		{
			printf("  line %d: %c-register access to %d-bit cpuRegs.%.*s\n", line, *r, bits,
				(int)(ne - name), name);
			bad++;
		}
	}

	printf("  %d cpuRegs fields (%d of them 64-bit), %d accesses checked\n", nfields, wide_fields,
		checked);
	free(hdr);
	free(src);
	if (!wide_fields || !checked)
	{
		printf("FAIL: nothing to check -- has the declaration or the access helper moved?\n");
		return 1;
	}
	if (bad)
	{
		printf("FAIL: %d access(es) of the wrong width\n", bad);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
