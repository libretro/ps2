/* IsoFS path lookup on the real IsoFS.cpp and IsoFile.cpp, against a
 * disc built here: files in the root, one and two directories down,
 * with and without the device prefix, a path that is only separators,
 * and a directory whose extent lies past the end of the image. C89. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libretro.h>

#define SECTOR  2048
#define SECTORS 32

int isofs_harness_find(const char* path, unsigned* lba, unsigned* size);

static unsigned char disc[SECTORS][SECTOR];
static unsigned failures;

void isofs_harness_log(enum retro_log_level level, const char* fmt, ...)
{
	(void)level; (void)fmt;
}

int isofs_harness_read(unsigned char* buffer, int lba)
{
	if (lba < 0 || lba >= SECTORS)
		return 0;
	memcpy(buffer, disc[lba], SECTOR);
	return 1;
}

static void put_both32(unsigned char* p, unsigned v)
{
	p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
	p[4] = p[3]; p[5] = p[2]; p[6] = p[1]; p[7] = p[0];
}

/* One directory record at p; returns its length. A one-byte name of 0
 * or 1 is "." or "..". */
static int record(unsigned char* p, const char* name, int name_len,
	unsigned lba, unsigned size, int is_dir)
{
	int len = 33 + name_len;
	if (len & 1)
		len++;
	memset(p, 0, (size_t)len);
	p[0] = (unsigned char)len;
	put_both32(p + 2, lba);
	put_both32(p + 10, size);
	p[25] = (unsigned char)(is_dir ? 2 : 0);
	p[32] = (unsigned char)name_len;
	memcpy(p + 33, name, (size_t)name_len);
	return len;
}

static void directory(int lba, int parent, const char** names, const unsigned* lbas,
	const unsigned* sizes, const int* dirs, int n)
{
	unsigned char* p = disc[lba];
	int i;
	p += record(p, "\0", 1, (unsigned)lba, SECTOR, 1);
	p += record(p, "\1", 1, (unsigned)parent, SECTOR, 1);
	for (i = 0; i < n; i++)
		p += record(p, names[i], (int)strlen(names[i]), lbas[i], sizes[i], dirs[i]);
}

static void build_disc(void)
{
	static const char* root_names[] = {"SUB", "FAR", "SYSTEM.CNF;1"};
	static const unsigned root_lbas[] = {21, 1000, 24};
	static const unsigned root_sizes[] = {SECTOR, SECTOR, 100};
	static const int root_dirs[] = {1, 1, 0};
	static const char* sub_names[] = {"DEEP", "INNER.ELF;1"};
	static const unsigned sub_lbas[] = {22, 25};
	static const unsigned sub_sizes[] = {SECTOR, 300};
	static const int sub_dirs[] = {1, 0};
	static const char* deep_names[] = {"BOTTOM.BIN;1"};
	static const unsigned deep_lbas[] = {26};
	static const unsigned deep_sizes[] = {400};
	static const int deep_dirs[] = {0};
	unsigned char* pvd = disc[16];

	memset(disc, 0, sizeof(disc));
	pvd[0] = 1;
	memcpy(pvd + 1, "CD001", 5);
	record(pvd + 156, "\0", 1, 20, SECTOR, 1);
	disc[17][0] = 0xff;
	memcpy(disc[17] + 1, "CD001", 5);

	directory(20, 20, root_names, root_lbas, root_sizes, root_dirs, 3);
	directory(21, 20, sub_names, sub_lbas, sub_sizes, sub_dirs, 2);
	directory(22, 21, deep_names, deep_lbas, deep_sizes, deep_dirs, 1);
}

static void expect(const char* path, int found, unsigned lba, unsigned size)
{
	unsigned got_lba = 0, got_size = 0;
	const int r = isofs_harness_find(path, &got_lba, &got_size);

	if (r < 0)
	{
		printf("  FAIL: the root directory did not open\n");
		failures++;
		return;
	}
	if (r != found || (found && (got_lba != lba || got_size != size)))
	{
		printf("  FAIL: %s: found %d at %u size %u, wanted %d at %u size %u\n",
			path, r, got_lba, got_size, found, lba, size);
		failures++;
	}
}

int main(void)
{
	build_disc();

	expect("\\SYSTEM.CNF;1", 1, 24, 100);
	expect("cdrom0:\\SYSTEM.CNF;1", 1, 24, 100);
	expect("\\SUB\\INNER.ELF;1", 1, 25, 300);
	expect("cdrom0:\\SUB\\INNER.ELF;1", 1, 25, 300);
	expect("\\SUB\\DEEP\\BOTTOM.BIN;1", 1, 26, 400);
	expect("/SUB/DEEP/BOTTOM.BIN;1", 1, 26, 400);
	expect("\\SUB\\MISSING;1", 0, 0, 0);
	expect("\\INNER.ELF;1", 0, 0, 0);
	expect("\\SYSTEM.CNF;1\\X", 0, 0, 0);
	expect("\\", 0, 0, 0);
	expect("//", 0, 0, 0);
	expect("\\FAR\\ANYTHING;1", 0, 0, 0);

	if (failures)
	{
		printf("isofs: FAILED (%u)\n", failures);
		return 1;
	}
	printf("isofs: ok\n");
	return 0;
}
