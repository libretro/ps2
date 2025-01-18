/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include "common/HashCombine.h"
#include "common/WindowInfo.h"

#include "../../GS.h"
#include "../../GSAlignedClass.h"
#include "../../GSExtra.h"

#include "GSFastList.h"
#include "GSTexture.h"
#include "GSVertex.h"

enum class ShaderConvert
{
	COPY = 0,
	RGBA8_TO_16_BITS,
	DATM_1,
	DATM_0,
	DATM_1_RTA_CORRECTION,
	DATM_0_RTA_CORRECTION,
	HDR_INIT,
	HDR_RESOLVE,
	RTA_CORRECTION,
	RTA_DECORRECTION,
	TRANSPARENCY_FILTER,
	FLOAT32_TO_16_BITS,
	FLOAT32_TO_32_BITS,
	FLOAT32_TO_RGBA8,
	FLOAT32_TO_RGB8,
	FLOAT16_TO_RGB5A1,
	RGBA8_TO_FLOAT32,
	RGBA8_TO_FLOAT24,
	RGBA8_TO_FLOAT16,
	RGB5A1_TO_FLOAT16,
	RGBA8_TO_FLOAT32_BILN,
	RGBA8_TO_FLOAT24_BILN,
	RGBA8_TO_FLOAT16_BILN,
	RGB5A1_TO_FLOAT16_BILN,
	FLOAT32_TO_FLOAT24,
	DEPTH_COPY,
	DOWNSAMPLE_COPY,
	RGBA_TO_8I,
	CLUT_4,
	CLUT_8,
	YUV,
	Count
};

enum class SetDATM : u8
{
	DATM0 = 0U,
	DATM1,
	DATM0_RTA_CORRECTION,
	DATM1_RTA_CORRECTION
};

enum class ShaderInterlace
{
	WEAVE = 0,
	BOB = 1,
	BLEND = 2,
	MAD_BUFFER = 3,
	MAD_RECONSTRUCT = 4,
	Count
};

static inline bool HasDepthOutput(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::RGBA8_TO_FLOAT32:
		case ShaderConvert::RGBA8_TO_FLOAT24:
		case ShaderConvert::RGBA8_TO_FLOAT16:
		case ShaderConvert::RGB5A1_TO_FLOAT16:
		case ShaderConvert::RGBA8_TO_FLOAT32_BILN:
		case ShaderConvert::RGBA8_TO_FLOAT24_BILN:
		case ShaderConvert::RGBA8_TO_FLOAT16_BILN:
		case ShaderConvert::RGB5A1_TO_FLOAT16_BILN:
		case ShaderConvert::FLOAT32_TO_FLOAT24:
		case ShaderConvert::DEPTH_COPY:
			return true;
		default:
			break;
	}
	return false;
}

static inline u32 ShaderConvertWriteMask(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::FLOAT32_TO_RGB8:
			return 0x7;
		default:
			break;
	}
	return 0xf;
}

enum class PresentShader
{
	COPY = 0,
	Count
};

/// Get the name of a shader
/// (Can't put methods on an enum class)
int SetDATMShader(SetDATM datm);
const char* shaderName(ShaderConvert value);

enum ChannelFetch
{
	ChannelFetch_NONE  = 0,
	ChannelFetch_RED   = 1,
	ChannelFetch_GREEN = 2,
	ChannelFetch_BLUE  = 3,
	ChannelFetch_ALPHA = 4,
	ChannelFetch_RGB   = 5,
	ChannelFetch_GXBY  = 6
};

struct alignas(16) DisplayConstantBuffer
{
	GSVector4 SourceRect; // +0,xyzw
	GSVector4 TargetRect; // +16,xyzw
	GSVector2 SourceSize; // +32,xy
	GSVector2 TargetSize; // +40,zw
	GSVector2 TargetResolution; // +48,xy
	GSVector2 RcpTargetResolution; // +56,zw
	GSVector2 SourceResolution; // +64,xy
	GSVector2 RcpSourceResolution; // +72,zw
	// +96

