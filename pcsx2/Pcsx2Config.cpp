/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

#include "../common/FileSystem.h"
#include "../common/Path.h"
#include "../common/SettingsInterface.h"
#include "../common/SettingsWrapper.h"
#include "../common/StringUtil.h"

#include "Config.h"
#include "GS.h"
#include "CDVD/CDVDcommon.h"
#include "MemoryCardFile.h"
#include "USB/USB.h"

#include <file/file_path.h>

// This macro is actually useful for about any and every possible application of C++
// equality operators.
#define OpEqu(field) (field == right.field)

// Default EE/VU control registers have exceptions off, DaZ/FTZ, and the rounding mode set to Chop/Zero.
static constexpr FPControlRegister DEFAULT_FPU_FP_CONTROL_REGISTER = FPControlRegister::GetDefault()
	.DisableExceptions()
	.SetDenormalsAreZero(true)
.SetFlushToZero(true)
	.SetRoundMode(FPRoundMode::ChopZero);
static constexpr FPControlRegister DEFAULT_VU_FP_CONTROL_REGISTER = FPControlRegister::GetDefault()
	.DisableExceptions()
	.SetDenormalsAreZero(true)
.SetFlushToZero(true)
	.SetRoundMode(FPRoundMode::ChopZero);


Pcsx2Config EmuConfig;

const char* SettingInfo::StringDefaultValue() const
{
	return default_value ? default_value : "";
}

bool SettingInfo::BooleanDefaultValue() const
{
	return default_value ? StringUtil::FromChars<bool>(default_value).value_or(false) : false;
}

s32 SettingInfo::IntegerDefaultValue() const
{
	return default_value ? StringUtil::FromChars<s32>(default_value).value_or(0) : 0;
}

s32 SettingInfo::IntegerMinValue() const
{
	static constexpr s32 fallback_value = std::numeric_limits<s32>::min();
	return min_value ? StringUtil::FromChars<s32>(min_value).value_or(fallback_value) : fallback_value;
}

s32 SettingInfo::IntegerMaxValue() const
{
	static constexpr s32 fallback_value = std::numeric_limits<s32>::max();
	return max_value ? StringUtil::FromChars<s32>(max_value).value_or(fallback_value) : fallback_value;
}

s32 SettingInfo::IntegerStepValue() const
{
	static constexpr s32 fallback_value = 1;
	return step_value ? StringUtil::FromChars<s32>(step_value).value_or(fallback_value) : fallback_value;
}

float SettingInfo::FloatDefaultValue() const
{
	return default_value ? StringUtil::FromChars<float>(default_value).value_or(0.0f) : 0.0f;
}

float SettingInfo::FloatMinValue() const
{
	static constexpr float fallback_value = std::numeric_limits<float>::min();
	return min_value ? StringUtil::FromChars<float>(min_value).value_or(fallback_value) : fallback_value;
}

float SettingInfo::FloatMaxValue() const
{
	static constexpr float fallback_value = std::numeric_limits<float>::max();
	return max_value ? StringUtil::FromChars<float>(max_value).value_or(fallback_value) : fallback_value;
}

float SettingInfo::FloatStepValue() const
{
	static constexpr float fallback_value = 0.1f;
	return step_value ? StringUtil::FromChars<float>(step_value).value_or(fallback_value) : fallback_value;
}

namespace EmuFolders
{
	std::string AppRoot;
	std::string DataRoot;
	std::string Bios;
	std::string MemoryCards;
	std::string Cheats;
	std::string CheatsWS;
	std::string CheatsNI;
	std::string Resources;
	std::string Cache;
	std::string Textures;
} // namespace EmuFolders

const char* const s_speed_hack_names[] =
{
	"mvuFlag",
	"instantVU1",
	"mtvu",
	"eeCycleRate",
};

const char* Pcsx2Config::SpeedhackOptions::GetSpeedHackName(SpeedHack id)
{
	return s_speed_hack_names[static_cast<u32>(id)];
}

std::optional<SpeedHack> Pcsx2Config::SpeedhackOptions::ParseSpeedHackName(const std::string_view& name)
{
	for (u32 i = 0; i < std::size(s_speed_hack_names); i++)
	{
		if (name == s_speed_hack_names[i])
			return static_cast<SpeedHack>(i);
	}

	return std::nullopt;
}

void Pcsx2Config::SpeedhackOptions::Set(SpeedHack id, int value)
{
	switch (id)
	{
		case SpeedHack::MVUFlag:
			vuFlagHack = (value != 0);
			break;
		case SpeedHack::InstantVU1:
			vu1Instant = (value != 0);
			break;
		case SpeedHack::MTVU:
			vuThread = (value != 0);
			break;
		case SpeedHack::EECycleRate:
			EECycleRate = static_cast<int>(std::clamp<int>(value, MIN_EE_CYCLE_RATE, MAX_EE_CYCLE_RATE));
			break;
		default:
			break;
	}
}

bool Pcsx2Config::SpeedhackOptions::operator==(const SpeedhackOptions& right) const
{
	return OpEqu(bitset) && OpEqu(EECycleRate) && OpEqu(EECycleSkip);
}

bool Pcsx2Config::SpeedhackOptions::operator!=(const SpeedhackOptions& right) const
{
	return !operator==(right);
}

Pcsx2Config::SpeedhackOptions::SpeedhackOptions()
{
	DisableAll();

	// Set recommended speedhacks to enabled by default. They'll still be off globally on resets.
	WaitLoop = true;
	IntcStat = true;
	vuFlagHack = true;
	vu1Instant = true;
}

Pcsx2Config::SpeedhackOptions& Pcsx2Config::SpeedhackOptions::DisableAll()
{
	bitset = 0;
	EECycleRate = 0;
	EECycleSkip = 0;

	return *this;
}

