// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "videodev.h"

namespace usb_eyetoy
{
	// The largest picture the camera hands over, and so the largest encoded frame.
	static constexpr u32 MAX_FRAME_SIZE = 640 * 480 * 3;

	// Both resample to the dst size by nearest neighbour; mirror flips left and right.
	void XrgbToRgb(const u32* src, int src_w, int src_h, size_t src_pitch, u8* dst, int dst_w, int dst_h, bool mirror);
	void YuyvToRgb(const u8* src, int src_w, int src_h, u8* dst, int dst_w, int dst_h, bool mirror);

	// rgb is width x height RGB24; returns the encoded size in out, which holds MAX_FRAME_SIZE.
	u32 EncodeFrame(u8* out, const u8* rgb, int width, int height, FrameFormat format);

	// A gradient, for when no camera delivers a picture.
	u32 EncodeTestPattern(u8* out, int width, int height, FrameFormat format);
} // namespace usb_eyetoy
