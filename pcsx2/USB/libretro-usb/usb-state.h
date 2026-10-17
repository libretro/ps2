// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <string.h>

/* Copies USB state into or out of a block of fixed size. A copy that
 * would run past the end copies nothing and marks the block bad. */
struct UsbState
{
	u8* data;
	size_t size;
	size_t pos;
	bool reading;
	bool error;

	void Bytes(void* p, size_t n)
	{
		if (error || n > size - pos)
		{
			error = true;
			return;
		}
		if (reading)
			memcpy(p, data + pos, n);
		else
			memcpy(data + pos, p, n);
		pos += n;
	}

	template <typename T>
	void Do(T* v)
	{
		Bytes(v, sizeof(T));
	}
};