void Pcsx2Config::SpeedhackOptions::LoadSave(SettingsWrapper& wrap)
{
	SettingsWrapSection("EmuCore/Speedhacks");

	SettingsWrapBitfield(EECycleRate);
	SettingsWrapBitfield(EECycleSkip);
	SettingsWrapBitBool(fastCDVD);
	SettingsWrapBitBool(IntcStat);
	SettingsWrapBitBool(WaitLoop);
	SettingsWrapBitBool(vuFlagHack);
	SettingsWrapBitBool(vuThread);
	SettingsWrapBitBool(vu1Instant);

	EECycleRate = std::clamp(EECycleRate, MIN_EE_CYCLE_RATE, MAX_EE_CYCLE_RATE);
	EECycleSkip = std::min(EECycleSkip, MAX_EE_CYCLE_SKIP);
}

Pcsx2Config::RecompilerOptions::RecompilerOptions()
{
	bitset = 0;

	// All recs are enabled by default.

	EnableEE = true;
	EnableEECache = false;
	EnableIOP = true;
	EnableVU0 = true;
	EnableVU1 = true;
	EnableFastmem = true;

	// vu and fpu clamping default to standard overflow.
	vu0Overflow = true;
	//vu0ExtraOverflow = false;
	//vu0SignOverflow = false;
	//vu0Underflow = false;
	vu1Overflow = true;
	//vu1ExtraOverflow = false;
	//vu1SignOverflow = false;
	//vu1Underflow = false;

	fpuOverflow = true;
	//fpuExtraOverflow = false;
	//fpuFullMode = false;
}

void Pcsx2Config::RecompilerOptions::ApplySanityCheck()
{
	bool fpuIsRight = true;

	if (fpuExtraOverflow)
		fpuIsRight = fpuOverflow;

	if (fpuFullMode)
		fpuIsRight = fpuOverflow && fpuExtraOverflow;

	if (!fpuIsRight)
	{
		// Values are wonky; assume the defaults.
		fpuOverflow = RecompilerOptions().fpuOverflow;
		fpuExtraOverflow = RecompilerOptions().fpuExtraOverflow;
		fpuFullMode = RecompilerOptions().fpuFullMode;
	}

	bool vuIsOk = true;

	if (vu0ExtraOverflow)
		vuIsOk = vuIsOk && vu0Overflow;
	if (vu0SignOverflow)
		vuIsOk = vuIsOk && vu0ExtraOverflow;

	if (!vuIsOk)
	{
		// Values are wonky; assume the defaults.
		vu0Overflow = RecompilerOptions().vu0Overflow;
		vu0ExtraOverflow = RecompilerOptions().vu0ExtraOverflow;
		vu0SignOverflow = RecompilerOptions().vu0SignOverflow;
		vu0Underflow = RecompilerOptions().vu0Underflow;
	}

	vuIsOk = true;

	if (vu1ExtraOverflow)
		vuIsOk = vuIsOk && vu1Overflow;
	if (vu1SignOverflow)
		vuIsOk = vuIsOk && vu1ExtraOverflow;

	if (!vuIsOk)
	{
		// Values are wonky; assume the defaults.
		vu1Overflow = RecompilerOptions().vu1Overflow;
		vu1ExtraOverflow = RecompilerOptions().vu1ExtraOverflow;
		vu1SignOverflow = RecompilerOptions().vu1SignOverflow;
		vu1Underflow = RecompilerOptions().vu1Underflow;
	}
}

void Pcsx2Config::RecompilerOptions::LoadSave(SettingsWrapper& wrap)
{
	SettingsWrapSection("EmuCore/CPU/Recompiler");

	SettingsWrapBitBool(EnableEE);
	SettingsWrapBitBool(EnableIOP);
	SettingsWrapBitBool(EnableEECache);
	SettingsWrapBitBool(EnableVU0);
	SettingsWrapBitBool(EnableVU1);
	SettingsWrapBitBool(EnableFastmem);

	SettingsWrapBitBool(vu0Overflow);
	SettingsWrapBitBool(vu0ExtraOverflow);
	SettingsWrapBitBool(vu0SignOverflow);
	SettingsWrapBitBool(vu0Underflow);
	SettingsWrapBitBool(vu1Overflow);
	SettingsWrapBitBool(vu1ExtraOverflow);
	SettingsWrapBitBool(vu1SignOverflow);
	SettingsWrapBitBool(vu1Underflow);

	SettingsWrapBitBool(fpuOverflow);
	SettingsWrapBitBool(fpuExtraOverflow);
	SettingsWrapBitBool(fpuFullMode);
}

bool Pcsx2Config::CpuOptions::CpusChanged(const CpuOptions& right) const
{
	return (Recompiler.EnableEE != right.Recompiler.EnableEE ||
			Recompiler.EnableIOP != right.Recompiler.EnableIOP ||
			Recompiler.EnableVU0 != right.Recompiler.EnableVU0 ||
			Recompiler.EnableVU1 != right.Recompiler.EnableVU1);
}

Pcsx2Config::CpuOptions::CpuOptions()
{
	FPUFPCR = DEFAULT_FPU_FP_CONTROL_REGISTER;

	// Rounding defaults to nearest to match old behavior.
	// TODO: Make it default to the same as the rest of the FPU operations, at some point.
	FPUDivFPCR = FPControlRegister(DEFAULT_FPU_FP_CONTROL_REGISTER).SetRoundMode(FPRoundMode::Nearest);

	VU0FPCR = DEFAULT_VU_FP_CONTROL_REGISTER;
	VU1FPCR = DEFAULT_VU_FP_CONTROL_REGISTER;
	AffinityControlMode = 0;
}

void Pcsx2Config::CpuOptions::ApplySanityCheck()
{
	AffinityControlMode = std::min<u32>(AffinityControlMode, 6);

	Recompiler.ApplySanityCheck();
}

