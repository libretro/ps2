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

#pragma once

#include <array>
#include <string>
#include <optional>
#include <vector>

#include "../common/General.h"
#include "../common/FPControl.h"

class SettingsInterface;
class SettingsWrapper;

enum class CDVD_SourceType : uint8_t;

/// Generic setting information which can be reused in multiple components.
struct SettingInfo
{
	using GetOptionsCallback = std::vector<std::pair<std::string, std::string>>(*)();

	enum class Type
	{
		Boolean,
		Integer,
		IntegerList,
		Float,
		String,
		StringList,
		Path,
	};

	Type type;
	const char* name;
	const char* display_name;
	const char* description;
	const char* default_value;
	const char* min_value;
	const char* max_value;
	const char* step_value;
	const char* format;
	const char* const* options; // For integer lists.
	GetOptionsCallback get_options; // For string lists.
	float multiplier;

	const char* StringDefaultValue() const;
	bool BooleanDefaultValue() const;
	s32 IntegerDefaultValue() const;
	s32 IntegerMinValue() const;
	s32 IntegerMaxValue() const;
	s32 IntegerStepValue() const;
	float FloatDefaultValue() const;
	float FloatMinValue() const;
	float FloatMaxValue() const;
	float FloatStepValue() const;
};

enum class GenericInputBinding : u8;

// TODO(Stenzek): Move to InputCommon.h or something?
struct InputBindingInfo
{
	enum class Type : u8
	{
		Unknown,
		Button,
		Axis,
		HalfAxis,
		Motor,
		Pointer, // Receive relative mouse movement events, bind_index is offset by the axis.
		Keyboard, // Receive host key events, bind_index is offset by the key code.
		Device, // Used for special-purpose device selection, e.g. force feedback.
		Macro,
	};

	const char* name;
	const char* display_name;
	Type bind_type;
	u16 bind_index;
	GenericInputBinding generic_mapping;
};

/// Generic input bindings. These roughly match a DualShock 4 or XBox One controller.
/// They are used for automatic binding to PS2 controller types.
enum class GenericInputBinding : u8
{
	Unknown,

	DPadUp,
	DPadRight,
	DPadLeft,
	DPadDown,

	LeftStickUp,
	LeftStickRight,
	LeftStickDown,
	LeftStickLeft,
	L3,

	RightStickUp,
	RightStickRight,
	RightStickDown,
	RightStickLeft,
	R3,

	Triangle, // Y on XBox pads.
	Circle, // B on XBox pads.
	Cross, // A on XBox pads.
	Square, // X on XBox pads.

	Select, // Share on DS4, View on XBox pads.
	Start, // Options on DS4, Menu on XBox pads.
	System, // PS button on DS4, Guide button on XBox pads.

	L1, // LB on Xbox pads.
	L2, // Left trigger on XBox pads.
	R1, // RB on XBox pads.
	R2, // Right trigger on Xbox pads.

	SmallMotor, // High frequency vibration.
	LargeMotor, // Low frequency vibration.

	Count,
};

enum GamefixId
{
	GamefixId_FIRST = 0,

	Fix_FpuMultiply = GamefixId_FIRST,
	Fix_GoemonTlbMiss,
	Fix_SoftwareRendererFMV,
	Fix_SkipMpeg,
	Fix_OPHFlag,
	Fix_EETiming,
	Fix_InstantDMA,
	Fix_DMABusy,
	Fix_GIFFIFO,
	Fix_VIFFIFO,
	Fix_VIF1Stall,
	Fix_VuAddSub,
	Fix_Ibit,
	Fix_VUSync,
	Fix_VUOverflow,
	Fix_XGKick,
	Fix_BlitInternalFPS,
	Fix_FullVU0Sync,

	GamefixId_COUNT
};

// TODO - config - not a fan of the excessive use of enums and macros to make them work
// a proper object would likely make more sense (if possible).

enum class SpeedHack
{
	MVUFlag,
	InstantVU1,
	MTVU,
	EECycleRate,
	MaxCount,
};