	// assumes that sRect is normalized
	void SetSource(const GSVector4& sRect, const GSVector2i& sSize)
	{
		SourceRect = sRect;
		SourceResolution = GSVector2(static_cast<float>(sSize.x), static_cast<float>(sSize.y));
		RcpSourceResolution = GSVector2(1.0f) / SourceResolution;
		SourceSize = GSVector2((sRect.z - sRect.x) * SourceResolution.x, (sRect.w - sRect.y) * SourceResolution.y);
	}
	void SetTarget(const GSVector4& dRect, const GSVector2i& dSize)
	{
		TargetRect = dRect;
		TargetResolution = GSVector2(static_cast<float>(dSize.x), static_cast<float>(dSize.y));
		RcpTargetResolution = GSVector2(1.0f) / TargetResolution;
		TargetSize = GSVector2(dRect.z - dRect.x, dRect.w - dRect.y);
	}
};

struct alignas(16) MergeConstantBuffer
{
	GSVector4 BGColor;
	u32 EMODA;
	u32 EMODC;
	u32 DOFFSET;
	float ScaleFactor;
};

struct alignas(16) InterlaceConstantBuffer
{
	GSVector4 ZrH; // data passed to the shader
};

enum HWBlendFlags
{
	// Flags to determine blending behavior
	BLEND_CD     = 0x1,    // Output is Cd, hw blend can handle it
	BLEND_HW1    = 0x2,    // Clear color blending (use directly the destination color as blending factor)
	BLEND_HW2    = 0x4,    // Clear color blending (use directly the destination color as blending factor)
	BLEND_HW3    = 0x8,    // Multiply Cs by (255/128) to compensate for wrong Ad/255 value, should be Ad/128
	BLEND_HW4    = 0x10,   // HW rendering is split in 2 passes
	BLEND_HW5    = 0x20,   // HW rendering is split in 2 passes
	BLEND_HW6    = 0x40,   // HW rendering is split in 2 passes
	BLEND_HW7    = 0x80,   // HW rendering is split in 2 passes
	BLEND_HW8    = 0x100,  // HW rendering is split in 2 passes
	BLEND_HW9    = 0x200,  // HW rendering is split in 2 passes
	BLEND_MIX1   = 0x400,  // Mix of hw and sw, do Cs*F or Cs*As in shader
	BLEND_MIX2   = 0x800,  // Mix of hw and sw, do Cs*(As + 1) or Cs*(F + 1) in shader
	BLEND_MIX3   = 0x1000, // Mix of hw and sw, do Cs*(1 - As) or Cs*(1 - F) in shader
	BLEND_ACCU   = 0x2000, // Allow to use a mix of SW and HW blending to keep the best of the 2 worlds
	BLEND_NO_REC = 0x4000, // Doesn't require sampling of the RT as a texture
	BLEND_A_MAX  = 0x8000, // Impossible blending uses coeff bigger than 1
};

// Determines the HW blend function for DX11/OGL
struct HWBlend
{
	u16 flags;
	u8 op, src, dst;
};

struct alignas(16) GSHWDrawConfig
{
	enum class Topology: u8
	{
		Point,
		Line,
		Triangle,
	};
	enum class VSExpand: u8
	{
		None,
		Point,
		Line,
		Sprite
	};
#pragma pack(push, 1)
	struct VSSelector
	{
		union
		{
			struct
			{
				u8 fst : 1;
				u8 tme : 1;
				u8 iip : 1;
				u8 point_size : 1;		///< Set when points need to be expanded without VS expanding.
				VSExpand expand : 2;
				u8 _free : 2;
			};
			u8 key;
		};
		VSSelector(): key(0) {}
		VSSelector(u8 k): key(k) {}