void Pcsx2Config::CpuOptions::LoadSave(SettingsWrapper& wrap)
{
	SettingsWrapSection("EmuCore/CPU");

	{
		FPUFPCR.SetDenormalsAreZero(wrap.EntryBitBool("FPU", "FPU.DenormalsAreZero",
			FPUFPCR.GetDenormalsAreZero(), FPUFPCR.GetDenormalsAreZero()));
		FPUFPCR.SetFlushToZero(wrap.EntryBitBool(CURRENT_SETTINGS_SECTION, "FPU.DenormalsAreZero",
			FPUFPCR.GetFlushToZero(), FPUFPCR.GetFlushToZero()));

		uint round_mode = static_cast<uint>(FPUFPCR.GetRoundMode());
		wrap.Entry("FPU", "FPU.Roundmode", round_mode, round_mode);
		round_mode = std::min(round_mode, static_cast<uint>(FPRoundMode::MaxCount) - 1u);
		FPUFPCR.SetRoundMode(static_cast<FPRoundMode>(round_mode));
	}
	{
		FPUDivFPCR.SetDenormalsAreZero(wrap.EntryBitBool("FPUDiv", "FPUDiv.DenormalsAreZero",
			FPUDivFPCR.GetDenormalsAreZero(), FPUDivFPCR.GetDenormalsAreZero()));
		FPUDivFPCR.SetFlushToZero(wrap.EntryBitBool(CURRENT_SETTINGS_SECTION, "FPUDiv.DenormalsAreZero",
			FPUDivFPCR.GetFlushToZero(), FPUDivFPCR.GetFlushToZero()));

		uint round_mode = static_cast<uint>(FPUDivFPCR.GetRoundMode());
		wrap.Entry("FPUDiv", "FPUDiv.Roundmode", round_mode, round_mode);
		round_mode = std::min(round_mode, static_cast<uint>(FPRoundMode::MaxCount) - 1u);
		FPUDivFPCR.SetRoundMode(static_cast<FPRoundMode>(round_mode));
	}
	{
		VU0FPCR.SetDenormalsAreZero(wrap.EntryBitBool("VU0", "VU0.DenormalsAreZero",
			VU0FPCR.GetDenormalsAreZero(), VU0FPCR.GetDenormalsAreZero()));
		VU0FPCR.SetFlushToZero(wrap.EntryBitBool(CURRENT_SETTINGS_SECTION, "VU0.DenormalsAreZero",
			VU0FPCR.GetFlushToZero(), VU0FPCR.GetFlushToZero()));

		uint round_mode = static_cast<uint>(VU0FPCR.GetRoundMode());
		wrap.Entry("VU0", "VU0.Roundmode", round_mode, round_mode);
		round_mode = std::min(round_mode, static_cast<uint>(FPRoundMode::MaxCount) - 1u);
		VU0FPCR.SetRoundMode(static_cast<FPRoundMode>(round_mode));
	}
	{
		VU1FPCR.SetDenormalsAreZero(wrap.EntryBitBool("VU1", "VU1.DenormalsAreZero",
			VU1FPCR.GetDenormalsAreZero(), VU1FPCR.GetDenormalsAreZero()));
		VU1FPCR.SetFlushToZero(wrap.EntryBitBool(CURRENT_SETTINGS_SECTION, "VU1.DenormalsAreZero",
			VU1FPCR.GetFlushToZero(), VU1FPCR.GetFlushToZero()));

		uint round_mode = static_cast<uint>(VU1FPCR.GetRoundMode());
		wrap.Entry("VU1", "VU1.Roundmode", round_mode, round_mode);
		round_mode = std::min(round_mode, static_cast<uint>(FPRoundMode::MaxCount) - 1u);
		VU1FPCR.SetRoundMode(static_cast<FPRoundMode>(round_mode));
	}

	SettingsWrapEntry(AffinityControlMode);

	Recompiler.LoadSave(wrap);
}

Pcsx2Config::GSOptions::GSOptions()
{
	bitset = 0;

	PCRTCAntiBlur = true;
	DisableInterlaceOffset = false;
	PCRTCOffsets = false;
	PCRTCOverscan = false;
	UseDebugDevice = false;
	DisableShaderCache = false;
	DisableFramebufferFetch = false;
	DisableVertexShaderExpand = false;
	SkipDuplicateFrames = false;

	HWDownloadMode = GSHardwareDownloadMode::Enabled;
	GPUPaletteConversion = false;
	AutoFlushSW = true;
	PreloadFrameWithGSData = false;
	Mipmap = true;

	ManualUserHacks = false;
	UserHacks_AlignSpriteX = false;
	UserHacks_AutoFlush = GSHWAutoFlushLevel::Disabled;
	UserHacks_CPUFBConversion = false;
	UserHacks_ReadTCOnClose = false;
	UserHacks_DisableDepthSupport = false;
	UserHacks_DisablePartialInvalidation = false;
	UserHacks_DisableSafeFeatures = false;
	UserHacks_DisableRenderFixes = false;
	UserHacks_MergePPSprite = false;
	UserHacks_ForceEvenSpritePosition = false;
	UserHacks_BilinearHack = GSBilinearDirtyMode::Automatic;
	UserHacks_NativePaletteDraw = false;

	LoadTextureReplacements = false;
	LoadTextureReplacementsAsync = true;
	PrecacheTextureReplacements = false;
}

bool Pcsx2Config::GSOptions::operator==(const GSOptions& right) const
{
	return (
		OpEqu(VsyncQueueSize) &&

		OpEqu(FramerateNTSC) &&
		OpEqu(FrameratePAL) &&

		OptionsAreEqual(right));
}

