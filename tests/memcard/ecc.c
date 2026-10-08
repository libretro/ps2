/* Memory card images with and without ECC, on the real memcard_ecc.c.
 *
 *   - the ECC of a 128-byte block is the one its definition gives: the
 *     tables are rebuilt here from the parities they stand for;
 *   - a .bin image converted to raw is its pages, each followed by the
 *     ECC of its quarters and four zero bytes, and converted back is the
 *     image it was; a trailing part page is dropped;
 *   - a conversion that cannot open its output fails, and leaves no file
 *     open: run often enough to use up every handle the process has, and
 *     a conversion after it still works.
 * Off Windows the handle limit is lowered first, so that takes a few
 * hundred runs rather than whatever the shell allows; Windows' C runtime
 * has 512 streams as it is. C89. Usage: memcard_ecc <scratch dir> */
#ifndef _WIN32
#define _XOPEN_SOURCE 500
#include <sys/resource.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <streams/file_stream.h>

#include "memcard_ecc.h"

#define PAGES 37
#define FAILED_OPENS 5000

static int s_failures;

static unsigned parity(unsigned v)
{
	unsigned p = 0;
	while (v)
	{
		p ^= v & 1;
		v >>= 1;
	}
	return p;
}

/* The ECC as mymc defines it, tables built from the parities. */
static unsigned ref_ecc(const unsigned char *buf)
{
	unsigned col = 0x77, l0 = 0x7F, l1 = 0x7F, i;
	for (i = 0; i < 128; i++)
	{
		const unsigned b = buf[i];
		col ^= parity(b & 0x55) | parity(b & 0x33) << 1 | parity(b & 0x0F) << 2
		     | parity(b & 0xAA) << 4 | parity(b & 0xCC) << 5 | parity(b & 0xF0) << 6;
		if (parity(b))
		{
			l0 ^= ~i;
			l1 ^= i;
		}
	}
	return (col & 0xFF) | (l0 & 0xFF) << 8 | (l1 & 0xFF) << 16;
}

static unsigned s_seed = 12345;
static unsigned char rnd(void)
{
	s_seed = s_seed * 1103515245u + 12345u;
	return (unsigned char)(s_seed >> 16);
}

static void check(int ok, const char *what)
{
	if (!ok)
	{
		printf("  FAIL: %s\n", what);
		s_failures++;
	}
}

static long put_file(const char *path, const unsigned char *data, long size)
{
	FILE *f = fopen(path, "wb");
	long n;
	if (!f)
		return -1;
	n = (long)fwrite(data, 1, (size_t)size, f);
	fclose(f);
	return n;
}

static unsigned char *get_file(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	unsigned char *data;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = (unsigned char*)malloc(*size ? (size_t)*size : 1);
	if (data && fread(data, 1, (size_t)*size, f) != (size_t)*size)
	{
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

int main(int argc, char **argv)
{
	static char bin[1024], raw[1024], back[1024], nowhere[1024];
	const long bin_size = PAGES * MEMCARD_PAGE_DATA + 100;
	unsigned char *image, *got;
	unsigned char block[128];
	long size;
	int i, j, mismatches = 0;

	if (argc < 2)
	{
		printf("usage: memcard_ecc <scratch dir>\n");
		return 2;
	}
	sprintf(bin, "%s/ecc_card.bin", argv[1]);
	sprintf(raw, "%s/ecc_card.binx", argv[1]);
	sprintf(back, "%s/ecc_card_back.bin", argv[1]);
	sprintf(nowhere, "%s/no_such_dir/ecc_card.binx", argv[1]);

	/* The ECC against its definition. */
	for (i = 0; i < 4096; i++)
	{
		for (j = 0; j < 128; j++)
			block[j] = (i < 256) ? (unsigned char)i : rnd();
		if (memcard_ecc(block) != ref_ecc(block))
			mismatches++;
	}
	check(!mismatches, "the ECC differs from its definition");

	/* Round trip. */
	image = (unsigned char*)malloc((size_t)bin_size);
	for (i = 0; i < bin_size; i++)
		image[i] = rnd();
	check(put_file(bin, image, bin_size) == bin_size, "cannot write the scratch image");
	check(memcard_noecc_to_raw(bin, raw), "converting to raw fails");
	got = get_file(raw, &size);
	check(got && size == PAGES * MEMCARD_PAGE_RAW, "the raw image is not whole pages of 528");
	if (got && size == PAGES * MEMCARD_PAGE_RAW)
	{
		int bad = 0;
		for (i = 0; i < PAGES; i++)
		{
			const unsigned char *pg = got + i * MEMCARD_PAGE_RAW;
			const unsigned char *src = image + i * MEMCARD_PAGE_DATA;
			if (memcmp(pg, src, MEMCARD_PAGE_DATA))
				bad++;
			for (j = 0; j < 4; j++)
			{
				const unsigned e = ref_ecc(src + j * 128);
				const unsigned char *eb = pg + MEMCARD_PAGE_DATA + j * 3;
				if (eb[0] != (e & 0xFF) || eb[1] != ((e >> 8) & 0xFF) || eb[2] != (e >> 16))
					bad++;
			}
			for (j = MEMCARD_PAGE_DATA + 12; j < MEMCARD_PAGE_RAW; j++)
				if (pg[j])
					bad++;
		}
		check(!bad, "a raw page is not its data, its ECC and four zeros");
	}
	free(got);
	check(memcard_raw_to_noecc(raw, back), "converting back fails");
	got = get_file(back, &size);
	check(got && size == PAGES * MEMCARD_PAGE_DATA
			&& !memcmp(got, image, PAGES * MEMCARD_PAGE_DATA),
			"converting back does not give the image");
	free(got);

	/* An output that cannot be opened: fails, and keeps no handle. */
#ifndef _WIN32
	{
		struct rlimit lim;
		if (!getrlimit(RLIMIT_NOFILE, &lim) && (lim.rlim_cur == RLIM_INFINITY || lim.rlim_cur > 256))
		{
			lim.rlim_cur = 256;
			setrlimit(RLIMIT_NOFILE, &lim);
		}
	}
#endif
	for (i = 0; i < FAILED_OPENS; i++)
		if (memcard_noecc_to_raw(bin, nowhere) || memcard_raw_to_noecc(raw, nowhere))
			break;
	check(i == FAILED_OPENS, "a conversion to an unopenable output succeeds");
	check(memcard_noecc_to_raw(bin, raw), "after failed conversions there are no handles left");

	remove(bin);
	remove(raw);
	remove(back);
	free(image);
	printf(s_failures ? "memcard ECC: FAILED (%d)\n" : "memcard ECC: ok\n", s_failures);
	return s_failures != 0;
}