		/// Returns true if the fixed index buffer should be used.
		__fi bool UseExpandIndexBuffer() const { return (expand == VSExpand::Point || expand == VSExpand::Sprite); }
	};
#pragma pack(pop)
#pragma pack(push, 4)
	struct PSSelector
	{
		// Performance note: there are too many shader combinations
		// It might hurt the performance due to frequent toggling worse it could consume
		// a lots of memory.
		union
		{
			struct
			{
				// Format
				u32 aem_fmt   : 2;
				u32 pal_fmt   : 2;
				u32 dst_fmt   : 2; // 0 → 32-bit, 1 → 24-bit, 2 → 16-bit
				u32 depth_fmt : 2; // 0 → None, 1 → 32-bit, 2 → 16-bit, 3 → RGBA
				// Alpha extension/Correction
				u32 aem : 1;
				u32 fba : 1;
				// Fog
				u32 fog : 1;
				// Flat/goround shading
				u32 iip : 1;
				// Pixel test
				u32 date : 3;
				u32 atst : 3;
				u32 afail : 2;
				// Color sampling
				u32 fst : 1; // Investigate to do it on the VS
				u32 tfx : 3;
				u32 tcc : 1;
				u32 wms : 2;
				u32 wmt : 2;
				u32 adjs : 1;
				u32 adjt : 1;
				u32 ltf : 1;
				// Shuffle and fbmask effect
				u32 shuffle  : 1;
				u32 shuffle_same : 1;
				u32 real16src: 1;
				u32 process_ba : 2;
				u32 process_rg : 2;
				u32 shuffle_across : 1;
				u32 write_rg : 1;
				u32 fbmask   : 1;

				// Blend and Colclip
				u32 blend_a        : 2;
				u32 blend_b        : 2;
				u32 blend_c        : 2;
				u32 blend_d        : 2;
				u32 fixed_one_a    : 1;
				u32 blend_hw       : 3;
				u32 a_masked       : 1;
				u32 hdr            : 1;
				u32 rta_correction : 1;
				u32 rta_source_correction : 1;
				u32 colclip        : 1;
				u32 blend_mix      : 2;
				u32 round_inv      : 1; // Blending will invert the value, so rounding needs to go the other way
				u32 pabe           : 1;
				u32 no_color       : 1; // disables color output entirely (depth only)
				u32 no_color1      : 1; // disables second color output (when unnecessary)

				// Others ways to fetch the texture
				u32 channel : 3;

				// Dithering
				u32 dither : 2;
				u32 dither_adjust : 1;

				// Depth clamp
				u32 zclamp : 1;

				// Hack
				u32 tcoffsethack : 1;
				u32 urban_chaos_hle : 1;
				u32 tales_of_abyss_hle : 1;
				u32 tex_is_fb : 1; // Jak Shadows
				u32 automatic_lod : 1;
				u32 manual_lod : 1;
				u32 point_sampler : 1;
				u32 region_rect : 1;

				// Scan mask
				u32 scanmsk : 2;
			};

			struct
			{
				u64 key_lo;
				u32 key_hi;
			};
		};
		__fi PSSelector() : key_lo(0), key_hi(0) {}

		__fi bool operator==(const PSSelector& rhs) const { return (key_lo == rhs.key_lo && key_hi == rhs.key_hi); }
		__fi bool operator!=(const PSSelector& rhs) const { return (key_lo != rhs.key_lo || key_hi != rhs.key_hi); }
		__fi bool operator<(const PSSelector& rhs) const { return (key_lo < rhs.key_lo || key_hi < rhs.key_hi); }

		__fi bool IsFeedbackLoop() const
		{
			const u32 sw_blend_bits = blend_a | blend_b | blend_d;
			const bool sw_blend_needs_rt = sw_blend_bits != 0 && ((sw_blend_bits | blend_c) & 1u);
			return tex_is_fb || fbmask || (date > 0 && date != 3) || sw_blend_needs_rt;
		}

		/// Disables color output from the pixel shader, this is done when all channels are masked.
		__fi void DisableColorOutput()
		{
			// remove software blending, since this will cause the color to be declared inout with fbfetch.
			blend_a = blend_b = blend_c = blend_d = 0;

			// TEX_IS_FB relies on us having a color output to begin with.
			tex_is_fb = 0;

			// no point having fbmask, since we're not writing. DATE has to stay.
			fbmask = 0;

			// disable both outputs.
			no_color = no_color1 = 1;
		}
	};
#pragma pack(pop)
	struct PSSelectorHash
	{
		std::size_t operator()(const PSSelector& p) const
		{
			std::size_t h = 0;
			HashCombine(h, p.key_lo, p.key_hi);
			return h;
		}
	};
#pragma pack(push, 1)
	struct SamplerSelector
	{
		union
		{
			struct
			{
				u8 tau      : 1;
				u8 tav      : 1;
				u8 biln     : 1;
				u8 triln    : 3;
				u8 aniso    : 1;
				u8 lodclamp : 1;
			};
			u8 key;
		};
		SamplerSelector(): key(0) {}
		SamplerSelector(u8 k): key(k) {}
		static SamplerSelector Point() { return SamplerSelector(); }
		static SamplerSelector Linear()
		{
			SamplerSelector out;
			out.biln = 1;
			return out;
		}