bool Pcsx2Config::GSOptions::OptionsAreEqual(const GSOptions& right) const
{
	return (
		OpEqu(bitset) &&

		OpEqu(InterlaceMode) &&

		OpEqu(Renderer) &&
		OpEqu(UpscaleMultiplier) &&

		OpEqu(AccurateBlendingUnit) &&
		OpEqu(HWMipmapMode) &&
		OpEqu(TextureFiltering) &&
		OpEqu(TexturePreloading) &&
		OpEqu(HWDownloadMode) &&
		OpEqu(Dithering) &&
		OpEqu(MaxAnisotropy) &&
		OpEqu(SWExtraThreads) &&
		OpEqu(SWExtraThreadsHeight) &&
		OpEqu(TriFilter) &&
		OpEqu(GetSkipCountFunctionId) &&
		OpEqu(BeforeDrawFunctionId) &&
		OpEqu(MoveHandlerFunctionId) &&
		OpEqu(SkipDrawEnd) &&
		OpEqu(SkipDrawStart) &&

		OpEqu(UserHacks_AutoFlush) &&
		OpEqu(UserHacks_HalfPixelOffset) &&
		OpEqu(UserHacks_RoundSprite) &&
		OpEqu(UserHacks_NativeScaling) &&
		OpEqu(UserHacks_TCOffsetX) &&
		OpEqu(UserHacks_TCOffsetY) &&
		OpEqu(UserHacks_CPUSpriteRenderBW) &&
		OpEqu(UserHacks_CPUSpriteRenderLevel) &&
		OpEqu(UserHacks_CPUCLUTRender) &&
		OpEqu(UserHacks_GPUTargetCLUTMode) &&
		OpEqu(UserHacks_TextureInsideRt) &&
		OpEqu(UserHacks_BilinearHack) &&
		OpEqu(OverrideTextureBarriers) &&
		OpEqu(Adapter) &&
		OpEqu(PGSSuperSampling) &&
		OpEqu(PGSHighResScanout) &&
		OpEqu(PGSDisableMipmaps) &&
		OpEqu(PGSSharpBackbuffer) &&
		OpEqu(PGSSuperSampleTextures)
		);
}

bool Pcsx2Config::GSOptions::operator!=(const GSOptions& right) const
{
	return !operator==(right);
}

bool Pcsx2Config::GSOptions::RestartOptionsAreEqual(const GSOptions& right) const
{
	return OpEqu(Renderer) &&
		   OpEqu(Adapter) &&
		   OpEqu(UseDebugDevice) &&
		   OpEqu(DisableShaderCache) &&
		   OpEqu(DisableFramebufferFetch) &&
		   OpEqu(DisableVertexShaderExpand) &&
		   OpEqu(OverrideTextureBarriers);
}

