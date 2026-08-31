/* zran.c -- example of zlib/gzip stream indexing and random access

Copyright (C) 2005, 2012 Mark Adler

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

Jean-loup Gailly        Mark Adler
jloup@gzip.org          madler@alumni.caltech.edu


The data format used by the zlib library is described by RFCs (Request for
Comments) 1950 to 1952 in the files http://tools.ietf.org/html/rfc1950
(zlib format), rfc1951 (deflate format) and rfc1952 (gzip format).
*/

/* zran.c -- example of zlib/gzip stream indexing and random access
 * Copyright (C) 2005, 2012 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
   Version 1.1  29 Sep 2012  Mark Adler */

/* Version History:
 1.0  29 May 2005  First version
 1.1  29 Sep 2012  Fix memory reallocation error

 1.1+ 16 Apr 2014  PCSX2 adaptation
    (This is verbatim copy from zlib/examples/zran.c, with the following mods):
  - Added an explicit license clause taken from zlib.h and removed the sample main(...).
  - zlib include path and included Pcsx2Types.h for windows off_t (s64)
  - fseeko and off_t #define'ed for windows too (on windows off_t is 32b and no fseeko)
  - typedefs for struct access/point (Access/Point) and allocation type casts
  - access: added members span and uncompressed_size which are filled by build_index.
  - point and access packed for safety since they go to disk as is (but no endian-ness handling).
      But they're still aligned since each member size is multiple of 4, so no perf issues.
  - extract: added state import/export for instant sequential access regardless of index
      (Thanks to Mark Adler for suggesting the approach)
  - build_index(...) - added progress prints
  - CHUNK changed from 16k to 512k
 */

/* Illustrate the use of Z_BLOCK, inflatePrime(), and inflateSetDictionary()
   for random access of a compressed file.  A file containing a zlib or gzip
   stream is provided on the command line.  The compressed stream is decoded in
   its entirety, and an index built with access points about every SPAN bytes
   in the uncompressed output.  The compressed file is left open, and can then
   be read randomly, having to decompress on the average SPAN/2 uncompressed
   bytes before getting to the desired block of data.

   An access point can be created at the start of any deflate block, by saving
   the starting file offset and bit of that block, and the 32K bytes of
   uncompressed data that precede that block.  Also the uncompressed offset of
   that block is saved to provide a referece for locating a desired starting
   point in the uncompressed stream.  build_index() works by decompressing the
   input zlib or gzip stream a block at a time, and at the end of each block
   deciding if enough uncompressed data has gone by to justify the creation of
   a new access point.  If so, that point is saved in a data structure that
   grows as needed to accommodate the points.

   To use the index, an offset in the uncompressed data is provided, for which
   the latest accees point at or preceding that offset is located in the index.
   The input file is positioned to the specified location in the index, and if
   necessary the first few bits of the compressed data is read from the file.
   inflate is initialized with those bits and the 32K of uncompressed data, and
   the decompression then proceeds until the desired offset in the file is
   reached.  Then the decompression continues to read the desired uncompressed
   data from the file.

   Another approach would be to generate the index on demand.  In that case,
   requests for random access reads from the compressed data would try to use
   the index, but if a read far enough past the end of the index is required,
   then further index entries would be generated and added.

   There is some fair bit of overhead to starting inflation for the random
   access, mainly copying the 32K byte dictionary.  So if small pieces of the
   file are being accessed, it would make sense to implement a cache to hold
   some lookahead and avoid many calls to extract() for small lengths.

   Another way to build an index would be to use inflateCopy().  That would
   not be constrained to have access points at block boundaries, but requires
   more memory per access point, and also cannot be saved to file due to the
   use of pointers in the state.  The approach here allows for storage of the
   index in a file.
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <encodings/deflate.h>

/* zlib-compatible status values kept for the existing callers */
#define ZIDX_OK          0
#define ZIDX_ERRNO     (-1)
#define ZIDX_DATA_ERROR (-3)
#define ZIDX_MEM_ERROR  (-4)

#include "HostFS.h"

//#define SPAN (1048576L)  /* desired distance between access points */
#define WINSIZE 32768U    /* sliding window size */
#define CHUNK (64 * 1024) /* file input buffer size */

#ifdef _WIN32
#pragma pack(push, indexData, 1)
#endif

/* access point entry */
struct point
{
	s64 out;                  /* corresponding offset in uncompressed data */
	s64 in;                   /* offset in input file of first full byte */
	int bits;                      /* number of bits (1-7) from byte at in - 1, or 0 */
	unsigned char window[WINSIZE]; /* preceding 32K of uncompressed data */
}
#ifndef _WIN32
__attribute__((packed))
#endif
;

typedef struct point Point;

/* access point list */
struct access
{
	int have;           /* number of list entries filled in */
	int size;           /* number of list entries allocated (only used internally during build)*/
	struct point* list; /* allocated list */

	s32 span;                   /* once the index is built, holds the span size used to build it */
	s64 uncompressed_size; /* filled by build_index */
}
#ifndef _WIN32
__attribute__((packed))
#endif
;

typedef struct access Access;