enum class VsyncMode
{
	Off,
	On,
	Adaptive,
};

enum class MemoryCardType
{
	Empty,
	File,
	MaxCount
};

enum class MemoryCardFileType
{
	Unknown,
	PS2_8MB,
	PS2_16MB,
	PS2_32MB,
	PS2_64MB,
	PS1,
	MaxCount
};

enum class GSRendererType : s8
{
	Auto = -1,
	DX11 = 3,
	OGL = 12,
	SW = 13,
	VK = 14,
	DX12 = 15,
	ParallelGS = 18
};

enum class GSInterlaceMode : u8
{
	Automatic,
	Off,
	WeaveTFF,
	WeaveBFF,
	BobTFF,
	BobBFF,
	BlendTFF,
	BlendBFF,
	AdaptiveTFF,
	AdaptiveBFF,
	Count
};

// Ordering was done to keep compatibility with older ini file.
enum class BiFiltering : u8
{
	Nearest,
	Forced,
	PS2,
	Forced_But_Sprite,
};

enum class GSHWMipmapMode : u8
{
	Disabled,
	Enabled,
	AllLevels,
	Unclamped,
	MaxCount
};

enum class TriFiltering : s8
{
	Automatic = -1,
	Off,
	PS2,
	Forced,
};

enum class AccBlendLevel : u8
{
	Minimum,
	Basic,
	Medium,
	High,
	Full,
	Maximum,
};

enum class TexturePreloadingLevel : u8
{
	Off,
	Partial,
	Full,
};

enum class GSHardwareDownloadMode : u8
{
	Enabled,
	NoReadbacks,
	Unsynchronized,
	Disabled
};

enum class GSHWAutoFlushLevel : u8
{
	Disabled,
	SpritesOnly,
	Enabled,
};

enum class GSGPUTargetCLUTMode : u8
{
	Disabled,
	Enabled,
	InsideTarget,
};

enum class GSTextureInRtMode : u8
{
	Disabled,
	InsideTargets,
	MergeTargets,
};

enum class GSBilinearDirtyMode : u8
{
	Automatic,
	ForceBilinear,
	ForceNearest,
	MaxCount
};

enum GSHalfPixelOffset
{
	Off = 0,
	Normal,
	Special,
	SpecialAggressive,
	Native,
	MaxCount
};

enum GSNativeScaling
{
	NativeScaling_Normal = 0,
	NativeScaling_Aggressive,
	NativeScaling_Off,
	NativeScaling_MaxCount
};

// Template function for casting enumerations to their underlying type
template <typename Enumeration>
typename std::underlying_type<Enumeration>::type enum_cast(Enumeration E)
{
	return static_cast<typename std::underlying_type<Enumeration>::type>(E);
}

ImplementEnumOperators(GamefixId);

// --------------------------------------------------------------------------------------
//  Pcsx2Config class
// --------------------------------------------------------------------------------------
// This is intended to be a public class library between the core emulator and GUI only.
//
// When GUI code performs modifications of this class, it must be done with strict thread
// safety, since the emu runs on a separate thread.  Additionally many components of the
// class require special emu-side resets or state save/recovery to be applied.  Please
// use the provided functions to lock the emulation into a safe state and then apply
// chances on the necessary scope (see Core_Pause, Core_ApplySettings, and Core_Resume).
//
struct Pcsx2Config
{
	// ------------------------------------------------------------------------
	struct RecompilerOptions
	{
		union
		{
			u32 bitset;
			struct 
			{
				bool
					EnableEE         : 1,
					EnableIOP        : 1,
				        EnableVU0        : 1,
				        EnableVU1        : 1;

				bool
					vu0Overflow      : 1,
					vu0ExtraOverflow : 1,
				        vu0SignOverflow  : 1,
				        vu0Underflow     : 1;

				bool
					vu1Overflow      : 1,
					vu1ExtraOverflow : 1,
					vu1SignOverflow  : 1,
					vu1Underflow     : 1;

