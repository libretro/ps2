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
#include <tuple>
#include <utility>
#include <vector>

#include "Config.h"
#include "common/Pcsx2Defs.h"
#include "SaveState.h"

#define MODE_DIGITAL	0x41
#define MODE_ANALOG	0x73
#define MODE_DS2_NATIVE 0x79

/// Total number of pad ports, across both multitaps.
#define NUM_CONTROLLER_PORTS 8

/// Default stick deadzone/sensitivity.
#define DEFAULT_STICK_DEADZONE 0.0f
#define DEFAULT_STICK_SCALE 1.33f
#define DEFAULT_TRIGGER_DEADZONE 0.0f
#define DEFAULT_TRIGGER_SCALE 1.0f
#define DEFAULT_MOTOR_SCALE 1.0f
#define DEFAULT_PRESSURE_MODIFIER 0.5f
#define DEFAULT_BUTTON_DEADZONE 0.0f

enum PadCommands
{
	CMD_SET_VREF_PARAM        = 0x40,
	CMD_QUERY_DS2_ANALOG_MODE = 0x41,
	CMD_READ_DATA_AND_VIBRATE = 0x42,
	CMD_CONFIG_MODE           = 0x43,
	CMD_SET_MODE_AND_LOCK     = 0x44,
	CMD_QUERY_MODEL_AND_MODE  = 0x45,
	CMD_QUERY_ACT             = 0x46, /* ?? */
	CMD_QUERY_COMB            = 0x47, /* ?? */
	CMD_QUERY_MODE            = 0x4C, /* QUERY_MODE ?? */
	CMD_VIBRATION_TOGGLE      = 0x4D,
	CMD_SET_DS2_NATIVE_MODE   = 0x4F  /* SET_DS2_NATIVE_MODE */
};

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

enum gamePadValues
{
	PAD_UP = 0,   //  0  - Directional pad ↑
	PAD_RIGHT,    //  1  - Directional pad →
	PAD_DOWN,     //  2  - Directional pad ↓
	PAD_LEFT,     //  3  - Directional pad ←
	PAD_TRIANGLE, //  4  - Triangle button ▲
	PAD_CIRCLE,   //  5  - Circle button ●
	PAD_CROSS,    //  6  - Cross button ✖
	PAD_SQUARE,   //  7  - Square button ■
	PAD_SELECT,   //  8  - Select button
	PAD_START,    //  9  - Start button
	PAD_L1,       // 10  - L1 button
	PAD_L2,       // 11  - L2 button
	PAD_R1,       // 12  - R1 button
	PAD_R2,       // 13  - R2 button
	PAD_L3,       // 14  - Left joystick button (L3)
	PAD_R3,       // 15  - Right joystick button (R3)
	PAD_ANALOG,   // 16  - Analog mode toggle
	PAD_PRESSURE, // 17  - Pressure modifier
	PAD_L_UP,     // 18  - Left joystick (Up) ↑
	PAD_L_RIGHT,  // 19  - Left joystick (Right) →
	PAD_L_DOWN,   // 20  - Left joystick (Down) ↓
	PAD_L_LEFT,   // 21  - Left joystick (Left) ←
	PAD_R_UP,     // 22  - Right joystick (Up) ↑
	PAD_R_RIGHT,  // 23  - Right joystick (Right) →
	PAD_R_DOWN,   // 24  - Right joystick (Down) ↓
	PAD_R_LEFT,   // 25  - Right joystick (Left) ←
	MAX_KEYS
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
	void rumble(unsigned port);
	void reset();

	static void stop_vibrate_all();
};

// Full state to manage save state
struct PadFullFreezeData
{
	char format[8];
	// active slot for port
	u8 slot[2];
	PadFreezeData padData[2][4];
	QueryInfo query;
};

struct KeyStatus
{
	struct PADAnalog
	{
		u8 lx, ly;
		u8 rx, ry;
		u8 invert_lx, invert_ly;
		u8 invert_rx, invert_ry;
	};

	ControllerType m_type[NUM_CONTROLLER_PORTS] = {};
	u32 m_button[NUM_CONTROLLER_PORTS];
	u8 m_button_pressure[NUM_CONTROLLER_PORTS][MAX_KEYS];
	PADAnalog m_analog[NUM_CONTROLLER_PORTS];
	float m_axis_scale[NUM_CONTROLLER_PORTS][2];
	float m_trigger_scale[NUM_CONTROLLER_PORTS][2];
	float m_vibration_scale[NUM_CONTROLLER_PORTS][2];
	float m_pressure_modifier[NUM_CONTROLLER_PORTS];
	float m_button_deadzone[NUM_CONTROLLER_PORTS];
};

namespace PAD
{
	struct ControllerInfo
	{
		ControllerType type;
		const char* name;
		const char* display_name;
		const InputBindingInfo* bindings;
		u32 num_bindings;
		const SettingInfo* settings;
		u32 num_settings;
		VibrationCapabilities vibration_caps;
	};

	/// Reloads configuration.
	void LoadConfig(const SettingsInterface& si);

	/// Updates vibration and other internal state. Called at the *end* of a frame.
	void Update();

	/// Returns general information for the specified controller type.
	const ControllerInfo* GetControllerInfo(const std::string_view& name);
} // namespace PAD
  
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
