/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#pragma once

#include "GS/Renderers/Common/GSTexture.h"

/* GSTextureSW
 *
 * A GSTexture backed by a CPU-side pixel buffer. Used by GSDeviceSW
 * (the no-HW-context fallback for the SW renderer) and by no other
 * code path. Storage is plain aligned-allocated bytes; Update is a
 * memcpy; Map returns the buffer directly.
 *
 * Only Color (RGBA8) format is meaningfully supported - the SW
 * renderer's m_output buffer and all final composite/present output
 * is XRGB8888. Other formats are accepted at construction (to satisfy
 * the GSDevice interface) but the storage is treated as opaque bytes
 * and shouldn't be sampled from outside the SW device. */
class GSTextureSW final : public GSTexture
{
	/* The table this texture answers through is in its own .cpp
	 * (gs_texture_ops, Common/GSTexture.h). */
	friend struct GSTextureSW_ops_access;
private:
	u8* m_data = nullptr;
	int m_pitch = 0;
	bool m_mapped = false;

public:
	GSTextureSW(Type type, int width, int height, Format format);
	~GSTextureSW();

	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0);
	bool Map(GSMap& m, const GSVector4i* r = NULL, int layer = 0);
	void Unmap();

	__fi u8* GetPointer() const { return m_data; }
	__fi int GetPitch() const { return m_pitch; }
};