				bool
					fpuOverflow      : 1,
					fpuExtraOverflow : 1,
					fpuFullMode      : 1;

				bool    EnableEECache    : 1;
				bool    EnableFastmem    : 1;
			};
		};

		RecompilerOptions();
		void ApplySanityCheck();

		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const RecompilerOptions& right) const
		{
			return bitset == right.bitset;
		}

		bool operator!=(const RecompilerOptions& right) const
		{
			return !(bitset == right.bitset);
		}

		u32 GetEEClampMode() const
		{
			return fpuFullMode ? 3 : (fpuExtraOverflow ? 2 : (fpuOverflow ? 1 : 0));
		}

		void SetEEClampMode(u32 value)
		{
			fpuOverflow = (value >= 1);
			fpuExtraOverflow = (value >= 2);
			fpuFullMode = (value >= 3);
		}

		u32 GetVUClampMode() const
		{
			return vu0SignOverflow ? 3 : (vu0ExtraOverflow ? 2 : (vu0Overflow ? 1 : 0));
		}
	};

	// ------------------------------------------------------------------------
	struct CpuOptions
	{
		RecompilerOptions Recompiler;

		FPControlRegister FPUFPCR;
		FPControlRegister FPUDivFPCR;
		FPControlRegister VU0FPCR;
		FPControlRegister VU1FPCR;

		u32 AffinityControlMode;

		CpuOptions();
		void LoadSave(SettingsWrapper& wrap);
		void ApplySanityCheck();

		bool CpusChanged(const CpuOptions& right) const;

		bool operator==(const CpuOptions& right) const
		{
			return     (FPUFPCR    == right.FPUFPCR) 
				&& (VU0FPCR == right.VU0FPCR)
				&& (VU1FPCR == right.VU1FPCR)
				&& (AffinityControlMode == right.AffinityControlMode)
				&& (Recompiler  == right.Recompiler);
		}

		bool operator!=(const CpuOptions& right) const
		{
			return !this->operator==(right);
		}
	};

	// ------------------------------------------------------------------------
	struct GSOptions
	{
		static constexpr float DEFAULT_FRAME_RATE_NTSC = 59.94f;
		static constexpr float DEFAULT_FRAME_RATE_PAL = 50.00f;

		union
		{
			u64 bitset;

			struct
			{
				bool
					PCRTCAntiBlur : 1,
					DisableInterlaceOffset : 1,
					PCRTCOffsets : 1,
					PCRTCOverscan : 1,
					UseDebugDevice : 1,
					DisableShaderCache : 1,
					DisableFramebufferFetch : 1,
					DisableVertexShaderExpand : 1,
					SkipDuplicateFrames : 1;

				bool
					GPUPaletteConversion : 1,
					AutoFlushSW : 1,
					PreloadFrameWithGSData : 1,
					Mipmap : 1,
					ManualUserHacks : 1,
					UserHacks_AlignSpriteX : 1,
					UserHacks_CPUFBConversion : 1,
					UserHacks_ReadTCOnClose : 1,
					UserHacks_DisableDepthSupport : 1,
					UserHacks_DisablePartialInvalidation : 1,
					UserHacks_DisableSafeFeatures : 1,
					UserHacks_DisableRenderFixes : 1,
					UserHacks_MergePPSprite : 1,
					UserHacks_ForceEvenSpritePosition : 1,
					UserHacks_NativePaletteDraw : 1,
					UserHacks_EstimateTextureRegion : 1,
					LoadTextureReplacements : 1,
					LoadTextureReplacementsAsync : 1,
					PrecacheTextureReplacements : 1;
			};
		};

		int VsyncQueueSize = 0;

		float FramerateNTSC = DEFAULT_FRAME_RATE_NTSC;
		float FrameratePAL = DEFAULT_FRAME_RATE_PAL;

		GSInterlaceMode InterlaceMode = GSInterlaceMode::Automatic;

		GSRendererType Renderer = GSRendererType::Auto;
		float UpscaleMultiplier = 1.0f;

		AccBlendLevel AccurateBlendingUnit = AccBlendLevel::Basic;
		GSHWMipmapMode HWMipmapMode = GSHWMipmapMode::Unclamped;
		BiFiltering TextureFiltering = BiFiltering::PS2;
		TexturePreloadingLevel TexturePreloading = TexturePreloadingLevel::Full;
		GSHardwareDownloadMode HWDownloadMode = GSHardwareDownloadMode::Enabled;
		u8 Dithering = 3;
		u8 MaxAnisotropy = 0;
		s16 GetSkipCountFunctionId = -1;
		s16 BeforeDrawFunctionId = -1;
		s16 MoveHandlerFunctionId = -1;
		int SkipDrawStart = 0;
		int SkipDrawEnd = 0;

		GSHWAutoFlushLevel UserHacks_AutoFlush = GSHWAutoFlushLevel::Disabled;
		GSHalfPixelOffset UserHacks_HalfPixelOffset = GSHalfPixelOffset::Off;
		s8 UserHacks_RoundSprite = 0;
		GSNativeScaling UserHacks_NativeScaling = GSNativeScaling::NativeScaling_Normal;
		s32 UserHacks_TCOffsetX = 0;
		s32 UserHacks_TCOffsetY = 0;
		u8 UserHacks_CPUSpriteRenderBW = 0;
		u8 UserHacks_CPUSpriteRenderLevel = 0;
		u8 UserHacks_CPUCLUTRender = 0;
		GSGPUTargetCLUTMode UserHacks_GPUTargetCLUTMode = GSGPUTargetCLUTMode::Disabled;
		GSTextureInRtMode UserHacks_TextureInsideRt = GSTextureInRtMode::Disabled;
		GSBilinearDirtyMode UserHacks_BilinearHack = GSBilinearDirtyMode::Automatic;
		TriFiltering TriFilter = TriFiltering::Automatic;
		s8 OverrideTextureBarriers = -1;
		u8 PGSSuperSampling = 0;
		u8 PGSHighResScanout = 0;
		u8 PGSDisableMipmaps = 0;
		u8 PGSSuperSampleTextures = 0;
		u8 PGSSharpBackbuffer = 0;

		u16 SWExtraThreads = 2;
		u16 SWExtraThreadsHeight = 4;

		std::string Adapter;

		GSOptions();

		void LoadSave(SettingsWrapper& wrap);

		/// Sets user hack values to defaults when user hacks are not enabled.
		void MaskUserHacks();

		/// Sets user hack values to defaults when upscaling is not enabled.
		void MaskUpscalingHacks();

		/// Returns true if any of the hardware renderers are selected.
		bool UseHardwareRenderer() const;

		/// Returns false if the compared to the old settings, we need to reopen GS.
		/// (i.e. renderer change, swap chain mode change, etc.)
		bool RestartOptionsAreEqual(const GSOptions& right) const;

		/// Returns false if any options need to be applied to the MTGS.
		bool OptionsAreEqual(const GSOptions& right) const;

		bool operator==(const GSOptions& right) const;
		bool operator!=(const GSOptions& right) const;
	};

	struct DEV9Options
	{
		enum struct NetApi : int
		{
			Unset = 0,
			PCAP_Bridged = 1,
			PCAP_Switched = 2,
			TAP = 3,
			Sockets = 4,
		};
		static const char* NetApiNames[];

		enum struct DnsMode : int
		{
			Manual = 0,
			Auto = 1,
			Internal = 2,
		};
		static const char* DnsModeNames[];

		struct HostEntry
		{
			std::string Url;
			std::string Desc;
			u8 Address[4]{};
			bool Enabled;

			bool operator==(const HostEntry& right) const
			{
				return     (Url  == right.Url)
					&& (Desc == right.Desc)
					&& (*(int*)Address == *(int*)right.Address)
					&& (Enabled == right.Enabled);
			}

			bool operator!=(const HostEntry& right) const
			{
				return !this->operator==(right);
			}
		};

		bool EthEnable{false};
		NetApi EthApi{NetApi::Unset};
		std::string EthDevice;
		bool EthLogDNS{false};

		bool InterceptDHCP{false};
		u8 PS2IP[4]{};
		u8 Mask[4]{};
		u8 Gateway[4]{};
		u8 DNS1[4]{};
		u8 DNS2[4]{};
		bool AutoMask{true};
		bool AutoGateway{true};
		DnsMode ModeDNS1{DnsMode::Auto};
		DnsMode ModeDNS2{DnsMode::Auto};

		std::vector<HostEntry> EthHosts;

		bool HddEnable{false};
		std::string HddFile;

		/* The PS2's HDD max size is 2TB
		 * which is 2^32 * 512 byte sectors
		 * Note that we don't yet support
		 * 48bit LBA, so our limit is lower */
		uint HddSizeSectors{40 * (1024 * 1024 * 1024 / 512)};

		DEV9Options();

		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const DEV9Options& right) const
		{
			return     (EthEnable == right.EthEnable)
				&& (EthApi    == right.EthApi)
				&& (EthDevice == right.EthDevice)
				&& (EthLogDNS == right.EthLogDNS)
				&& (InterceptDHCP == right.InterceptDHCP)
				&& (*(int*)PS2IP == *(int*)right.PS2IP)
				&& (*(int*)Gateway == *(int*)right.Gateway)
				&& (*(int*)DNS1 == *(int*)right.DNS1)
				&& (*(int*)DNS2 == *(int*)right.DNS2)
				&& (AutoMask    == right.AutoMask)
				&& (AutoGateway == right.AutoGateway)
				&& (ModeDNS1    == right.ModeDNS1)
				&& (ModeDNS2    == right.ModeDNS2)
				&& (EthHosts    == right.EthHosts)
				&& (HddEnable   == right.HddEnable)
				&& (HddFile     == right.HddFile)
				&& (HddSizeSectors == right.HddSizeSectors);
		}

		bool operator!=(const DEV9Options& right) const
		{
			return !this->operator==(right);
		}

	protected:
		static void LoadIPHelper(u8* field, const std::string& setting);
		static std::string SaveIPHelper(u8* field);
	};

	// ------------------------------------------------------------------------
	// NOTE: The GUI's GameFixes panel is dependent on the order of bits in this structure.
	struct GamefixOptions
	{
		union
		{
			u32 bitset;
			struct
			{
				bool
					FpuMulHack    : 1, // Tales of Destiny hangs.
					GoemonTlbHack : 1, // Goemon TLB miss hack. The game need to access unmapped virtual address. Instead to handle it as exception, TLB are preloaded at startup
				        SoftwareRendererFMVHack : 1, // Switches to software renderer for FMVs
				        SkipMPEGHack   : 1, // Skips MPEG videos (Katamari and other games need this)
				        OPHFlagHack    : 1, // Bleach Blade Battlers
				        EETimingHack   : 1, // General purpose timing hack.
			                InstantDMAHack : 1, // Instantly complete DMA's if possible, good for cache emulation problems.
					DMABusyHack    : 1, // Denies writes to the DMAC when it's busy. This is correct behaviour but bad timing can cause problems.
					GIFFIFOHack    : 1, // Enabled the GIF FIFO (more correct but slower)
					VIFFIFOHack    : 1, // Pretends to fill the non-existant VIF FIFO Buffer.
					VIF1StallHack  : 1, // Like above, processes FIFO data before the stall is allowed (to make sure data goes over).
					VuAddSubHack   : 1, // Tri-ace games, they use an encryption algorithm that requires VU ADDI opcode to be bit-accurate.
					IbitHack       : 1, // I bit hack. Needed to stop constant VU recompilation in some games
					VUSyncHack     : 1, // Makes microVU run behind the EE to avoid VU register reading/writing sync issues. Useful for M-Bit games
					VUOverflowHack : 1, // Tries to simulate overflow flag checks (not really possible on x86 without soft floats)
					XgKickHack     : 1, // Erementar Gerad, adds more delay to VU XGkick instructions. Corrects the color of some graphics, but breaks Tri-ace games and others.
					BlitInternalFPSHack : 1, // Disables privileged register write-based FPS detection.
					FullVU0SyncHack     : 1; // Forces tight VU0 sync on every COP2 instruction.
			};
		};

		GamefixOptions();
		void LoadSave(SettingsWrapper& wrap);
		GamefixOptions& DisableAll();

		bool Get(GamefixId id) const;
		void Set(GamefixId id, bool enabled = true);
		void Clear(GamefixId id) { Set(id, false); }

		bool operator==(const GamefixOptions& right) const
		{
			return (bitset == right.bitset);
		}

		bool operator!=(const GamefixOptions& right) const
		{
			return !(bitset == right.bitset);
		}
	};

	// ------------------------------------------------------------------------
	struct SpeedhackOptions
	{
		static constexpr s8 MIN_EE_CYCLE_RATE = -3;
		static constexpr s8 MAX_EE_CYCLE_RATE = 3;
		static constexpr u8 MAX_EE_CYCLE_SKIP = 3;

		union
		{
			u32 bitset;
			struct
			{
				bool fastCDVD   : 1, // enables fast CDVD access
				     IntcStat   : 1, // fast-forward through INTC_STAT waits.
				     WaitLoop   : 1, // enables constant loop detection and fast-forwarding
				     vuFlagHack : 1, // microVU specific flag hack
				     vuThread   : 1, // Enable Threaded VU1
				     vu1Instant : 1; // Enable Instant VU1 (Without MTVU only)
			};
		};

		s8 EECycleRate; // EE cycle rate selector (1.0, 1.5, 2.0)
		u8 EECycleSkip; // EE Cycle skip factor (0, 1, 2, or 3)

		SpeedhackOptions();
		void LoadSave(SettingsWrapper& conf);
		SpeedhackOptions& DisableAll();

		void Set(SpeedHack id, int value);
		bool operator==(const SpeedhackOptions& right) const;
		bool operator!=(const SpeedhackOptions& right) const;
		static const char* GetSpeedHackName(SpeedHack id);
		static std::optional<SpeedHack> ParseSpeedHackName(const std::string_view& name);
	};

	// ------------------------------------------------------------------------
	struct FramerateOptions
	{
		float TurboScalar{2.0f};

		void LoadSave(SettingsWrapper& wrap);
		void SanityCheck();

		bool operator==(const FramerateOptions& right) const
		{
			return (TurboScalar == right.TurboScalar);
		}

		bool operator!=(const FramerateOptions& right) const
		{
			return !this->operator==(right);
		}
	};



	// ------------------------------------------------------------------------
	struct FilenameOptions
	{
		std::string Bios;

		FilenameOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const FilenameOptions& right) const
		{
			return (Bios == right.Bios);
		}

		bool operator!=(const FilenameOptions& right) const
		{
			return !this->operator==(right);
		}
	};

	// ------------------------------------------------------------------------
	struct USBOptions
	{
		enum : u32
		{
			NUM_PORTS = 2
		};

		struct Port
		{
			s32 DeviceType;
			u32 DeviceSubtype;

			bool operator==(const USBOptions::Port& right) const;
			bool operator!=(const USBOptions::Port& right) const;
		};

		std::array<Port, NUM_PORTS> Ports;

		USBOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const USBOptions& right) const;
		bool operator!=(const USBOptions& right) const;
	};

	// ------------------------------------------------------------------------
	// Options struct for each memory card.
	//
	struct McdOptions
	{
		std::string Filename; // user-configured location of this memory card
		bool Enabled; // memory card enabled (if false, memcard will not show up in-game)
		MemoryCardType Type; // the memory card implementation that should be used
	};

	// ------------------------------------------------------------------------

	// ------------------------------------------------------------------------

	union
	{
		u32 bitset;
		struct
		{
			bool EnablePatches : 1, // enables patch detection and application
			     EnableCheats  : 1, // enables cheat detection and application
			     EnableWideScreenPatches    : 1,
			     EnableNoInterlacingPatches : 1,
		             EnableGameFixes            : 1, // enables automatic game fixes
			     // When enabled uses BOOT2 injection, skipping sony bios splashes
			     UseBOOT2Injection          : 1,
			     // Enables simulated ejection of memory cards when loading savestates
			     McdEnableEjection          : 1,
			     McdFolderAutoManage        : 1,

			     MultitapPort0_Enabled      : 1,
			     MultitapPort1_Enabled      : 1,

			     HostFs                     : 1;

			// uses automatic NTFS compression when creating new memory cards (Win32 only)
		};
	};

	CpuOptions Cpu;
	GSOptions GS;
	SpeedhackOptions Speedhacks;
	GamefixOptions Gamefixes;
	FramerateOptions Framerate;
	DEV9Options DEV9;
