// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "videodev.h"

namespace usb_eyetoy
{
	// The host's own camera and microphone, for frontends that offer neither.
	// Linux only (V4L2 and ALSA); nullptr elsewhere.
	std::unique_ptr<VideoDevice> CreateHostVideoDevice();
	std::unique_ptr<MicSource> CreateHostMicSource();
} // namespace usb_eyetoy
