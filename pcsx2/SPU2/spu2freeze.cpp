/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

#include "Global.h"
#include <memalign.h>
#include "spu2.h" // hopefully temporary, until I resolve lClocks depdendency
#include "../IopMem.h"

/* Arbitrary ID to identify SPU2 saves. */
#define SPU2_SAVE_ID 0x1227521

/* Incremented whenever the savestate layout changes. */
#define SPU2_SAVE_VERSION 0x000f

/* What the block has to be aligned to for the members inside it. */
#define SPU2_SAVE_ALIGN 64

struct SPU2Savestate_DataBlock
{
	u32 spu2id;          // SPU2 state identifier lets ZeroGS/PeopsSPU2 know this isn't their state)
	u8 unkregs[0x10000]; // SPU2 raw register memory
	u8 mem[0x200000];    // SPU2 raw sample memory

	u32 version; // SPU2 version identifier
	V_Core Cores[2];
	V_SPDIF Spdif;
	u16 OutPos;
	u16 InputPos;
	u32 Cycles;
	u32 lClocks;
	int PlayMode;
};

/* The block carries V_Core, whose Voices[] is 64-byte aligned. */
#ifdef __cplusplus
static_assert(alignof(struct SPU2Savestate_DataBlock) == SPU2_SAVE_ALIGN,
              "SPU2_SAVE_ALIGN no longer matches the block");
#endif

static void FreezeItImpl(struct SPU2Savestate_DataBlock *spud)
{
	spud->spu2id = SPU2_SAVE_ID;
	spud->version = SPU2_SAVE_VERSION;

	memcpy(spud->unkregs, spu2regs, sizeof(spud->unkregs));
	memcpy(spud->mem, _spu2mem, sizeof(spud->mem));

	memcpy(spud->Cores, Cores, sizeof(Cores));
	memcpy(&spud->Spdif, &Spdif, sizeof(Spdif));

	// Convert pointers to offsets so we can safely restore them when loading.
	// We use -1 for null, and anything else as an offset from iop memory.
#define FIX_POINTER(x) \
	if (!(x)) \
		(x) = (u16*)(uintptr_t)-1; \
	else \
		(x) = (u16*)(uintptr_t)((const u8*)(x) - &iopMem->Main[0])

	for (u32 i = 0; i < 2; i++)
	{
		V_Core *core = &spud->Cores[i];
		FIX_POINTER(core->DMAPtr);
		FIX_POINTER(core->DMARPtr);
	}

#undef FIX_POINTER

	spud->OutPos = OutPos;
	spud->InputPos = InputPos;
	spud->Cycles = Cycles;
	spud->lClocks = lClocks;
	spud->PlayMode = PlayMode;

	// note: Don't save the cache.  PCSX2 doesn't offer a safe method of predicting
	// the required size of the savestate prior to saving, plus this is just too
	// "implementation specific" for the intended spec of a savestate.  Let's just
	// force the user to rebuild their cache instead.
}

static s32 ThawItImpl(struct SPU2Savestate_DataBlock *spud)
{
	if (spud->spu2id != SPU2_SAVE_ID || spud->version < SPU2_SAVE_VERSION)
	{
		// Do *not* reset the cores.
		// We'll need some "hints" as to how the cores should be initialized, and the
		// only way to get that is to use the game's existing core settings and hope
		// they kinda match the settings for the savestate (IRQ enables and such).

		// adpcm cache : Clear all the cache flags and buffers.
		memset(pcm_cache_data, 0, pcm_BlockCount * sizeof(PcmCacheEntry));
	}
	else
	{
		//TODO/FIXME - implement this?
		//SndBuffer::ClearContents();

		memcpy(spu2regs, spud->unkregs, sizeof(spud->unkregs));
		memcpy(_spu2mem, spud->mem, sizeof(spud->mem));

		memcpy(Cores, spud->Cores, sizeof(Cores));
		memcpy(&Spdif, &spud->Spdif, sizeof(Spdif));

		// Reverse the pointer offset from above.
#define FIX_POINTER(x) \
	if ((x) == (u16*)(uintptr_t)-1) \
		(x) = NULL; \
	else \
		(x) = (u16*)(&iopMem->Main[0] + (size_t)(x))

		for (u32 i = 0; i < 2; i++)
		{
			V_Core *core = &Cores[i];
			FIX_POINTER(core->DMAPtr);
			FIX_POINTER(core->DMARPtr);
		}

#undef FIX_POINTER

		OutPos = spud->OutPos;
		InputPos = spud->InputPos;
		Cycles = spud->Cycles;
		lClocks = spud->lClocks;
		PlayMode = spud->PlayMode;

		memset(pcm_cache_data, 0, pcm_BlockCount * sizeof(PcmCacheEntry));

		// Go through the V_Voice structs and recalculate SBuffer pointer from
		// the NextA setting.

		for (int c = 0; c < 2; c++)
		{
			for (int v = 0; v < 24; v++)
			{
				const int cacheIdx = Cores[c].Voices[v].NextA / pcm_WordsPerBlock;
				Cores[c].Voices[v].SBuffer = pcm_cache_data[cacheIdx].Sampledata;
			}
		}
	}
	return 0;
}


/* The caller hands us a DataBlock reference formed by casting into the
 * frontend's savestate blob, which carries no alignment guarantee -
 * DataBlock embeds V_Core (alignas(64)), so accessing members through
 * such a reference is UB (UBSan: 34 misaligned V_Core reports across
 * freeze/thaw).  When the blob happens to be aligned, run in place;
 * otherwise bounce through an aligned heap temp.  If the temp cannot
 * be allocated, fall back to in-place access: on every ABI we target
 * that behaves as before (x86 tolerates the misalignment), which
 * beats losing the savestate. */
void SPU2Savestate_FreezeIt(struct SPU2Savestate_DataBlock *spud)
{
	if (((uintptr_t)spud & (SPU2_SAVE_ALIGN - 1)) == 0)
	{
		FreezeItImpl(spud);
		return;
	}
	struct SPU2Savestate_DataBlock *tmp = (struct SPU2Savestate_DataBlock *)memalign_alloc(SPU2_SAVE_ALIGN, sizeof(struct SPU2Savestate_DataBlock));
	if (!tmp)
	{
		FreezeItImpl(spud);
		return;
	}
	FreezeItImpl(tmp);
	memcpy((void *)spud, tmp, sizeof(struct SPU2Savestate_DataBlock));
	memalign_free(tmp);
}

s32 SPU2Savestate_ThawIt(struct SPU2Savestate_DataBlock *spud)
{
	if (((uintptr_t)spud & (SPU2_SAVE_ALIGN - 1)) == 0)
		return ThawItImpl(spud);
	struct SPU2Savestate_DataBlock *tmp = (struct SPU2Savestate_DataBlock *)memalign_alloc(SPU2_SAVE_ALIGN, sizeof(struct SPU2Savestate_DataBlock));
	if (!tmp)
		return ThawItImpl(spud);
	memcpy(tmp, (const void *)spud, sizeof(struct SPU2Savestate_DataBlock));
	const s32 ret = ThawItImpl(tmp);
	memalign_free(tmp);
	return ret;
}

s32 SPU2Savestate_SizeIt(void)
{
	return sizeof(struct SPU2Savestate_DataBlock);
}