#if 0
	USBOptions USB;
#endif

	FilenameOptions BaseFilenames;

	// Memorycard options - first 2 are default slots, last 6 are multitap 1 and 2
	// slots (3 each)
	McdOptions Mcd[8];
	std::string GzipIsoIndexTemplate; // for quick-access index with gzipped ISO

	// Set at runtime, not loaded from config.
	std::string CurrentIRX;
	std::string CurrentGameArgs;

	Pcsx2Config();
	void LoadSave(SettingsWrapper& wrap);
	void LoadSaveMemcards(SettingsWrapper& wrap);

	std::string FullpathToBios() const;
	std::string FullpathToMcd(uint slot) const;

	bool MultitapEnabled(uint port) const;

	bool operator==(const Pcsx2Config& right) const;
	bool operator!=(const Pcsx2Config& right) const
	{
		return !this->operator==(right);
	}

	/// Copies runtime configuration settings (e.g. frame limiter state).
	void CopyRuntimeConfig(Pcsx2Config& cfg);
};

extern Pcsx2Config EmuConfig;

namespace EmuFolders
{
	extern std::string AppRoot;
	extern std::string DataRoot;
	extern std::string Bios;
	extern std::string MemoryCards;
	extern std::string Cheats;
	extern std::string CheatsWS;
	extern std::string CheatsNI;
	extern std::string Resources;
	extern std::string Cache;
	extern std::string Textures;

