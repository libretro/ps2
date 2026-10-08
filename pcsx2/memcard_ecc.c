/* See memcard_ecc.h. C89.
 * ECC code ported from mymc (public domain):
 * https://sourceforge.net/p/mymc-opl/code/ci/master/tree/ps2mc_ecc.py */
#include <string.h>

#include <streams/file_stream.h>

#include "memcard_ecc.h"

static const uint8_t parity_table[256] = {
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0
};

static const uint8_t column_parity_mask[256] = {
	0, 7, 22, 17, 37, 34, 51, 52, 52, 51, 34, 37, 17, 22, 7, 0,
	67, 68, 85, 82, 102, 97, 112, 119, 119, 112, 97, 102, 82, 85, 68, 67,
	82, 85, 68, 67, 119, 112, 97, 102, 102, 97, 112, 119, 67, 68, 85, 82,
	17, 22, 7, 0, 52, 51, 34, 37, 37, 34, 51, 52, 0, 7, 22, 17,
	97, 102, 119, 112, 68, 67, 82, 85, 85, 82, 67, 68, 112, 119, 102, 97,
	34, 37, 52, 51, 7, 0, 17, 22, 22, 17, 0, 7, 51, 52, 37, 34,
	51, 52, 37, 34, 22, 17, 0, 7, 7, 0, 17, 22, 34, 37, 52, 51,
	112, 119, 102, 97, 85, 82, 67, 68, 68, 67, 82, 85, 97, 102, 119, 112,
	112, 119, 102, 97, 85, 82, 67, 68, 68, 67, 82, 85, 97, 102, 119, 112,
	51, 52, 37, 34, 22, 17, 0, 7, 7, 0, 17, 22, 34, 37, 52, 51,
	34, 37, 52, 51, 7, 0, 17, 22, 22, 17, 0, 7, 51, 52, 37, 34,
	97, 102, 119, 112, 68, 67, 82, 85, 85, 82, 67, 68, 112, 119, 102, 97,
	17, 22, 7, 0, 52, 51, 34, 37, 37, 34, 51, 52, 0, 7, 22, 17,
	82, 85, 68, 67, 119, 112, 97, 102, 102, 97, 112, 119, 67, 68, 85, 82,
	67, 68, 85, 82, 102, 97, 112, 119, 119, 112, 97, 102, 82, 85, 68, 67,
	0, 7, 22, 17, 37, 34, 51, 52, 52, 51, 34, 37, 17, 22, 7, 0
};

uint32_t memcard_ecc(const uint8_t *buf)
{
	unsigned column_parity = 0x77;
	unsigned line_parity_0 = 0x7F;
	unsigned line_parity_1 = 0x7F;
	unsigned i;

	for (i = 0; i < 128; i++)
	{
		const uint8_t b = buf[i];
		column_parity ^= column_parity_mask[b];
		if (parity_table[b])
		{
			line_parity_0 ^= ~i;
			line_parity_1 ^= i;
		}
	}
	return (column_parity & 0xFF) | ((line_parity_0 & 0xFF) << 8)
	     | ((line_parity_1 & 0xFF) << 16);
}

/* Opens both files; on failure neither is left open. */
static int open_pair(const char *file_in, const char *file_out,
		RFILE **fin, RFILE **fout)
{
	*fin = filestream_open(file_in, RETRO_VFS_FILE_ACCESS_READ,
			RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!*fin)
		return 0;
	*fout = filestream_open(file_out, RETRO_VFS_FILE_ACCESS_WRITE,
			RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!*fout)
	{
		filestream_close(*fin);
		return 0;
	}
	return 1;
}

/* Closes both; the output is flushed first. ok is what the copy came to. */
static int close_pair(RFILE *fin, RFILE *fout, int ok)
{
	filestream_close(fin);
	if (ok && filestream_flush(fout) != 0)
		ok = 0;
	if (filestream_close(fout) != 0)
		ok = 0;
	return ok;
}

int memcard_noecc_to_raw(const char *file_in, const char *file_out)
{
	uint8_t page[MEMCARD_PAGE_RAW];
	RFILE  *fin, *fout;
	int64_t pages, i;
	int     ok = 1;

	if (!open_pair(file_in, file_out, &fin, &fout))
		return 0;
	pages = filestream_get_size(fin) / MEMCARD_PAGE_DATA;
	memset(page + MEMCARD_PAGE_DATA, 0, MEMCARD_PAGE_RAW - MEMCARD_PAGE_DATA);
	for (i = 0; ok && i < pages; i++)
	{
		int j;
		if (filestream_read(fin, page, MEMCARD_PAGE_DATA) != MEMCARD_PAGE_DATA)
		{
			ok = 0;
			break;
		}
		for (j = 0; j < 4; j++)
		{
			const uint32_t ecc = memcard_ecc(page + j * 128);
			uint8_t *e = page + MEMCARD_PAGE_DATA + j * 3;
			e[0] = (uint8_t)ecc;
			e[1] = (uint8_t)(ecc >> 8);
			e[2] = (uint8_t)(ecc >> 16);
		}
		if (filestream_write(fout, page, MEMCARD_PAGE_RAW) != MEMCARD_PAGE_RAW)
			ok = 0;
	}
	return close_pair(fin, fout, ok);
}

int memcard_raw_to_noecc(const char *file_in, const char *file_out)
{
	uint8_t page[MEMCARD_PAGE_RAW];
	RFILE  *fin, *fout;
	int64_t pages, i;
	int     ok = 1;

	if (!open_pair(file_in, file_out, &fin, &fout))
		return 0;
	pages = filestream_get_size(fin) / MEMCARD_PAGE_RAW;
	for (i = 0; ok && i < pages; i++)
	{
		if (filestream_read(fin, page, MEMCARD_PAGE_RAW) != MEMCARD_PAGE_RAW
		 || filestream_write(fout, page, MEMCARD_PAGE_DATA) != MEMCARD_PAGE_DATA)
			ok = 0;
	}
	return close_pair(fin, fout, ok);
}