void Pcsx2Config::GSOptions::LoadSave(SettingsWrapper& wrap)
{
	SettingsWrapSection("EmuCore/GS");

	SettingsWrapEntry(VsyncQueueSize);

	SettingsWrapEntry(FramerateNTSC);
	SettingsWrapEntry(FrameratePAL);

	// Unfortunately, because code in the GS still reads the setting by key instead of
	// using these variables, we need to use the old names. Maybe post 2.0 we can change this.
	SettingsWrapBitBoolEx(PCRTCAntiBlur, "pcrtc_antiblur");
	SettingsWrapBitBoolEx(DisableInterlaceOffset, "disable_interlace_offset");
	SettingsWrapBitBoolEx(PCRTCOffsets, "pcrtc_offsets");
	SettingsWrapBitBoolEx(PCRTCOverscan, "pcrtc_overscan");
	SettingsWrapBitBool(UseDebugDevice);
	SettingsWrapBitBoolEx(DisableShaderCache, "disable_shader_cache");
	SettingsWrapBitBool(DisableFramebufferFetch);
	SettingsWrapBitBool(DisableVertexShaderExpand);
	SettingsWrapBitBool(SkipDuplicateFrames);

	SettingsWrapBitBoolEx(GPUPaletteConversion, "paltex");
	SettingsWrapBitBoolEx(AutoFlushSW, "autoflush_sw");
	SettingsWrapIntEnumEx(PGSSuperSampling, "pgsSuperSampling");
	SettingsWrapIntEnumEx(PGSHighResScanout, "pgsHighResScanout");
	SettingsWrapIntEnumEx(PGSDisableMipmaps, "pgsDisableMipmaps");
	SettingsWrapIntEnumEx(PGSSuperSampleTextures, "pgsSuperSampleTextures");
	SettingsWrapIntEnumEx(PGSSharpBackbuffer, "pgsSharpBackbuffer");
	SettingsWrapBitBoolEx(PreloadFrameWithGSData, "preload_frame_with_gs_data");
	SettingsWrapBitBoolEx(Mipmap, "mipmap");
	SettingsWrapBitBoolEx(ManualUserHacks, "UserHacks");
	SettingsWrapBitBoolEx(UserHacks_AlignSpriteX, "UserHacks_align_sprite_X");
	SettingsWrapIntEnumEx(UserHacks_AutoFlush, "UserHacks_AutoFlushLevel");
	SettingsWrapBitBoolEx(UserHacks_CPUFBConversion, "UserHacks_CPU_FB_Conversion");
	SettingsWrapBitBoolEx(UserHacks_ReadTCOnClose, "UserHacks_ReadTCOnClose");
	SettingsWrapBitBoolEx(UserHacks_DisableDepthSupport, "UserHacks_DisableDepthSupport");
	SettingsWrapBitBoolEx(UserHacks_DisablePartialInvalidation, "UserHacks_DisablePartialInvalidation");
	SettingsWrapBitBoolEx(UserHacks_DisableSafeFeatures, "UserHacks_Disable_Safe_Features");
	SettingsWrapBitBoolEx(UserHacks_DisableRenderFixes, "UserHacks_DisableRenderFixes");
	SettingsWrapBitBoolEx(UserHacks_MergePPSprite, "UserHacks_merge_pp_sprite");
	SettingsWrapBitBoolEx(UserHacks_ForceEvenSpritePosition, "UserHacks_ForceEvenSpritePosition");
	SettingsWrapIntEnumEx(UserHacks_BilinearHack, "UserHacks_BilinearHack");
	SettingsWrapBitBoolEx(UserHacks_NativePaletteDraw, "UserHacks_NativePaletteDraw");
	SettingsWrapIntEnumEx(UserHacks_TextureInsideRt, "UserHacks_TextureInsideRt");
	SettingsWrapBitBoolEx(UserHacks_EstimateTextureRegion, "UserHacks_EstimateTextureRegion");
	SettingsWrapBitBool(LoadTextureReplacements);
	SettingsWrapBitBool(LoadTextureReplacementsAsync);
	SettingsWrapBitBool(PrecacheTextureReplacements);

	SettingsWrapIntEnumEx(InterlaceMode, "deinterlace_mode");

	SettingsWrapIntEnumEx(Renderer, "Renderer");
	SettingsWrapEntryEx(UpscaleMultiplier, "upscale_multiplier");

	SettingsWrapIntEnumEx(HWMipmapMode, "hw_mipmap_mode");
	SettingsWrapIntEnumEx(AccurateBlendingUnit, "accurate_blending_unit");
	SettingsWrapIntEnumEx(TextureFiltering, "filter");
	SettingsWrapIntEnumEx(TexturePreloading, "texture_preloading");
	SettingsWrapIntEnumEx(HWDownloadMode, "HWDownloadMode");
	SettingsWrapBitfieldEx(Dithering, "dithering_ps2");
	SettingsWrapBitfieldEx(MaxAnisotropy, "MaxAnisotropy");
	SettingsWrapBitfieldEx(SkipDrawStart, "UserHacks_SkipDraw_Start");
	SettingsWrapBitfieldEx(SkipDrawEnd, "UserHacks_SkipDraw_End");
	SkipDrawEnd = std::max(SkipDrawStart, SkipDrawEnd);

	SettingsWrapIntEnumEx(UserHacks_HalfPixelOffset, "UserHacks_HalfPixelOffset");
	SettingsWrapBitfieldEx(UserHacks_RoundSprite, "UserHacks_round_sprite_offset");
	SettingsWrapIntEnumEx(UserHacks_NativeScaling, "UserHacks_native_scaling");
	SettingsWrapBitfieldEx(UserHacks_TCOffsetX, "UserHacks_TCOffsetX");
	SettingsWrapBitfieldEx(UserHacks_TCOffsetY, "UserHacks_TCOffsetY");
	SettingsWrapBitfieldEx(UserHacks_CPUSpriteRenderBW, "UserHacks_CPUSpriteRenderBW");
	SettingsWrapBitfieldEx(UserHacks_CPUSpriteRenderLevel, "UserHacks_CPUSpriteRenderLevel");
	SettingsWrapBitfieldEx(UserHacks_CPUCLUTRender, "UserHacks_CPUCLUTRender");
	SettingsWrapIntEnumEx(UserHacks_GPUTargetCLUTMode, "UserHacks_GPUTargetCLUTMode");
	SettingsWrapIntEnumEx(TriFilter, "TriFilter");
	SettingsWrapBitfieldEx(OverrideTextureBarriers, "OverrideTextureBarriers");

	SettingsWrapEntry(Adapter);
}

void Pcsx2Config::GSOptions::MaskUserHacks()
{
	if (ManualUserHacks)
		return;

	UserHacks_AlignSpriteX = false;
	UserHacks_MergePPSprite = false;
	UserHacks_ForceEvenSpritePosition = false;
	UserHacks_NativePaletteDraw = false;
	UserHacks_DisableSafeFeatures = false;
	UserHacks_DisableRenderFixes = false;
	UserHacks_HalfPixelOffset = GSHalfPixelOffset::Off;
	UserHacks_RoundSprite = 0;
	UserHacks_NativeScaling = GSNativeScaling::NativeScaling_Normal;
	UserHacks_AutoFlush = GSHWAutoFlushLevel::Disabled;
	GPUPaletteConversion = false;
	PreloadFrameWithGSData = false;
	UserHacks_DisablePartialInvalidation = false;
	UserHacks_DisableDepthSupport = false;
	UserHacks_CPUFBConversion = false;
	UserHacks_ReadTCOnClose = false;
	UserHacks_TextureInsideRt = GSTextureInRtMode::Disabled;
	UserHacks_EstimateTextureRegion = false;
	UserHacks_TCOffsetX = 0;
	UserHacks_TCOffsetY = 0;
	UserHacks_CPUSpriteRenderBW = 0;
	UserHacks_CPUSpriteRenderLevel = 0;
	UserHacks_CPUCLUTRender = 0;
	UserHacks_GPUTargetCLUTMode = GSGPUTargetCLUTMode::Disabled;
	UserHacks_BilinearHack = GSBilinearDirtyMode::Automatic;
	SkipDrawStart = 0;
	SkipDrawEnd = 0;
}

void Pcsx2Config::GSOptions::MaskUpscalingHacks()
{
	if (UpscaleMultiplier > 1.0f && ManualUserHacks)
		return;

	UserHacks_AlignSpriteX = false;
	UserHacks_MergePPSprite = false;
	UserHacks_ForceEvenSpritePosition = false;
	UserHacks_BilinearHack = GSBilinearDirtyMode::Automatic;
	UserHacks_NativePaletteDraw = false;
	UserHacks_HalfPixelOffset = GSHalfPixelOffset::Off;
	UserHacks_RoundSprite = 0;
	UserHacks_NativeScaling = GSNativeScaling::NativeScaling_Normal;
	UserHacks_TCOffsetX = 0;
	UserHacks_TCOffsetY = 0;
}