		/// Returns true if the effective minification filter is linear.
		__fi bool IsMinFilterLinear() const
		{
			// use the same filter as mag when mipmapping is off
			if (triln < static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Nearest))
				return biln;
			// Linear_Mipmap_Nearest or Linear_Mipmap_Linear
			return (triln >= static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Nearest));
		}

		/// Returns true if the effective magnification filter is linear.
		__fi bool IsMagFilterLinear() const
		{
			// magnification uses biln regardless of mip mode (they're only used for minification)
			return biln;
		}

		/// Returns true if the effective mipmap filter is linear.
		__fi bool IsMipFilterLinear() const
		{
			return (triln == static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Linear) ||
					triln == static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Linear));
		}

		/// Returns true if mipmaps should be used when filtering (i.e. LOD not clamped to zero).
		__fi bool UseMipmapFiltering() const
		{
			return (triln >= static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Nearest));
		}
	};
	struct DepthStencilSelector
	{
		union
		{
			struct
			{
				u8 ztst : 2;
				u8 zwe  : 1;
				u8 date : 1;
				u8 date_one : 1;

				u8 _free : 3;
			};
			u8 key;
		};
		DepthStencilSelector(): key(0) {}
		DepthStencilSelector(u8 k): key(k) {}
		static DepthStencilSelector NoDepth()
		{
			DepthStencilSelector out;
			out.ztst = ZTST_ALWAYS;
			return out;
		}
	};
	struct ColorMaskSelector
	{
		union
		{
			struct
			{
				u8 wr : 1;
				u8 wg : 1;
				u8 wb : 1;
				u8 wa : 1;

				u8 _free : 4;
			};
			struct
			{
				u8 wrgba : 4;
			};
			u8 key;
		};
		ColorMaskSelector(): key(0xF) {}
		ColorMaskSelector(u8 c): key(0) { wrgba = c; }
	};