	// Assumes that AppRoot and DataRoot have been initialized.
	void SetDefaults(SettingsInterface& si);
	void LoadConfig(SettingsInterface& si);
	void EnsureFoldersExist();
} // namespace EmuFolders

/////////////////////////////////////////////////////////////////////////////////////////
// Helper Macros for Reading Emu Configurations.
//

// ------------ CPU / Recompiler Options ---------------

#define THREAD_VU1 (EmuConfig.Cpu.Recompiler.EnableVU1 && EmuConfig.Speedhacks.vuThread)
#define INSTANT_VU1 (EmuConfig.Speedhacks.vu1Instant)
#define CHECK_EEREC (EmuConfig.Cpu.Recompiler.EnableEE)
#define CHECK_CACHE (EmuConfig.Cpu.Recompiler.EnableEECache)
#define CHECK_IOPREC (EmuConfig.Cpu.Recompiler.EnableIOP)
#define CHECK_FASTMEM (EmuConfig.Cpu.Recompiler.EnableEE && EmuConfig.Cpu.Recompiler.EnableFastmem)

//------------ SPECIAL GAME FIXES!!! ---------------
#define CHECK_VUADDSUBHACK (EmuConfig.Gamefixes.VuAddSubHack) // Special Fix for Tri-ace games, they use an encryption algorithm that requires VU addi opcode to be bit-accurate.
#define CHECK_FPUMULHACK (EmuConfig.Gamefixes.FpuMulHack) // Special Fix for Tales of Destiny hangs.
#define CHECK_XGKICKHACK (EmuConfig.Gamefixes.XgKickHack) // Special Fix for Erementar Gerad, adds more delay to VU XGkick instructions. Corrects the color of some graphics.
#define CHECK_EETIMINGHACK (EmuConfig.Gamefixes.EETimingHack) // Fix all scheduled events to happen in 1 cycle.
#define CHECK_INSTANTDMAHACK (EmuConfig.Gamefixes.InstantDMAHack) // Attempt to finish DMA's instantly, useful for games which rely on cache emulation.
#define CHECK_SKIPMPEGHACK (EmuConfig.Gamefixes.SkipMPEGHack) // Finds sceMpegIsEnd pattern to tell the game the mpeg is finished (Katamari and a lot of games need this)
#define CHECK_OPHFLAGHACK (EmuConfig.Gamefixes.OPHFlagHack) // Bleach Blade Battlers
#define CHECK_DMABUSYHACK (EmuConfig.Gamefixes.DMABusyHack) // Denies writes to the DMAC when it's busy. This is correct behaviour but bad timing can cause problems.
#define CHECK_VIFFIFOHACK (EmuConfig.Gamefixes.VIFFIFOHack) // Pretends to fill the non-existant VIF FIFO Buffer.
#define CHECK_VIF1STALLHACK (EmuConfig.Gamefixes.VIF1StallHack) // Like above, processes FIFO data before the stall is allowed (to make sure data goes over).
#define CHECK_GIFFIFOHACK (EmuConfig.Gamefixes.GIFFIFOHack) // Enabled the GIF FIFO (more correct but slower)
#define CHECK_VUOVERFLOWHACK (EmuConfig.Gamefixes.VUOverflowHack) // Special Fix for Superman Returns, they check for overflows on PS2 floats which we can't do without soft floats.
#define CHECK_FULLVU0SYNCHACK (EmuConfig.Gamefixes.FullVU0SyncHack)