bool Pcsx2Config::GSOptions::UseHardwareRenderer() const
{
	return (Renderer != GSRendererType::SW);
}

const char* Pcsx2Config::DEV9Options::NetApiNames[] = {
	"Unset",
	"PCAP Bridged",
	"PCAP Switched",
	"TAP",
	"Sockets",
	nullptr};

const char* Pcsx2Config::DEV9Options::DnsModeNames[] = {
	"Manual",
	"Auto",
	"Internal",
	nullptr};

Pcsx2Config::DEV9Options::DEV9Options()
{
	HddFile = "DEV9hdd.raw";
}

void Pcsx2Config::DEV9Options::LoadSave(SettingsWrapper& wrap)
{
	{
		SettingsWrapSection("DEV9/Eth");
		SettingsWrapEntry(EthEnable);
		SettingsWrapEnumEx(EthApi, "EthApi", NetApiNames);
		SettingsWrapEntry(EthDevice);
		SettingsWrapEntry(EthLogDNS);

		SettingsWrapEntry(InterceptDHCP);

		std::string ps2IPStr = "0.0.0.0";
		std::string maskStr = "0.0.0.0";
		std::string gatewayStr = "0.0.0.0";
		std::string dns1Str = "0.0.0.0";
		std::string dns2Str = "0.0.0.0";
		if (wrap.IsSaving())
		{
			ps2IPStr = SaveIPHelper(PS2IP);
			maskStr = SaveIPHelper(Mask);
			gatewayStr = SaveIPHelper(Gateway);
			dns1Str = SaveIPHelper(DNS1);
			dns2Str = SaveIPHelper(DNS2);
		}
		SettingsWrapEntryEx(ps2IPStr, "PS2IP");
		SettingsWrapEntryEx(maskStr, "Mask");
		SettingsWrapEntryEx(gatewayStr, "Gateway");
		SettingsWrapEntryEx(dns1Str, "DNS1");
		SettingsWrapEntryEx(dns2Str, "DNS2");
		if (wrap.IsLoading())
		{
			LoadIPHelper(PS2IP, ps2IPStr);
			LoadIPHelper(Mask, maskStr);
			LoadIPHelper(Gateway, gatewayStr);
			LoadIPHelper(DNS1, dns1Str);
			LoadIPHelper(DNS1, dns1Str);
		}

		SettingsWrapEntry(AutoMask);
		SettingsWrapEntry(AutoGateway);
		SettingsWrapEnumEx(ModeDNS1, "ModeDNS1", DnsModeNames);
		SettingsWrapEnumEx(ModeDNS2, "ModeDNS2", DnsModeNames);
	}

	if (wrap.IsLoading())
		EthHosts.clear();

	int hostCount = static_cast<int>(EthHosts.size());
	{
		SettingsWrapSection("DEV9/Eth/Hosts");
		SettingsWrapEntryEx(hostCount, "Count");
	}

	for (int i = 0; i < hostCount; i++)
	{
		std::string section = "DEV9/Eth/Hosts/Host" + std::to_string(i);
		SettingsWrapSection(section.c_str());

		HostEntry entry;
		if (wrap.IsSaving())
			entry = EthHosts[i];

		SettingsWrapEntryEx(entry.Url, "Url");
		SettingsWrapEntryEx(entry.Desc, "Desc");

		std::string addrStr = "0.0.0.0";
		if (wrap.IsSaving())
			addrStr = SaveIPHelper(entry.Address);
		SettingsWrapEntryEx(addrStr, "Address");
		if (wrap.IsLoading())
			LoadIPHelper(entry.Address, addrStr);

		SettingsWrapEntryEx(entry.Enabled, "Enabled");

		if (wrap.IsLoading())
			EthHosts.push_back(entry);
	}

	{
		SettingsWrapSection("DEV9/Hdd");
		SettingsWrapEntry(HddEnable);
		SettingsWrapEntry(HddFile);
		SettingsWrapEntry(HddSizeSectors);
	}
}

void Pcsx2Config::DEV9Options::LoadIPHelper(u8* field, const std::string& setting)
{
	if (4 == sscanf(setting.c_str(), "%hhu.%hhu.%hhu.%hhu", &field[0], &field[1], &field[2], &field[3]))
		return;
	std::fill(field, field + 4, 0);
}
std::string Pcsx2Config::DEV9Options::SaveIPHelper(u8* field)
{
	return StringUtil::StdStringFromFormat("%u.%u.%u.%u", field[0], field[1], field[2], field[3]);
}

static const char* const tbl_GamefixNames[] =
{
	"FpuMul",
	"GoemonTlb",
	"SoftwareRendererFMV",
	"SkipMPEG",
	"OPHFlag",
	"EETiming",
	"InstantDMA",
	"DMABusy",
	"GIFFIFO",
	"VIFFIFO",
	"VIF1Stall",
	"VuAddSub",
	"Ibit",
	"VUSync",
	"VUOverflow",
	"XGKick",
	"BlitInternalFPS",
	"FullVU0Sync",
};

const char* EnumToString(GamefixId id)
{
	return tbl_GamefixNames[id];
}

// all gamefixes are disabled by default.
Pcsx2Config::GamefixOptions::GamefixOptions()
{
	DisableAll();
}

Pcsx2Config::GamefixOptions& Pcsx2Config::GamefixOptions::DisableAll()
{
	bitset = 0;
	return *this;
}