#pragma pack(pop)
	struct alignas(16) VSConstantBuffer
	{
		GSVector2 vertex_scale;
		GSVector2 vertex_offset;
		GSVector2 texture_scale;
		GSVector2 texture_offset;
		GSVector2 point_size;
		GSVector2i max_depth;
		__fi VSConstantBuffer()
		{
			memset(this, 0, sizeof(*this));
		}
		__fi VSConstantBuffer(const VSConstantBuffer& other)
		{
			memcpy(this, &other, sizeof(*this));
		}
		__fi VSConstantBuffer& operator=(const VSConstantBuffer& other)
		{
			new (this) VSConstantBuffer(other);
			return *this;
		}
		__fi bool operator==(const VSConstantBuffer& other) const
		{
			return BitEqual(*this, other);
		}
		__fi bool operator!=(const VSConstantBuffer& other) const
		{
			return !(*this == other);
		}
		__fi bool Update(const VSConstantBuffer& other)
		{
			if (*this == other)
				return false;

			memcpy(this, &other, sizeof(*this));
			return true;
		}
	};
	struct alignas(16) PSConstantBuffer
	{
		GSVector4 FogColor_AREF;
		GSVector4 WH;
		GSVector4 TA_MaxDepth_Af;
		GSVector4i FbMask;

		GSVector4 HalfTexel;
		GSVector4 MinMax;
		GSVector4 LODParams;
		GSVector4 STRange;
		GSVector4i ChannelShuffle;
		GSVector2 TCOffsetHack;
		GSVector2 STScale;

		GSVector4 DitherMatrix[4];

		GSVector4 ScaleFactor;

		__fi PSConstantBuffer()
		{
			memset(this, 0, sizeof(*this));
		}
		__fi PSConstantBuffer(const PSConstantBuffer& other)
		{
			memcpy(this, &other, sizeof(*this));
		}
		__fi PSConstantBuffer& operator=(const PSConstantBuffer& other)
		{
			new (this) PSConstantBuffer(other);
			return *this;
		}
		__fi bool operator==(const PSConstantBuffer& other) const
		{
			return BitEqual(*this, other);
		}
		__fi bool operator!=(const PSConstantBuffer& other) const
		{
			return !(*this == other);
		}
		__fi bool Update(const PSConstantBuffer& other)
		{
			if (*this == other)
				return false;

			memcpy(this, &other, sizeof(*this));
			return true;
		}
	};
	struct BlendState
	{
		union
		{
			struct
			{
				u8 enable : 1;
				u8 constant_enable : 1;
				u8 op : 6;
				u8 src_factor : 4;
				u8 dst_factor : 4;
				u8 src_factor_alpha : 4;
				u8 dst_factor_alpha : 4;
				u8 constant;
			};
			u32 key;
		};
		BlendState(): key(0) {}
		BlendState(bool enable_, u8 src_factor_, u8 dst_factor_, u8 op_,
			u8 src_alpha_factor_, u8 dst_alpha_factor_, bool constant_enable_, u8 constant_)
			: key(0)
		{
			enable = enable_;
			constant_enable = constant_enable_;
			src_factor = src_factor_;
			dst_factor = dst_factor_;
			op = op_;
			src_factor_alpha = src_alpha_factor_;
			dst_factor_alpha = dst_alpha_factor_;
			constant = constant_;
		}

		// Blending has no effect if RGB is masked.
		bool IsEffective(ColorMaskSelector colormask) const;
	};
	enum class DestinationAlphaMode : u8
	{
		Off,            ///< No destination alpha test
		Stencil,        ///< Emulate using read-only stencil
		StencilOne,     ///< Emulate using read-write stencil (first write wins)
		PrimIDTracking, ///< Emulate by tracking the primitive ID of the last pixel allowed through
		Full,           ///< Full emulation (using barriers / ROV)
	};

	GSTexture* rt;        ///< Render target
	GSTexture* ds;        ///< Depth stencil
	GSTexture* tex;       ///< Source texture
	GSTexture* pal;       ///< Palette texture
	const GSVertex* verts;///< Vertices to draw
	const u16* indices;   ///< Indices to draw
	u32 nverts;           ///< Number of vertices
	u32 nindices;         ///< Number of indices
	u32 indices_per_prim; ///< Number of indices that make up one primitive
	const std::vector<size_t>* drawlist; ///< For reducing barriers on sprites
	GSVector4i scissor; ///< Scissor rect
	GSVector4i drawarea; ///< Area in the framebuffer which will be modified.
	Topology topology;  ///< Draw topology

	alignas(8) PSSelector ps;
	VSSelector vs;

	BlendState blend;
	SamplerSelector sampler;
	ColorMaskSelector colormask;
	DepthStencilSelector depth;

	bool require_one_barrier;  ///< Require texture barrier before draw (also used to requst an rt copy if texture barrier isn't supported)
	bool require_full_barrier; ///< Require texture barrier between all prims

	DestinationAlphaMode destination_alpha;
	SetDATM datm : 2;
	bool line_expand : 1;

	struct AlphaPass
	{
		alignas(8) PSSelector ps;
		bool enable;
		ColorMaskSelector colormask;
		DepthStencilSelector depth;
		float ps_aref;
	};

	AlphaPass alpha_second_pass;

	struct BlendPass
	{
		BlendState blend;
		u8 blend_hw;
		u8 dither;
		bool enable;
	};

	BlendPass blend_second_pass;

	VSConstantBuffer cb_vs;
	PSConstantBuffer cb_ps;
};

class GSDevice : public GSAlignedClass<32>
{
public:
	enum class PresentResult
	{
		OK,
		FrameSkipped,
		DeviceLost
	};

	enum class DebugMessageCategory
	{
		Cache,
		Reg,
		Debug,
		Message,
		Performance
	};