#ifdef _WIN32
#pragma pack(pop, indexData)
#endif

/* Deallocate an index built by build_index() */
static inline void free_index(struct access* index)
{
	if (index != NULL)
	{
		free(index->list);
		free(index);
	}
}

/* Add an entry to the access point list.  If out of memory, deallocate the
   existing list and return NULL. */
static inline struct access* addpoint(struct access* index, int bits,
							  s64 in, s64 out, unsigned left, unsigned char* window)
{
	struct point* next;

	/* if list is empty, create it (start with eight points) */
	if (index == NULL)
	{
		index = (Access*)malloc(sizeof(struct access));
		if (index == NULL)
			return NULL;
		index->list = (Point*)malloc(sizeof(struct point) << 3);
		if (index->list == NULL)
		{
			free(index);
			return NULL;
		}
		index->size = 8;
		index->have = 0;
	}

	/* if list is full, make it bigger */
	else if (index->have == index->size)
	{
		index->size <<= 1;
		next = (Point*)realloc(index->list, sizeof(struct point) * index->size);
		if (next == NULL)
		{
			free_index(index);
			return NULL;
		}
		index->list = next;
	}

	/* fill in entry and increment how many we have */
	next = index->list + index->have;
	next->bits = bits;
	next->in = in;
	next->out = out;
	if (left)
		memcpy(next->window, window + WINSIZE - left, left);
	if (left < WINSIZE)
		memcpy(next->window + left, window, WINSIZE - left);
	index->have++;

	/* return list, possibly reallocated */
	return index;
}

/* Make one entire pass through the compressed stream and build an index, with
   access points about every span bytes of uncompressed output -- span is
   chosen to balance the speed of random access against the memory requirements
   of the list, about 32K bytes per access point.  Note that data after the end
   of the first zlib or gzip stream in the file is ignored.  build_index()
   returns the number of access points on success (>= 1), Z_MEM_ERROR for out
   of memory, Z_DATA_ERROR for an error in the input file, or Z_ERRNO for a
   file read error.  On success, *built points to the resulting index. */
static inline int build_index(RFILE* in, s64 span, struct access** built)
{
	/* rinflate port of the zran builder: stop-at-block reporting plus
	   the exact bit tell replace Z_BLOCK/data_type, and the decoder's
	   own gzip/zlib auto header handling (window_bits 47) replaces
	   inflateInit2(47).  The access-point encoding is unchanged - (in,
	   bits, out, window) mean exactly what they meant - so indexes
	   remain compatible with what the extractor expects. */
	int ret = ZIDX_OK;
	s64 totout, last;
	s64 chunk_base;              /* file offset of the current input chunk */
	struct access* index = NULL;
	void* z;
	unsigned char input[CHUNK];
	unsigned char window[WINSIZE];
	size_t in_avail = 0, in_used = 0;
	size_t win_fill = 0;         /* valid bytes in window (current cycle) */
	int done = 0;

	z = rinflate_new(47); /* automatic zlib or gzip decoding */
	if (!z)
		return ZIDX_MEM_ERROR;
	rinflate_set_stop_at_block(z, 1);

	totout = last = 0;
	chunk_base = 0;
	rinflate_set_out(z, window, WINSIZE);

	while (!done)
	{
		if (in_used == in_avail)
		{
			chunk_base = filestream_tell(in);
			in_avail = rfread(input, 1, CHUNK, in);
			in_used = 0;
			if (rferror(in))
			{
				ret = ZIDX_ERRNO;
				goto build_index_error;
			}
			if (in_avail == 0)
			{
				ret = ZIDX_DATA_ERROR;
				goto build_index_error;
			}
			rinflate_set_in(z, input, in_avail);
		}

		{
			size_t rd = 0, wr = 0;
			const int st = rinflate_process(z, &rd, &wr);
			in_used += rd;
			totout  += (s64)wr;
			win_fill += wr;

			if (st == RDEFLATE_PROCESS_ERROR)
			{
				ret = ZIDX_DATA_ERROR;
				goto build_index_error;
			}

			if (win_fill == WINSIZE)
			{
				rinflate_set_out(z, window, WINSIZE);
				win_fill = 0;
			}

			if (st == RDEFLATE_PROCESS_END)
			{
				done = 1;
			}
			else if (st == RDEFLATE_PROCESS_BLOCK &&
					(totout == 0 || totout - last > span))
			{
				/* Exact boundary: absolute bit position, then the same
				   (in, bits) encoding zran used - in is the next full
				   byte, bits the tail of the byte before it that
				   belongs to the next block. */
				const u64 abs_bits = (u64)chunk_base * 8 + rinflate_tell_bits(z);
				const s64 in_off   = (s64)((abs_bits + 7) / 8);
				const int bits     = (int)((8 - (abs_bits & 7)) & 7);
				index = addpoint(index, bits, in_off, totout,
						(uint)(WINSIZE - win_fill), window);
				if (index == NULL)
				{
					ret = ZIDX_MEM_ERROR;
					goto build_index_error;
				}
				last = totout;
			}
		}
	}

	if (index == NULL)
	{
		rinflate_free(z);
		return 0;
	}

	rinflate_free(z);
	index->list = (Point*)realloc(index->list, sizeof(struct point) * index->have);
	index->size = index->have;
	index->span = span;
	index->uncompressed_size = totout;
	*built = index;
	return index->have;

build_index_error:
	rinflate_free(z);
	if (index != NULL)
		free_index(index);
	return ret;
}

