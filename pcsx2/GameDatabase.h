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

#include "Config.h"
#include "Patch.h"

#include "common/FPControl.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum GamefixId;

namespace GameDatabaseSchema
{
	enum class ClampMode
	{
		Undefined = -1,
		Disabled = 0,
		Normal,
		Extra,
		Full,
		Count
	};

	enum class GSHWFixId : u32
	{
		// boolean settings
		AutoFlush,
		CPUFramebufferConversion,
		FlushTCOnClose,
		DisableDepthSupport,
		PreloadFrameData,
		DisablePartialInvalidation,
		TextureInsideRT,
		AlignSprite,
		MergeSprite,
		Mipmap,
		WildArmsHack,
		BilinearUpscale,
		NativePaletteDraw,
		EstimateTextureRegion,
		PCRTCOffsets,
		PCRTCOverscan,

		// integer settings
		TrilinearFiltering,
		SkipDrawStart,
		SkipDrawEnd,
		HalfBottomOverride,
		HalfPixelOffset,
		RoundSprite,
		NativeScaling,
		TexturePreloading,
		Deinterlace,
		CPUSpriteRenderBW,
		CPUSpriteRenderLevel,
		CPUCLUTRender,
		GPUTargetCLUT,
		GPUPaletteConversion,
		MinimumBlendingLevel,
		MaximumBlendingLevel,
		RecommendedBlendingLevel,
		GetSkipCount,
		BeforeDraw,
		MoveHandler,

		Count
	};

	struct GameEntry
	{
		std::string name;
		std::string region;
		FPRoundMode eeRoundMode = FPRoundMode::MaxCount;
		FPRoundMode eeDivRoundMode = FPRoundMode::MaxCount;
		FPRoundMode vu0RoundMode = FPRoundMode::MaxCount;
		FPRoundMode vu1RoundMode = FPRoundMode::MaxCount;
		ClampMode eeClampMode = ClampMode::Undefined;
		ClampMode vu0ClampMode = ClampMode::Undefined;
		ClampMode vu1ClampMode = ClampMode::Undefined;
		std::vector<GamefixId> gameFixes;
		std::vector<std::pair<SpeedHack, int>> speedHacks;
		std::vector<std::pair<GSHWFixId, s32>> gsHWFixes;
		std::vector<std::string> memcardFilters;
		std::unordered_map<u32, std::string> patches;
		std::vector<DynamicPatch> dynaPatches;

		// Returns the list of memory card serials as a `/` delimited string
		std::string memcardFiltersAsString() const;
		const std::string* findPatch(u32 crc) const;

		/// Applies Core game fixes to an existing config. Returns the number of applied fixes.
		u32 applyGameFixes(Pcsx2Config& config, bool applyAuto) const;

		/// Applies GS hardware fixes to an existing config. Returns the number of applied fixes.
		u32 applyGSHardwareFixes(Pcsx2Config::GSOptions& config) const;

		/// Returns true if the current config value for the specified hw fix id matches the value.
		static bool configMatchesHWFix(const Pcsx2Config::GSOptions& config, GSHWFixId id, int value);
	};
};

namespace GameDatabase
{
	void ensureLoaded();
	const GameDatabaseSchema::GameEntry* findGame(const std::string_view& serial);
}; // namespace GameDatabase
