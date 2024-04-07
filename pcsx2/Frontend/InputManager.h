/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <variant>
#include <utility>

#include "common/Pcsx2Types.h"
#include "common/SettingsInterface.h"
#include "common/WindowInfo.h"

#include "pcsx2/Config.h"

/// Class, or source of an input event.
enum class InputSourceType : u32
{
	Keyboard,
	Pointer,
	Count,
};

/// Subtype of a key for an input source.
enum class InputSubclass : u32
{
	None = 0,

	PointerButton = 0,
	PointerAxis = 1,

	ControllerButton = 0,
	ControllerAxis = 1,
	ControllerHat = 2,
	ControllerMotor = 3,
	ControllerHaptic = 4,
};

enum class InputModifier : u32
{
	None = 0,
	Negate, ///< Input * -1, gets the negative side of the axis
	FullAxis, ///< (Input * 0.5) + 0.5, uses both the negative and positive side of the axis together
};

/// A composite type representing a full input key which is part of an event.
union InputBindingKey
{
	struct
	{
		InputSourceType source_type : 4;
		u32 source_index : 8; ///< controller number
		InputSubclass source_subtype : 3; ///< if 1, binding is for an axis and not a button (used for controllers)
		InputModifier modifier : 2;
		u32 invert : 1; ///< if 1, value is inverted prior to being sent to the sink
		u32 unused : 14;
		u32 data;
	};

	u64 bits;

	bool operator==(const InputBindingKey& k) const { return bits == k.bits; }
	bool operator!=(const InputBindingKey& k) const { return bits != k.bits; }

	/// Removes the direction bit from the key, which is used to look up the bindings for it.
	/// This is because negative bindings should still fire when they reach zero again.
	InputBindingKey MaskDirection() const
	{
		InputBindingKey r;
		r.bits = bits;
		r.modifier = InputModifier::None;
		r.invert = 0;
		return r;
	}
};
static_assert(sizeof(InputBindingKey) == sizeof(u64), "Input binding key is 64 bits");

/// Hashability for InputBindingKey
struct InputBindingKeyHash
{
	std::size_t operator()(const InputBindingKey& k) const { return std::hash<u64>{}(k.bits); }
};

/// Callback type for a binary event. Usually used for hotkeys.
using InputButtonEventHandler = std::function<void(s32 value)>;

/// Callback types for a normalized event. Usually used for pads.
using InputAxisEventHandler = std::function<void(float value)>;

/// Input monitoring for external access.
struct InputInterceptHook
{
	enum class CallbackResult
	{
		StopProcessingEvent,
		ContinueProcessingEvent,
		RemoveHookAndStopProcessingEvent,
		RemoveHookAndContinueProcessingEvent,
	};

	using Callback = std::function<CallbackResult(InputBindingKey key, float value)>;
};

/// Host mouse relative axes are X, Y, wheel horizontal, wheel vertical.
enum class InputPointerAxis : u8
{
	X,
	Y,
	WheelX,
	WheelY,
	Count
};

/// External input source class.
class InputSource;

namespace InputManager
{
	/// Minimum interval between vibration updates when the effect is continuous.
	static constexpr double VIBRATION_UPDATE_INTERVAL_SECONDS = 0.5; // 500ms

	/// Maximum number of host mouse devices.
	static constexpr u32 MAX_POINTER_DEVICES = 1;
	static constexpr u32 MAX_POINTER_BUTTONS = 3;

	/// Parses an input class string.
	std::optional<InputSourceType> ParseInputSourceString(const std::string_view& str);

	/// Converts a key code from a human-readable string to an identifier.
	std::optional<u32> ConvertHostKeyboardStringToCode(const std::string_view& str);

	/// Converts a key code from an identifier to a human-readable string.
	std::optional<std::string> ConvertHostKeyboardCodeToString(u32 code);

	/// Parses an input binding key string.
	std::optional<InputBindingKey> ParseInputBindingKey(const std::string_view& binding);

	/// Converts a input key to a string.
	std::string ConvertInputBindingKeyToString(InputBindingInfo::Type binding_type, InputBindingKey key);

	/// Converts a chord of binding keys to a string.
	std::string ConvertInputBindingKeysToString(InputBindingInfo::Type binding_type, const InputBindingKey* keys, size_t num_keys);

	/// Retrieves bindings that match the generic bindings for the specified device.
	using GenericInputBindingMapping = std::vector<std::pair<GenericInputBinding, std::string>>;

	/// Re-parses the config and registers all hotkey and pad bindings.
	void ReloadBindings(SettingsInterface& si, SettingsInterface& binding_si);

	/// Re-parses the sources part of the config and initializes any backends.
	void ReloadSources(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock);

	/// Shuts down any enabled input sources.
	void CloseSources();

	/// Polls input sources for events (e.g. external controllers).
	void PollSources();

	/// Clears internal state for any binds with a matching source/index.
	void ClearBindStateFromSource(InputBindingKey key);

	/// Sets a hook which can be used to intercept events before they're processed by the normal bindings.
	/// This is typically used when binding new controls to detect what gets pressed.
	void SetHook(InputInterceptHook::Callback callback);

	/// Removes any currently-active interception hook.
	void RemoveHook();

	/// Returns true if there is an interception hook present.
	bool HasHook();

	/// Internal method used by pads to dispatch vibration updates to input sources.
	/// Intensity is normalized from 0 to 1.
	void SetPadVibrationIntensity(u32 pad_index, float large_or_single_motor_intensity, float small_motor_intensity);

	/// Zeros all vibration intensities. Call when pausing.
	/// The pad vibration state will internally remain, so that when emulation is unpaused, the effect resumes.
	void PauseVibration();

	/// Reads absolute pointer position.
	std::pair<float, float> GetPointerAbsolutePosition(u32 index);

	/// Updates absolute pointer position. Can call from UI thread, use when the host only reports absolute coordinates.
	void UpdatePointerAbsolutePosition(u32 index, float x, float y);

	/// Updates relative pointer position. Can call from the UI thread, use when host supports relative coordinate reporting.
	void UpdatePointerRelativeDelta(u32 index, InputPointerAxis axis, float d, bool raw_input = false);

	/// Called when a new input device is connected.
	void OnInputDeviceConnected(const std::string_view& identifier, const std::string_view& device_name);

	/// Called when an input device is disconnected.
	void OnInputDeviceDisconnected(const std::string_view& identifier);
} // namespace InputManager

namespace Host
{
	/// Called when a new input device is connected.
	void OnInputDeviceConnected(const std::string_view& identifier, const std::string_view& device_name);

	/// Called when an input device is disconnected.
	void OnInputDeviceDisconnected(const std::string_view& identifier);

	/// Enables relative mouse mode in the host.
	void SetRelativeMouseMode(bool enabled);
} // namespace Host