void Pcsx2Config::GamefixOptions::Set(GamefixId id, bool enabled)
{
	switch (id)
	{
		case Fix_VuAddSub:            VuAddSubHack            = enabled; break;
		case Fix_FpuMultiply:         FpuMulHack              = enabled; break;
		case Fix_XGKick:              XgKickHack              = enabled; break;
		case Fix_EETiming:            EETimingHack            = enabled; break;
		case Fix_InstantDMA:          InstantDMAHack          = enabled; break;
		case Fix_SoftwareRendererFMV: SoftwareRendererFMVHack = enabled; break;
		case Fix_SkipMpeg:            SkipMPEGHack            = enabled; break;
		case Fix_OPHFlag:             OPHFlagHack             = enabled; break;
		case Fix_DMABusy:             DMABusyHack             = enabled; break;
		case Fix_VIFFIFO:             VIFFIFOHack             = enabled; break;
		case Fix_VIF1Stall:           VIF1StallHack           = enabled; break;
		case Fix_GIFFIFO:             GIFFIFOHack             = enabled; break;
		case Fix_GoemonTlbMiss:       GoemonTlbHack           = enabled; break;
		case Fix_Ibit:                IbitHack                = enabled; break;
		case Fix_VUSync:              VUSyncHack              = enabled; break;
		case Fix_VUOverflow:          VUOverflowHack          = enabled; break;
		case Fix_BlitInternalFPS:     BlitInternalFPSHack     = enabled; break;
		case Fix_FullVU0Sync:         FullVU0SyncHack         = enabled; break;
		default:
					      break;
	}
}

bool Pcsx2Config::GamefixOptions::Get(GamefixId id) const
{
	switch (id)
	{
		case Fix_VuAddSub:            return VuAddSubHack;
		case Fix_FpuMultiply:         return FpuMulHack;
		case Fix_XGKick:              return XgKickHack;
		case Fix_EETiming:            return EETimingHack;
		case Fix_InstantDMA:          return InstantDMAHack;
		case Fix_SoftwareRendererFMV: return SoftwareRendererFMVHack;
		case Fix_SkipMpeg:            return SkipMPEGHack;
		case Fix_OPHFlag:             return OPHFlagHack;
		case Fix_DMABusy:             return DMABusyHack;
		case Fix_VIFFIFO:             return VIFFIFOHack;
		case Fix_VIF1Stall:           return VIF1StallHack;
		case Fix_GIFFIFO:             return GIFFIFOHack;
		case Fix_GoemonTlbMiss:       return GoemonTlbHack;
		case Fix_Ibit:                return IbitHack;
		case Fix_VUSync:              return VUSyncHack;
		case Fix_VUOverflow:          return VUOverflowHack;
		case Fix_BlitInternalFPS:     return BlitInternalFPSHack;
		case Fix_FullVU0Sync:         return FullVU0SyncHack;
		default:
					      break;
	}
	return false; // unreachable, but we still need to suppress warnings >_<
}

void Pcsx2Config::GamefixOptions::LoadSave(SettingsWrapper& wrap)
{
	SettingsWrapSection("EmuCore/Gamefixes");

	SettingsWrapBitBool(VuAddSubHack);
	SettingsWrapBitBool(FpuMulHack);
	SettingsWrapBitBool(XgKickHack);
	SettingsWrapBitBool(EETimingHack);
	SettingsWrapBitBool(InstantDMAHack);
	SettingsWrapBitBool(SoftwareRendererFMVHack);
	SettingsWrapBitBool(SkipMPEGHack);
	SettingsWrapBitBool(OPHFlagHack);
	SettingsWrapBitBool(DMABusyHack);
	SettingsWrapBitBool(VIFFIFOHack);
	SettingsWrapBitBool(VIF1StallHack);
	SettingsWrapBitBool(GIFFIFOHack);
	SettingsWrapBitBool(GoemonTlbHack);
	SettingsWrapBitBool(IbitHack);
	SettingsWrapBitBool(VUSyncHack);
	SettingsWrapBitBool(VUOverflowHack);
	SettingsWrapBitBool(BlitInternalFPSHack);
	SettingsWrapBitBool(FullVU0SyncHack);
}

Pcsx2Config::FilenameOptions::FilenameOptions()
{
}

void Pcsx2Config::FilenameOptions::LoadSave(SettingsWrapper& wrap)
{
	SettingsWrapSection("Filenames");

	wrap.Entry(CURRENT_SETTINGS_SECTION, "BIOS", Bios, Bios);
}

void Pcsx2Config::FramerateOptions::SanityCheck()
{
       // Ensure Conformation of various options...

       TurboScalar = std::clamp(TurboScalar, 0.05f, 10.0f);
}

void Pcsx2Config::FramerateOptions::LoadSave(SettingsWrapper& wrap)
{
       SettingsWrapSection("Framerate");

       SettingsWrapEntry(TurboScalar);
}


Pcsx2Config::Pcsx2Config()
{
	bitset = 0;
	// Set defaults for fresh installs / reset settings
	McdEnableEjection = true;
	McdFolderAutoManage = true;
	EnablePatches = true;
	EnableGameFixes = true;

	// To be moved to FileMemoryCard pluign (someday)
	for (uint slot = 0; slot < 8; ++slot)
	{
		Mcd[slot].Enabled = !FileMcd_IsMultitapSlot(slot); // enables main 2 slots
		Mcd[slot].Filename = FileMcd_GetDefaultName(slot);

		// Folder memory card is autodetected later.
		Mcd[slot].Type = MemoryCardType::File;
	}

	GzipIsoIndexTemplate = "$(f).pindex.tmp";
}

void Pcsx2Config::LoadSave(SettingsWrapper& wrap)
{
	SettingsWrapSection("EmuCore");

	SettingsWrapBitBool(EnablePatches);
	SettingsWrapBitBool(EnableCheats);
	SettingsWrapBitBool(EnableWideScreenPatches);
	SettingsWrapBitBool(EnableNoInterlacingPatches);
	SettingsWrapBitBool(EnableGameFixes);
	SettingsWrapBitBool(HostFs);

	SettingsWrapBitBool(McdEnableEjection);
	SettingsWrapBitBool(McdFolderAutoManage);

	// Process various sub-components:

	Speedhacks.LoadSave(wrap);
	Cpu.LoadSave(wrap);
	GS.LoadSave(wrap);
	DEV9.LoadSave(wrap);
	Gamefixes.LoadSave(wrap);

	SettingsWrapEntry(GzipIsoIndexTemplate);

	BaseFilenames.LoadSave(wrap);
	Framerate.LoadSave(wrap);
	LoadSaveMemcards(wrap);
}

