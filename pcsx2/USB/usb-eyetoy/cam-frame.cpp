// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "cam-frame.h"
#include "cam-jpeg.h"
#include "jo_mpeg.h"

#include <stdlib.h>
#include <string.h>

namespace usb_eyetoy
{
	static u8 Clamp(int v)
	{
		return static_cast<u8>(v < 0 ? 0 : v > 255 ? 255 : v);
	}

	void XrgbToRgb(const u32* src, int src_w, int src_h, size_t src_pitch, u8* dst, int dst_w, int dst_h, bool mirror)
	{
		for (int y = 0; y < dst_h; y++)
		{
			const u32* row = reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(src) + (y * src_h / dst_h) * src_pitch);
			for (int x = 0; x < dst_w; x++)
			{
				const u32 p = row[(mirror ? dst_w - 1 - x : x) * src_w / dst_w];
				*dst++ = static_cast<u8>(p >> 16);
				*dst++ = static_cast<u8>(p >> 8);
				*dst++ = static_cast<u8>(p);
			}
		}
	}

	void YuyvToRgb(const u8* src, int src_w, int src_h, u8* dst, int dst_w, int dst_h, bool mirror)
	{
		for (int y = 0; y < dst_h; y++)
		{
			const u8* row = src + (y * src_h / dst_h) * src_w * 2;
			for (int x = 0; x < dst_w; x++)
			{
				const int sx = (mirror ? dst_w - 1 - x : x) * src_w / dst_w;
				const u8* pair = row + (sx & ~1) * 2;
				// BT.601 limited range
				const int c = 298 * (row[sx * 2] - 16) + 128;
				const int d = pair[1] - 128;
				const int e = pair[3] - 128;
				*dst++ = Clamp((c + 409 * e) >> 8);
				*dst++ = Clamp((c - 100 * d - 208 * e) >> 8);
				*dst++ = Clamp((c + 516 * d) >> 8);
			}
		}
	}

	// The Capture Eye's 80x64 greyscale, sampled every fourth pixel and laid out in 8x8 blocks.
	static u32 EncodeYuv400(u8* out, const u8* rgb, int width, int height)
	{
		u32 pos = 0;
		for (int my = 0; my < 8; my++)
			for (int mx = 0; mx < 10; mx++)
				for (int y = 0; y < 8; y++)
					for (int x = 0; x < 8; x++)
					{
						const int sx = 4 * (8 * mx + x);
						const int sy = 4 * (8 * my + y);
						if (sx >= width || sy >= height)
						{
							out[pos++] = 0x01;
							continue;
						}
						const u8* p = rgb + (sy * width + sx) * 3;
						out[pos++] = static_cast<u8>(0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]);
					}
		return pos;
	}

	u32 EncodeFrame(u8* out, const u8* rgb, int width, int height, FrameFormat format)
	{
		switch (format)
		{
			case format_mpeg:
				return static_cast<u32>(jo_write_mpeg(out, rgb, width, height, JO_RGB24, JO_NONE, JO_NONE));
			case format_jpeg:
				return CompressCamJPEG(out, MAX_FRAME_SIZE, rgb, width, height, 80);
			case format_yuv400:
				return EncodeYuv400(out, rgb, width, height);
		}
		return 0;
	}

	u32 EncodeTestPattern(u8* out, int width, int height, FrameFormat format)
	{
		if (format == format_yuv400)
		{
			for (int y = 0; y < 64; y++)
				for (int x = 0; x < 80; x++)
					out[80 * y + x] = static_cast<u8>(255 * y / 80);
			return 80 * 64;
		}

		u8* rgb = static_cast<u8*>(malloc(static_cast<size_t>(width) * height * 3));
		if (!rgb)
			return 0;
		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < width; x++)
			{
				u8* p = rgb + (y * width + x) * 3;
				p[0] = static_cast<u8>(255 * y / height);
				p[1] = static_cast<u8>(255 * x / width);
				p[2] = static_cast<u8>(255 * y / height);
			}
		}
		const u32 size = EncodeFrame(out, rgb, width, height, format);
		free(rgb);
		return size;
	}
} // namespace usb_eyetoy
