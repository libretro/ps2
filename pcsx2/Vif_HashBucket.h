/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef VIF_HASHBUCKET_H
#define VIF_HASHBUCKET_H

#include <string.h>

#ifdef __cplusplus
#include "../common/Pcsx2Defs.h"
#include "../common/AlignedMalloc.h"
#else
/* C branch: fixed-width aliases from stdint (the u64 alias is why the
 * C89 gate runs with -Wno-long-long), and prototypes for the aligned
 * allocation trio, which lives in C++ translation units today. */
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
void* _aligned_malloc(size_t size, size_t align);
void _aligned_free(void* pmem);
void* pcsx2_aligned_realloc(void* handle, size_t new_size, size_t align, size_t old_size);
#ifndef safe_aligned_free
#define safe_aligned_free(ptr) ((void)(_aligned_free(ptr), (ptr) = NULL))
#endif
#endif

#if defined(_MSC_VER)
#define VIF_HASH_INLINE __forceinline
#elif defined(__GNUC__)
#define VIF_HASH_INLINE __inline__ __attribute__((always_inline))
#else
#define VIF_HASH_INLINE
#endif

/* nVifBlock - ordered for hashing; the 'num' and 'upkType' fields select
 * the bucket. The two views alias on purpose: the dynarec builds
 * hash_key/key0/key1 as packed words and reads the individual fields back
 * through 'f'.
 * Warning: the field order is baked into the newVifDynaRec code. */
typedef union nVifBlock
{
	struct
	{
		u8 num;          /* [00] Num Field */
		u8 upkType;      /* [01] Unpack Type [usn1:mask1:upk*4] */
		u16 length;      /* [02] Extra: pre computed Length */
		u32 mask;        /* [04] Mask Field */
		u8 mode;         /* [08] Mode Field */
		u8 aligned;      /* [09] Packet Alignment */
		u8 cl;           /* [10] CL Field */
		u8 wl;           /* [11] WL Field */
		u32 startOffset; /* [12] +1-biased offset of RecGen code in the rec reserve; 0 = empty cell */
	} f;

	struct
	{
		u16 hash_key;
		u16 _pad0;
		u32 key0;
		u32 key1;
		u32 value;
	} k;
} nVifBlock; /* 16 bytes -- 4 entries per 64B cache line during chain walks. */

/* 0x10000 buckets: the full [upkType*8:num*8] space, so the hashed key is
 * a plain 16-bit move and the bucket never needs a masking 'and'. */
#define VIF_HASH_SIZE 0x10000

/* A hash container built around nVifBlock: the bucket index is simply the
 * first two bytes of the block, so the most diverse data sits first in
 * the struct. Buckets are sentinel-terminated flat chains. */
typedef struct
{
	nVifBlock* bucket[VIF_HASH_SIZE];
} vif_hash_bucket_t;

/* The empty-cell sentinel must be tested before the key compare: it is
 * all-zero, so an all-zero key query (doMask=0 makes key0==0; key1==0
 * with mode/aligned/cl/wl all zero, e.g. STCYCL cl=0 wl=0) would
 * otherwise match the sentinel itself and hand the caller a block with
 * no code behind it.
 *
 * The caller passes key0|key1 packed into one 8-byte word, built in a
 * register from values it already holds; each chain entry is compared
 * with a single 8-byte load at +4, which stays inside the 16-byte entry
 * and its cache line on every supported target. */
static VIF_HASH_INLINE nVifBlock* vif_hash_find(vif_hash_bucket_t* h, u32 hash_key, u64 key)
{
	nVifBlock* chainpos = h->bucket[hash_key];
	for (;;)
	{
		u64 ckey;
		if (chainpos->f.startOffset == 0)
			return NULL;
		memcpy(&ckey, &chainpos->k.key0, sizeof(ckey));
		if (ckey == key)
			return chainpos;
		chainpos++;
	}
}

static VIF_HASH_INLINE u32 vif_hash_bucket_size(vif_hash_bucket_t* h, u32 hash_key)
{
	const nVifBlock* chainpos = h->bucket[hash_key];
	u32 size = 0;
	while (chainpos->f.startOffset != 0)
	{
		size++;
		chainpos++;
	}
	return size;
}

static void vif_hash_add(vif_hash_bucket_t* h, const nVifBlock* q)
{
	u32 b    = q->k.hash_key;
	u32 size = vif_hash_bucket_size(h, b);

	/* +1 for the sentinel already present, +1 for the new entry.
	 * 64B alignment keeps chain walks inside whole cache lines. */
	nVifBlock* new_bucket = (nVifBlock*)pcsx2_aligned_realloc(h->bucket[b],
			sizeof(nVifBlock) * (size + 2), 64, sizeof(nVifBlock) * (size + 1));
	if (!new_bucket)
		return;
	h->bucket[b] = new_bucket;
	/* Replace the sentinel with the new block and lay a fresh sentinel. */
	memcpy(&h->bucket[b][size], q, sizeof(nVifBlock));
	size++;
	memset(&h->bucket[b][size], 0, sizeof(nVifBlock));
}

static void vif_hash_clear(vif_hash_bucket_t* h)
{
	int i;
	for (i = 0; i < VIF_HASH_SIZE; i++)
	{
		if (h->bucket[i])
			safe_aligned_free(h->bucket[i]);
	}
}

static void vif_hash_reset(vif_hash_bucket_t* h)
{
	int i;
	vif_hash_clear(h);
	/* Allocate a lone sentinel for every bucket. */
	for (i = 0; i < VIF_HASH_SIZE; i++)
	{
		h->bucket[i] = (nVifBlock*)_aligned_malloc(sizeof(nVifBlock), 16);
		memset(h->bucket[i], 0, sizeof(nVifBlock));
	}
}

#endif /* VIF_HASHBUCKET_H */