	// clang-format off
	struct FeatureSupport
	{
		bool broken_point_sampler : 1; ///< Issue with AMD cards, see tfx shader for details
		bool vs_expand            : 1; ///< Supports expanding points/lines/sprites in the vertex shader
		bool primitive_id         : 1; ///< Supports primitive ID for use with prim tracking destination alpha algorithm
		bool texture_barrier      : 1; ///< Supports sampling rt and hopefully texture barrier
		bool provoking_vertex_last: 1; ///< Supports using the last vertex in a primitive as the value for flat shading.
		bool point_expand         : 1; ///< Supports point expansion in hardware.
		bool line_expand          : 1; ///< Supports line expansion in hardware.
		bool prefer_new_textures  : 1; ///< Allocate textures up to the pool size before reusing them, to avoid render pass restarts.
		bool dxt_textures         : 1; ///< Supports DXTn texture compression, i.e. S3TC and BC1-3.
		bool bptc_textures        : 1; ///< Supports BC6/7 texture compression.
		bool framebuffer_fetch    : 1; ///< Can sample from the framebuffer without texture barriers.
		bool clip_control         : 1; ///< Can use 0..1 depth range instead of -1..1.
		bool stencil_buffer       : 1; ///< Supports stencil buffer, and can use for DATE.
		bool test_and_sample_depth: 1; ///< Supports concurrently binding the depth-stencil buffer for sampling and depth testing.
		FeatureSupport()
		{
			memset(this, 0, sizeof(*this));
		}
	};

	struct MultiStretchRect
	{
		GSVector4 src_rect;
		GSVector4 dst_rect;
		GSTexture* src;
		bool linear;
		GSHWDrawConfig::ColorMaskSelector wmask; // 0xf for all channels by default
	};

	struct TextureRecycleDeleter
	{
		void operator()(GSTexture* const tex);
	};
	using RecycledTexture = std::unique_ptr<GSTexture, TextureRecycleDeleter>;

	enum BlendFactor : u8
	{
		// HW blend factors
		SRC_COLOR,   INV_SRC_COLOR,   DST_COLOR,  INV_DST_COLOR,
		SRC1_COLOR,  INV_SRC1_COLOR,  SRC_ALPHA,  INV_SRC_ALPHA,
		DST_ALPHA,   INV_DST_ALPHA,   SRC1_ALPHA, INV_SRC1_ALPHA,
		CONST_COLOR, INV_CONST_COLOR, CONST_ONE,  CONST_ZERO,
	};
	enum BlendOp : u8
	{
		// HW blend operations
		OP_ADD, OP_SUBTRACT, OP_REV_SUBTRACT
	};
	// clang-format on

private:
	std::array<FastList<GSTexture*>, 2> m_pool; // [texture, target]

	static const HWBlend m_blendMap[81];

protected:
	static constexpr int NUM_INTERLACE_SHADERS = 5;
	static constexpr float MAD_SENSITIVITY = 0.08f;
	static constexpr u32 MAX_POOLED_TARGETS = 300;
	static constexpr u32 MAX_TARGET_AGE = 20;
	static constexpr u32 MAX_POOLED_TEXTURES = 300;
	static constexpr u32 MAX_TEXTURE_AGE = 10;
	static constexpr u32 EXPAND_BUFFER_SIZE = sizeof(u16) * 16383 * 6;

	WindowInfo m_window_info;

	GSTexture* m_merge = nullptr;
	GSTexture* m_weavebob = nullptr;
	GSTexture* m_blend = nullptr;
	GSTexture* m_mad = nullptr;
	GSTexture* m_target_tmp = nullptr;
	GSTexture* m_current = nullptr;

	struct
	{
		u32 start, count;
	} m_vertex = {};
	struct
	{
		u32 start, count;
	} m_index = {};
	unsigned int m_frame = 0; // for ageing the pool
	bool m_rbswapped = false;
	FeatureSupport m_features;

	void AcquireWindow();

	virtual GSTexture* CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format) = 0;
	GSTexture* FetchSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format, bool clear, bool prefer_reuse);

	virtual void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear) = 0;
	virtual void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb) = 0;