//------------ Advanced Options!!! ---------------
#define CHECK_VU_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0Overflow : EmuConfig.Cpu.Recompiler.vu1Overflow)
#define CHECK_VU_EXTRA_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0ExtraOverflow : EmuConfig.Cpu.Recompiler.vu1ExtraOverflow) // If enabled, Operands are clamped before being used in the VU recs
#define CHECK_VU_SIGN_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0SignOverflow : EmuConfig.Cpu.Recompiler.vu1SignOverflow)

#define CHECK_FPU_OVERFLOW (EmuConfig.Cpu.Recompiler.fpuOverflow)
#define CHECK_FPU_EXTRA_OVERFLOW (EmuConfig.Cpu.Recompiler.fpuExtraOverflow) // If enabled, Operands are checked for infinities before being used in the FPU recs
#define CHECK_FPU_FULL (EmuConfig.Cpu.Recompiler.fpuFullMode)

//------------ EE Recompiler defines - Comment to disable a recompiler ---------------

#define SHIFT_RECOMPILE // Speed majorly reduced if disabled
#define BRANCH_RECOMPILE // Speed extremely reduced if disabled - more then shift

// Disabling all the recompilers in this block is interesting, as it still runs at a reasonable rate.
// It also adds a few glitches. Really reminds me of the old Linux 64-bit version. --arcum42
#define ARITHMETICIMM_RECOMPILE
#define ARITHMETIC_RECOMPILE
#define MULTDIV_RECOMPILE
#define JUMP_RECOMPILE
#define LOADSTORE_RECOMPILE
#define MOVE_RECOMPILE
#define MMI_RECOMPILE
#define MMI0_RECOMPILE
#define MMI1_RECOMPILE
#define MMI2_RECOMPILE
#define MMI3_RECOMPILE
#define FPU_RECOMPILE
#define CP0_RECOMPILE
#define CP2_RECOMPILE

/* You can't recompile ARITHMETICIMM without ARITHMETIC. */
#ifndef ARITHMETIC_RECOMPILE
#undef ARITHMETICIMM_RECOMPILE
#endif
