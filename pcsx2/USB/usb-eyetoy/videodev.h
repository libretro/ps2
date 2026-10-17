// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <memory>

namespace usb_eyetoy
{
	enum FrameFormat
	{
		format_mpeg,
		format_jpeg,
		format_yuv400
	};

	enum DeviceType
	{
		TYPE_EYETOY,
		TYPE_OV511P,
	};

	class VideoDevice
	{
	public:
		virtual ~VideoDevice() {}
		virtual int Open(int width, int height, FrameFormat format, int mirror) = 0;
		virtual int Close() = 0;
		virtual int GetImage(uint8_t* buf, size_t len) = 0;
		virtual void SetMirroring(bool state) = 0;

		// The frontend's camera when it has one, else the host's own where there is a backend.
		static std::unique_ptr<VideoDevice> CreateInstance();
	};

	// 16 kHz mono, as the EyeToy's microphone delivers it.
	class MicSource
	{
	public:
		virtual ~MicSource() {}
		// Returns how many samples were available, up to max; never waits.
		virtual u32 Read(s16* samples, u32 max) = 0;

		// The frontend's microphone when it has one, else the host's; nullptr when neither exists.
		static std::unique_ptr<MicSource> CreateInstance();
	};
} // namespace usb_eyetoy
