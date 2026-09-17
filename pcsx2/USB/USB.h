/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../Config.h"


namespace USB
{
	enum : u32
	{
		NUM_PORTS = 2,
	};

	s32 DeviceTypeNameToIndex(const std::string_view& device);
	const char* DeviceTypeIndexToName(s32 device);

	std::vector<std::pair<std::string, std::string>> GetDeviceTypes();
	const char* GetDeviceName(const std::string_view& device);
	const char* GetDeviceSubtypeName(const std::string_view& device, u32 subtype);

	float GetDeviceBindValue(u32 port, u32 bind_index);
	void SetDeviceBindValue(u32 port, u32 bind_index, float value);

	/// Called when a new input device is connected.
	void InputDeviceConnected(const std::string_view& identifier);

	/// Called when an input device is disconnected.
	void InputDeviceDisconnected(const std::string_view& identifier);

	std::string GetConfigSection(int port);

	/// Returns the configuration key for the specified bind and device type.
	std::string GetConfigSubKey(const std::string_view& device, const std::string_view& bind_name);

	/// Performs automatic controller mapping with the provided list of generic mappings.

	/// Clears all bindings for a given port.

	/// Identifies any device/subtype changes and recreates devices.
	void CheckForConfigChanges(const Pcsx2Config& old_config);

	/// Reads a device-specific configuration boolean.

	/// Reads a device-specific configuration integer.

	/// Reads a device-specific configuration floating-point value.

	/// Reads a device-specific configuration string.
} // namespace USB

// ---------------------------------------------------------------------

/* libretro USB port device selection (driven by core options). */
enum UsbPortDevice
{
	USB_DEV_NONE = 0,
	USB_DEV_KEYBOARD,
	USB_DEV_MOUSE,
};

void USBSetPortDevice(unsigned port, int device, unsigned input_port);

void USBinit(void);
void USBasync(u32 cycles);
void USBshutdown(void);
void USBclose(void);
bool USBopen(void);
void USBreset(void);

u8 USBread8(u32 addr);
u16 USBread16(u32 addr);
u32 USBread32(u32 addr);
void USBwrite8(u32 addr, u8 value);
void USBwrite16(u32 addr, u16 value);
void USBwrite32(u32 addr, u32 value);

void USBsetRAM(void* mem);