void Pcsx2Config::LoadSaveMemcards(SettingsWrapper& wrap)
{
	for (uint slot = 0; slot < 2; ++slot)
	{
		wrap.Entry("MemoryCards", StringUtil::StdStringFromFormat("Slot%u_Enable", slot + 1).c_str(),
			Mcd[slot].Enabled, Mcd[slot].Enabled);
		wrap.Entry("MemoryCards", StringUtil::StdStringFromFormat("Slot%u_Filename", slot + 1).c_str(),
			Mcd[slot].Filename, Mcd[slot].Filename);
	}

	for (uint slot = 2; slot < 8; ++slot)
	{
		int mtport = FileMcd_GetMtapPort(slot) + 1;
		int mtslot = FileMcd_GetMtapSlot(slot) + 1;

		wrap.Entry("MemoryCards", StringUtil::StdStringFromFormat("Multitap%u_Slot%u_Enable", mtport, mtslot).c_str(),
			Mcd[slot].Enabled, Mcd[slot].Enabled);
		wrap.Entry("MemoryCards", StringUtil::StdStringFromFormat("Multitap%u_Slot%u_Filename", mtport, mtslot).c_str(),
			Mcd[slot].Filename, Mcd[slot].Filename);
	}
}

bool Pcsx2Config::MultitapEnabled(uint port) const
{
	return (port == 0) ? MultitapPort0_Enabled : MultitapPort1_Enabled;
}

std::string Pcsx2Config::FullpathToBios() const
{
	std::string ret;
	if (!BaseFilenames.Bios.empty())
		ret = Path::Combine(EmuFolders::Bios, BaseFilenames.Bios);
	return ret;
}

std::string Pcsx2Config::FullpathToMcd(uint slot) const
{
	return Path::Combine(EmuFolders::MemoryCards, Mcd[slot].Filename);
}

bool Pcsx2Config::operator==(const Pcsx2Config& right) const
{
	bool equal =
		OpEqu(bitset) &&
		OpEqu(Cpu) &&
		OpEqu(GS) &&
		OpEqu(DEV9) &&
		OpEqu(Speedhacks) &&
		OpEqu(Gamefixes) &&
		OpEqu(Framerate) &&
		OpEqu(BaseFilenames) &&
		OpEqu(GzipIsoIndexTemplate);
	for (u32 i = 0; i < sizeof(Mcd) / sizeof(Mcd[0]); i++)
	{
		equal &= OpEqu(Mcd[i].Enabled);
		equal &= OpEqu(Mcd[i].Filename);
	}

	return equal;
}

void Pcsx2Config::CopyRuntimeConfig(Pcsx2Config& cfg)
{
	UseBOOT2Injection = cfg.UseBOOT2Injection;
	CurrentIRX = std::move(cfg.CurrentIRX);
	CurrentGameArgs = std::move(cfg.CurrentGameArgs);

	for (u32 i = 0; i < sizeof(Mcd) / sizeof(Mcd[0]); i++)
	{
		Mcd[i].Type = cfg.Mcd[i].Type;
	}
}

void EmuFolders::SetDefaults(SettingsInterface& si)
{
	si.SetStringValue("Folders", "Bios", "bios");
	si.SetStringValue("Folders", "MemoryCards", "memcards");
	si.SetStringValue("Folders", "Cheats", "cheats");
	si.SetStringValue("Folders", "CheatsWS", "cheats_ws");
	si.SetStringValue("Folders", "CheatsNI", "cheats_ni");
	si.SetStringValue("Folders", "Cache", "cache");
	si.SetStringValue("Folders", "Textures", "textures");
}

static std::string LoadPathFromSettings(SettingsInterface& si, const std::string& root, const char* name, const char* def)
{
	std::string value = si.GetStringValue("Folders", name, def);
	if (!Path::IsAbsolute(value))
		value = Path::Combine(root, value);
	return value;
}

void EmuFolders::LoadConfig(SettingsInterface& si)
{
	Bios        = LoadPathFromSettings(si, DataRoot, "Bios", "bios");
	MemoryCards = LoadPathFromSettings(si, DataRoot, "MemoryCards", "memcards");
	Cheats      = LoadPathFromSettings(si, DataRoot, "Cheats", "cheats");
	CheatsWS    = LoadPathFromSettings(si, DataRoot, "CheatsWS", "cheats_ws");
	CheatsNI    = LoadPathFromSettings(si, DataRoot, "CheatsNI", "cheats_ni");
	Cache       = LoadPathFromSettings(si, DataRoot, "Cache", "cache");
	Textures    = LoadPathFromSettings(si, DataRoot, "Textures", "textures");
}

void EmuFolders::EnsureFoldersExist()
{
	if (!path_is_valid(Bios.c_str()))
		path_mkdir(Bios.c_str());
	if (!path_is_valid(MemoryCards.c_str()))
		path_mkdir(MemoryCards.c_str());
	if (!path_is_valid(Cheats.c_str()))
		path_mkdir(Cheats.c_str());
	if (!path_is_valid(CheatsWS.c_str()))
		path_mkdir(CheatsWS.c_str());
	if (!path_is_valid(CheatsNI.c_str()))
		path_mkdir(CheatsNI.c_str());
	if (!path_is_valid(Cache.c_str()))
		path_mkdir(Cache.c_str());
	if (!path_is_valid(Textures.c_str()))
		path_mkdir(Textures.c_str());
}