typedef struct zstate
{
	s64 out_offset;
	s64 in_offset;
	void* strm; /* rinflate raw stream, live between sequential extracts */
	int isValid;
} Zstate;

static inline void zstate_free_strm(zstate* state)
{
	if (state && state->strm)
	{
		rinflate_free(state->strm);
		state->strm = nullptr;
	}
}

static inline s64 getInOffset(zstate* state)
{
	return state->in_offset;
}

/* Use the index to read len bytes from offset into buf, return bytes read or
   negative for error (Z_DATA_ERROR or Z_MEM_ERROR).  If data is requested past
   the end of the uncompressed data, then extract() will return a value less
   than len, indicating how much as actually read into buf.  This function
   should not return a data error unless the file was modified since the index
   was generated.  extract() may also return Z_ERRNO if there is an error on
   reading or seeking the input file. */
static inline int extract(RFILE* in, struct access* index, s64 offset,
				  unsigned char* buf, int len, zstate* state)
{
	int ret = ZIDX_OK, skip;
	struct point* here;
	unsigned char input[CHUNK];
	unsigned char discard[WINSIZE];
	int isEnd = 0;
	size_t in_avail = 0, in_used = 0;
	size_t out_size = 0, out_done = 0;
	unsigned char* out_ptr = nullptr;

	if (len < 0 || state == nullptr)
		return 0;

	if (state->isValid && offset != state->out_offset)
	{
		zstate_free_strm(state);
		state->isValid = 0;
	}
	state->out_offset = offset;

	if (state->isValid)
	{
		/* sequential continuation: the live stream is positioned right
		   where the previous extract stopped */
		state->isValid = 0;
		filestream_seek(in, state->in_offset, RETRO_VFS_SEEK_POSITION_START);
		offset = 0;
		skip = 1;
	}
	else
	{
		here = index->list;
		ret = index->have;
		while (--ret && here[1].out <= offset)
			here++;

		zstate_free_strm(state);
		state->strm = rinflate_new(-15); /* raw inflate */
		if (!state->strm)
			return ZIDX_MEM_ERROR;
		ret = filestream_seek(in, here->in - (here->bits ? 1 : 0), RETRO_VFS_SEEK_POSITION_START);
		if (ret == -1)
		{
			ret = ZIDX_ERRNO;
			goto extract_ret;
		}
		if (here->bits)
		{
			/* the boundary is mid-byte: feed from the byte before it
			   and discard the bits that belong to the previous block */
			rinflate_set_start_bit(state->strm, 8 - here->bits);
		}
		rinflate_set_dictionary(state->strm, here->window, WINSIZE);

		offset -= here->out;
		skip = 1;
	}

	do
	{
		if (offset == 0 && skip)
		{
			out_ptr  = buf;
			out_size = (size_t)len;
			out_done = 0;
			rinflate_set_out(state->strm, out_ptr, out_size);
			skip = 0;
		}
		if (offset > WINSIZE)
		{
			out_ptr  = discard;
			out_size = WINSIZE;
			out_done = 0;
			rinflate_set_out(state->strm, out_ptr, out_size);
			offset -= WINSIZE;
		}
		else if (offset != 0)
		{
			out_ptr  = discard;
			out_size = (size_t)offset;
			out_done = 0;
			rinflate_set_out(state->strm, out_ptr, out_size);
			offset = 0;
		}

		for (;;)
		{
			if (in_used == in_avail)
			{
				state->in_offset = filestream_tell(in);
				in_avail = rfread(input, 1, CHUNK, in);
				in_used = 0;
				if (rferror(in))
				{
					ret = ZIDX_ERRNO;
					goto extract_ret;
				}
				if (in_avail == 0)
				{
					ret = ZIDX_DATA_ERROR;
					goto extract_ret;
				}
				rinflate_set_in(state->strm, input, in_avail);
			}

			{
				size_t rd = 0, wr = 0;
				const int st = rinflate_process(state->strm, &rd, &wr);
				in_used += rd;
				out_done += wr;
				state->in_offset += (s64)rd;
				if (st == RDEFLATE_PROCESS_ERROR)
				{
					ret = ZIDX_DATA_ERROR;
					goto extract_ret;
				}
				if (st == RDEFLATE_PROCESS_END)
				{
					isEnd = 1;
					break;
				}
			}
			if (out_done == out_size)
				break;
		}

		if (isEnd)
			break;
	} while (skip);

	ret = skip ? 0 : (int)out_done;
	if (out_ptr != buf)
		ret = 0;

extract_ret:
	if (!isEnd && ret == len && out_ptr == buf)
	{
		state->out_offset += len;
		state->isValid = 1;
	}
	else
		zstate_free_strm(state);

	return ret;
}
