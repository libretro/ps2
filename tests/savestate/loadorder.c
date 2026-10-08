/*  Savestate load order audit.
 *
 *  A state is applied block by block: registers, then the subsystems, then
 *  memory, SPU2, PAD and GS. One that is rejected partway has already
 *  replaced everything before the failing block, and a reader in error
 *  zero-fills every block after it, so carrying on runs the CPUs on wiped
 *  memory. retro_unserialize therefore snapshots the running machine with
 *  state_write before it calls state_read on the frontend's data, and on a
 *  failed read puts the snapshot back with state_read.
 *
 *  This checks that shape in libretro/main.cpp.
 *
 *  Usage: tests/savestate/loadorder <libretro/main.cpp>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const char* within(const char* s, const char* e, const char* needle)
{
	const char* p = strstr(s, needle);
	return (p && p < e) ? p : NULL;
}

int main(int argc, char** argv)
{
	char* src;
	const char *s, *e, *snap, *load, *restore;
	int bad = 0;

	if (argc != 2)
	{
		fprintf(stderr, "usage: %s <libretro/main.cpp>\n", argv[0]);
		return 2;
	}
	src = read_file(argv[1]);
	if (!src)
	{
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}

	s = strstr(src, "bool retro_unserialize(");
	e = s ? strstr(s, "\n}\n") : NULL;
	if (!s || !e)
	{
		printf("  retro_unserialize not found -- has it moved?\nFAIL\n");
		free(src);
		return 1;
	}
	snap = within(s, e, "state_write(");
	load = within(s, e, "state_read(data");
	restore = load ? within(load + 1, e, "state_read(") : NULL;

	if (!snap || !load || snap > load)
	{
		printf("  the running machine is not snapshotted before the load\n");
		bad++;
	}
	if (!restore)
	{
		printf("  a failed load does not restore the snapshot\n");
		bad++;
	}
	if (!within(s, e, "SaveState_FreezeInternals") && !within(src, src + strlen(src), "!SaveState_FreezeInternals(&loadme)"))
	{
		printf("  state_read does not stop at a rejected internals block\n");
		bad++;
	}

	free(src);
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad ? 1 : 0;
}
