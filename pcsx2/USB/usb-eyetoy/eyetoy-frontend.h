// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "libretro.h"

namespace usb_eyetoy
{
	// All three run on the main thread. Load asks the frontend for its camera and microphone,
	// and must be called from retro_load_game; Run starts and stops the camera and collects
	// microphone samples, once per retro_run; Unload lets both go.
	void FrontendLoad(retro_environment_t environ_cb);
	void FrontendRun();
	void FrontendUnload();
} // namespace usb_eyetoy
