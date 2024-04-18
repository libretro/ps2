#include "PrecompiledHeader.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <libretro.h>

#include "input.h"

#include "PAD/StateManagement.h"
#include "PAD/KeyStatus.h"
#include "Frontend/InputManager.h"

extern retro_environment_t environ_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;
struct retro_rumble_interface rumble;

static struct retro_input_descriptor desc[] = {
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Triangle"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Circle"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Cross"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Square"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
	{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
	{0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "L-Analog X"},
	{0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "L-Analog Y"},
	{0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "R-Analog X"},
	{0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "R-Analog Y"},

	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Triangle"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Circle"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Cross"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Square"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
	{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
	{1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "L-Analog X"},
	{1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "L-Analog Y"},
	{1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "R-Analog X"},
	{1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "R-Analog Y"},

	{0},
};

namespace Input
{
static u32 button_mask[2];
static int pad_lx[2];
static int pad_ly[2];
static int pad_rx[2];
static int pad_ry[2];

static int keymap[] =
	{
		RETRO_DEVICE_ID_JOYPAD_L2,     // PAD_L2
		RETRO_DEVICE_ID_JOYPAD_R2,     // PAD_R2
		RETRO_DEVICE_ID_JOYPAD_L,      // PAD_L1
		RETRO_DEVICE_ID_JOYPAD_R,      // PAD_R1
		RETRO_DEVICE_ID_JOYPAD_X,      // PAD_TRIANGLE
		RETRO_DEVICE_ID_JOYPAD_A,      // PAD_CIRCLE
		RETRO_DEVICE_ID_JOYPAD_B,      // PAD_CROSS
		RETRO_DEVICE_ID_JOYPAD_Y,      // PAD_SQUARE
		RETRO_DEVICE_ID_JOYPAD_SELECT, // PAD_SELECT
		RETRO_DEVICE_ID_JOYPAD_L3,     // PAD_L3
		RETRO_DEVICE_ID_JOYPAD_R3,     // PAD_R3
		RETRO_DEVICE_ID_JOYPAD_START,  // PAD_START
		RETRO_DEVICE_ID_JOYPAD_UP,     // PAD_UP
		RETRO_DEVICE_ID_JOYPAD_RIGHT,  // PAD_RIGHT
		RETRO_DEVICE_ID_JOYPAD_DOWN,   // PAD_DOWN
		RETRO_DEVICE_ID_JOYPAD_LEFT,   // PAD_LEFT
};

void Init()
{
	environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble);
	static const struct retro_controller_description ds2_desc[] = {
		{"DualShock 2", RETRO_DEVICE_ANALOG},
	};

	static const struct retro_controller_info ports[] = {
		{ds2_desc, sizeof(ds2_desc) / sizeof(*ds2_desc)},
		{ds2_desc, sizeof(ds2_desc) / sizeof(*ds2_desc)},
		{},
	};

	environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
	//	environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
	button_mask[0] = 0xFFFFFFFF;
	button_mask[1] = 0xFFFFFFFF;
}

void Shutdown()
{
}

void Update()
{
	poll_cb();
	Pad::rumble_all();

	u32 mask = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
	button_mask[0] = 0xFFFF0000;
	for (int i = 0; i < 16; i++)
		button_mask[0] |= !(mask & (1 << keymap[i])) << i;

	mask = input_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
	button_mask[1] = 0xFFFF0000;
	for (int i = 0; i < 16; i++)
		button_mask[1] |= !(mask & (1 << keymap[i])) << i;

	pad_lx[0] = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
	pad_ly[0] = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
	pad_rx[0] = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
	pad_ry[0] = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
	pad_lx[1] = input_cb(1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
	pad_ly[1] = input_cb(1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
	pad_rx[1] = input_cb(1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
	pad_ry[1] = input_cb(1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
}

} // namespace Input

void retro_set_input_poll(retro_input_poll_t cb)
{
	poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
	input_cb = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

//void Device::DoRumble(unsigned type, unsigned pad)
//{
//	if (pad >= GAMEPAD_NUMBER)
//		return;

//	if (type == 0)
//		rumble.set_rumble_state(pad, RETRO_RUMBLE_WEAK, 0xFFFF);
//	else
//		rumble.set_rumble_state(pad, RETRO_RUMBLE_STRONG, 0xFFFF);
//}

u32 PAD::KeyStatus::GetButtons(u32 pad)
{
	return Input::button_mask[pad];
}

u8 PAD::KeyStatus::GetPressure(u32 pad, u32 index)
{
	int val = 0;
	switch (index)
	{
		case PAD_R_LEFT:
		case PAD_R_RIGHT:
			val = Input::pad_lx[pad];
			break;

		case PAD_R_DOWN:
		case PAD_R_UP:
			val = Input::pad_ly[pad];
			break;

		case PAD_L_LEFT:
		case PAD_L_RIGHT:
			val = Input::pad_rx[pad];
			break;

		case PAD_L_DOWN:
		case PAD_L_UP:
			val = Input::pad_ry[pad];
			break;

		default:
			if (index < 16)
				val = !(Input::button_mask[pad] & (1 << Input::keymap[index]));
			break;
	}

	if (index < 16)
	{
#if 0
		return 0xFF - (val >> 7);
#else
		return val ? 0x00 : 0xFF;
#endif
	}

	return 0x80 + (val >> 8);
}

PAD::KeyStatus::KeyStatus()
{
}

void PAD::KeyStatus::Init()
{
}

void PAD::KeyStatus::Set(u32 pad, u32 index, float value)
{
}

void InputManager::ReloadSources(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
}

void InputManager::ReloadBindings(SettingsInterface& si, SettingsInterface& binding_si)
{
}

void InputManager::PauseVibration()
{
}
