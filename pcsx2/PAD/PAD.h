/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include <string>

#include "../Config.h"

#include "../../common/Pcsx2Defs.h"

#include "../SaveState.h"

enum ControllerType
{
	NotConnected = 0,
	DualShock2
};

enum VibrationCapabilities
{
	NoVibration = 0,
	LargeSmallMotors,
	SingleMotor
};

/* forward declarations */
class SettingsInterface;

/* The state of the PS2 bus */
struct QueryInfo
{
	u8 port;
	u8 slot;
	u8 lastByte;
	u8 currentCommand;
	u8 numBytes;
	u8 queryDone;
	u8 response[42];

	template <size_t S>
	void set_result(const u8 (&rsp)[S])
	{
		memcpy(response + 2, rsp, S);
		numBytes = 2 + S;
	}
};

// Freeze data, for a single pad.  Basically has all pad state that
// a PS2 can set.
struct PadFreezeData
{
	// Digital / Analog / DS2 Native
	u8 mode;

	u8 modeLock;

	// In config mode
	u8 config;

	u8 vibrate[8];
	u8 umask[3];

	// Vibration indices.
	u8 vibrateI[2];

	// Last vibration value sent to controller.
	// Only used so as not to call vibration
	// functions when old and new values are both 0.
	u8 currentVibrate[2];

	// Next vibrate val to send to controller.  If next and current are
	// both 0, nothing is sent to the controller.  Otherwise, it's sent
	// on every update.
	u8 nextVibrate[2];
};

class Pad : public PadFreezeData
{
public:
	void rumble(float rumble_scale, unsigned port);
	void reset();

	static void stop_vibrate_all();
};

namespace PAD
{
	struct ControllerInfo
	{
		ControllerType type;
		const char* name;
		const InputBindingInfo* bindings; /* TODO/FIXME - not used yet */
		u32 num_bindings;                 /* TODO/FIXME - not used yet */
		VibrationCapabilities vibration_caps;
	};

	/// Reloads configuration.
	void LoadConfig(const SettingsInterface& si);
}
  
namespace Input
{
	void Init();
	void Update();
	void Shutdown();
}

s32 PADinit();
void PADshutdown();
s32 PADopen();
void PADclose();
s32 PADsetSlot(u8 port, u8 slot);
s32 PADfreeze(FreezeAction mode, freezeData* data);
u8 PADstartPoll(int _port, int _slot);
u8 PADpoll(u8 value);
bool PADcomplete(void);

struct PadSettings
{
	float axis_scale   = 1.33f;
	float rumble_scale = 1.0f;
	int axis_invert_lx = 1;
	int axis_invert_ly = 1;
	int axis_invert_rx = 1;
	int axis_invert_ry = 1;
	u16 axis_deadzone;
	u16 button_deadzone;
};

extern PadSettings pad_settings[2];
