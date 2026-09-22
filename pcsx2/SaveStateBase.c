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

/* The block reader and writer every subsystem's freeze entry point runs
 * through. It touches nothing of the emulator: hand it a buffer and a
 * direction and it moves bytes. The part that names the emulator's own
 * state -- FreezeBios, FreezeInternals -- stays in SaveState.cpp, which is
 * where the C++ globals it serialises live. */

#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>

#include "SaveState.h"

void SaveState_Init(SaveStateBase *s, u8 *memory, size_t size, size_t cap,
                    bool is_saving)
{
	s->memory      = memory;
	s->memory_size = size;
	s->memory_cap  = cap;
	s->idx         = 0;
	s->error       = false;
	s->is_saving   = is_saving;
	s->tagspace[0] = 0;
}

/* Grow geometrically, so a state built out of many small blocks does not
 * copy itself on every one. The vector this replaced grew the same way; the
 * difference is that realloc can extend in place, which a vector's
 * copy-then-free cannot. */
static bool savestate_reserve(SaveStateBase *s, size_t want)
{
	u8 *grown;
	size_t cap;

	if (want <= s->memory_cap)
		return true;

	cap = s->memory_cap ? s->memory_cap : 4096;
	while (cap < want)
		cap *= 2;

	grown = (u8 *)realloc(s->memory, cap);
	if (!grown)
	{
		s->error = true;
		return false;
	}

	s->memory     = grown;
	s->memory_cap = cap;
	return true;
}

void SaveState_PrepBlock(SaveStateBase *s, int size)
{
	size_t end;

	if (s->error)
		return;

	end = (size_t)s->idx + (size_t)size;
	if (s->is_saving)
	{
		if (!savestate_reserve(s, end))
			return;
		if (end > s->memory_size)
			s->memory_size = end;
	}
	else if (s->memory_size < end)
		s->error = true;
}

/* One FreezeMem for both directions. Saving grows the buffer and copies in;
 * loading copies out, and after an error it zero-fills rather than reading
 * past whatever went wrong. */
void SaveState_FreezeMem(SaveStateBase *s, void *data, int size)
{
	/* A negative size reaches here when a freeze function takes its length
	 * from the state it is loading and that length went past INT_MAX. Both
	 * branches below would turn it into an enormous memcpy. */
	if (size <= 0)
		return;

	if (s->is_saving)
	{
		const size_t end = (size_t)s->idx + (size_t)size;

		if (!savestate_reserve(s, end))
			return;
		if (end > s->memory_size)
			s->memory_size = end;

		memcpy(&s->memory[s->idx], data, size);
		s->idx += size;
		return;
	}

	if (s->error)
	{
		memset(data, 0, size);
		return;
	}

	/* The saving side above grows the buffer to fit. The loading side has
	 * no such freedom: the block is whatever the frontend handed us, and
	 * reading past it is reading past someone else's allocation. Nothing
	 * checked that until now, so a state that was truncated -- or that
	 * simply came from a build whose structures were larger -- ran off the
	 * end, and every freeze function that takes a length out of the state
	 * it is loading could ask for as much as it liked. PrepBlock has had
	 * this check all along; FreezeMem never called it.
	 *
	 * Failing the same way as any other error: the flag latches, so the
	 * rest of the load zero-fills rather than reading on from a cursor
	 * that is already past the end. */
	{
		const size_t end = (size_t)s->idx + (size_t)size;

		if (end > s->memory_size)
		{
			s->error = true;
			memset(data, 0, size);
			return;
		}
	}

	{
		const u8 * const src = &s->memory[s->idx];
		s->idx += size;
		memcpy(data, src, size);
	}
}

bool SaveState_FreezeTag(SaveStateBase *s, const char *src)
{
	if (s->error)
		return false;

	memset(s->tagspace, 0, sizeof(s->tagspace));
	strlcpy(s->tagspace, src, sizeof(s->tagspace));
	SaveState_Freeze(s, s->tagspace);

	if (strcmp(s->tagspace, src) != 0)
	{
		s->error = true;
		return false;
	}

	return true;
}