public:
	GSDevice();
	virtual ~GSDevice();

	/// Generates a fixed index buffer for expanding points and sprites. Buffer is assumed to be at least EXPAND_BUFFER_SIZE in size.
	static void GenerateExpansionIndexBuffer(void* buffer);

	/// Returns the maximum number of mipmap levels for a given texture size.
	static int GetMipmapLevelsForSize(int width, int height);

	__fi FeatureSupport Features() const { return m_features; }

	__fi s32 GetWindowWidth() const { return static_cast<s32>(m_window_info.surface_width); }
	__fi s32 GetWindowHeight() const { return static_cast<s32>(m_window_info.surface_height); }
	__fi GSVector2i GetWindowSize() const { return GSVector2i(static_cast<s32>(m_window_info.surface_width), static_cast<s32>(m_window_info.surface_height)); }
	__fi GSTexture* GetCurrent() const { return m_current; }

	void Recycle(GSTexture* t);

	virtual bool Create();
	virtual void Destroy();

	virtual void ResetAPIState();
	virtual void RestoreAPIState();

	/// Returns the graphics API used by this device.
	virtual RenderAPI GetRenderAPI() const = 0;

	/// Returns false if the window was completely occluded. If frame_skip is set, the frame won't be
	/// displayed, but the GPU command queue will still be flushed.
	virtual PresentResult BeginPresent(bool frame_skip) = 0;

	/// Presents the frame to the display.
	virtual void EndPresent() = 0;

	void ClearRenderTarget(GSTexture* t, u32 c);
	void ClearDepth(GSTexture* t, float d);
	void InvalidateRenderTarget(GSTexture* t);

	GSTexture* CreateRenderTarget(int w, int h, GSTexture::Format format, bool clear = true);
	GSTexture* CreateDepthStencil(int w, int h, GSTexture::Format format, bool clear = true);
	GSTexture* CreateTexture(int w, int h, int mipmap_levels, GSTexture::Format format, bool prefer_reuse = false);

	virtual std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) = 0;

	virtual void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) = 0;
	virtual void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader = ShaderConvert::COPY, bool linear = true) = 0;
	virtual void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha, ShaderConvert shader = ShaderConvert::COPY) = 0;

	void StretchRect(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader = ShaderConvert::COPY, bool linear = true);

	/// Performs a screen blit for display. If dTex is null, it assumes you are writing to the system framebuffer/swap chain.
	virtual void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect) = 0;

	/// Same as doing StretchRect for each item, except tries to batch together rectangles in as few draws as possible.
	/// The provided list should be sorted by texture, the implementations only check if it's the same as the last.
	virtual void DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader = ShaderConvert::COPY);

	/// Sorts a MultiStretchRect list for optimal batching.
	static void SortMultiStretchRects(MultiStretchRect* rects, u32 num_rects);

	/// Updates a GPU CLUT texture from a source texture.
	virtual void UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize) = 0;

	/// Converts a colour format to an indexed format texture.
	virtual void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM) = 0;

	/// Uses box downsampling to resize a texture.
	virtual void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect) = 0;

	virtual void RenderHW(GSHWDrawConfig& config) = 0;

	virtual void ClearSamplerCache() = 0;

	void ClearCurrent();
	void Merge(GSTexture* sTex[3], GSVector4* sRect, GSVector4* dRect, const GSVector2i& fs, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c);
	void Interlace(const GSVector2i& ds, int field, int mode, float yoffset);

	bool ResizeRenderTarget(GSTexture** t, int w, int h, bool preserve_contents, bool recycle);

	bool IsRBSwapped() { return m_rbswapped; }

	void AgePool();
	void PurgePool();

	__fi static constexpr bool IsDualSourceBlendFactor(u8 factor)
	{
		return (factor == SRC1_ALPHA || factor == INV_SRC1_ALPHA || factor == SRC1_COLOR || factor == INV_SRC1_COLOR);
	}
	__fi static constexpr bool IsConstantBlendFactor(u16 factor)
	{
		return (factor == CONST_COLOR || factor == INV_CONST_COLOR);
	}

	// Convert the GS blend equations to HW blend factors/ops
	// Index is computed as ((((A * 3 + B) * 3) + C) * 3) + D. A, B, C, D taken from ALPHA register.
	__ri static HWBlend GetBlend(u32 index) { return m_blendMap[index]; }
	__ri static u16 GetBlendFlags(u32 index) { return m_blendMap[index].flags; }
};

template <>
struct std::hash<GSHWDrawConfig::PSSelector> : public GSHWDrawConfig::PSSelectorHash {};

extern std::unique_ptr<GSDevice> g_gs_device;
