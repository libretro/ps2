// See TextureDecompress.cpp for license info.

#pragma once

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4201) //  nonstandard extension used: nameless struct/union
#endif

#include <stdlib.h>
#include <stdint.h>
#include <algorithm>
#include <math.h>

enum BC4Mode
{
	BC4_UNORM = 0,
	BC4_SNORM = 1
};

enum BC5Mode
{
	BC5_UNORM = 0,
	BC5_SNORM = 1
};

void DecompressBlockBC1(uint32_t x, uint32_t y, uint32_t stride,
	const uint8_t* blockStorage, unsigned char* image);
void DecompressBlockBC2(uint32_t x, uint32_t y, uint32_t stride,
	const uint8_t* blockStorage, unsigned char* image);
void DecompressBlockBC3(uint32_t x, uint32_t y, uint32_t stride,
	const uint8_t* blockStorage, unsigned char* image);
void DecompressBlockBC4(uint32_t x, uint32_t y, uint32_t stride,
	enum BC4Mode mode, const uint8_t* blockStorage, unsigned char* image);
void DecompressBlockBC5(uint32_t x, uint32_t y, uint32_t stride,
	enum BC5Mode mode, const uint8_t* blockStorage, unsigned char* image);

namespace bc7decomp 
{

enum eNoClamp { cNoClamp };

template <typename S> inline S clamp(S value, S low, S high) { return (value < low) ? low : ((value > high) ? high : value); }

class color_rgba
{
public:
	union
	{
		uint8_t m_comps[4];

		struct
		{
			uint8_t r;
			uint8_t g;
			uint8_t b;
			uint8_t a;
		};
	};

	inline color_rgba() { }

	inline color_rgba(int y)
	{
		m_comps[0] = static_cast<uint8_t>(clamp<int>(y, 0, 255));
		m_comps[1] = m_comps[0];
		m_comps[2] = m_comps[0];
		m_comps[3] = 255;
	}

	inline color_rgba(int y, int na)
	{
		m_comps[0] = static_cast<uint8_t>(clamp<int>(y, 0, 255));
		m_comps[1] = m_comps[0];
		m_comps[2] = m_comps[0];
		m_comps[3] = static_cast<uint8_t>(clamp<int>(na, 0, 255));
	}

	inline color_rgba(int sr, int sg, int sb, int sa)
	{
		m_comps[0] = static_cast<uint8_t>(clamp<int>(sr, 0, 255));
		m_comps[1] = static_cast<uint8_t>(clamp<int>(sg, 0, 255));
		m_comps[2] = static_cast<uint8_t>(clamp<int>(sb, 0, 255));
		m_comps[3] = static_cast<uint8_t>(clamp<int>(sa, 0, 255));
	}

	inline color_rgba(eNoClamp, int sr, int sg, int sb, int sa)
	{
		set_noclamp_rgba((uint8_t)sr, (uint8_t)sg, (uint8_t)sb, (uint8_t)sa);
	}

	inline color_rgba &set_noclamp_rgba(int sr, int sg, int sb, int sa)
	{
		m_comps[0] = (uint8_t)sr;
		m_comps[1] = (uint8_t)sg;
		m_comps[2] = (uint8_t)sb;
		m_comps[3] = (uint8_t)sa;
		return *this;
	}

	inline const uint8_t &operator[] (uint32_t index) const { return m_comps[index]; }
	inline uint8_t &operator[] (uint32_t index) { return m_comps[index]; }
		
	inline void clear()
	{
		m_comps[0] = 0;
		m_comps[1] = 0;
		m_comps[2] = 0;
		m_comps[3] = 0;
	}

	inline bool operator== (const color_rgba &rhs) const
	{
		if (m_comps[0] != rhs.m_comps[0]) return false;
		if (m_comps[1] != rhs.m_comps[1]) return false;
		if (m_comps[2] != rhs.m_comps[2]) return false;
		if (m_comps[3] != rhs.m_comps[3]) return false;
		return true;
	}

	inline bool operator!= (const color_rgba &rhs) const
	{
		return !(*this == rhs);
	}

	inline bool operator<(const color_rgba &rhs) const
	{
		for (int i = 0; i < 4; i++)
		{
			if (m_comps[i] < rhs.m_comps[i])
				return true;
			else if (m_comps[i] != rhs.m_comps[i])
				return false;
		}
		return false;
	}
};

bool unpack_bc7(const void *pBlock, color_rgba *pPixels);

} // namespace bc7decomp

#ifdef _MSC_VER
#pragma warning(pop)
#endif
