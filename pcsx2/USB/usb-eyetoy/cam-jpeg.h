// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Baseline JFIF with 4:2:2 (H2V1) chroma, the layout the EyeToy's JPEG mode sends.
// rgb is width x height RGB24. Returns the size written, or 0 if out_size was too small.
u32 CompressCamJPEG(u8* out, u32 out_size, const u8* rgb, int width, int height, int quality);
