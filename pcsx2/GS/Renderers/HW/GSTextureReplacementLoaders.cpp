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

#include <cstring> /* memset/memcpy */
#include <csetjmp>

#include <png.h>

#include <file/file_path.h>
#include <string/stdstring.h>

#include <functional> /* std::function */

#include "common/Align.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "GSTextureReplacements.h"

struct LoaderDefinition
{
	const char* extension;
	GSTextureReplacements::ReplacementTextureLoader loader;
};

static bool PNGLoader(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image);
static bool DDSLoader(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image);

static constexpr LoaderDefinition s_loaders[] = {
	{"png", PNGLoader},
	{"dds", DDSLoader},
};


GSTextureReplacements::ReplacementTextureLoader GSTextureReplacements::GetLoader(const char *filename)
{
	const char *extension = path_get_extension(filename);
	if (string_is_empty(extension))
		return nullptr;

	for (int i = 0; i < 2; i++)
	{
		if (string_is_equal(extension, s_loaders[i].extension))
			return s_loaders[i].loader;
	}
	return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper routines
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static u32 GetBlockCount(u32 extent, u32 block_size)
{
	return std::max(Common::AlignUp(extent, block_size) / block_size, 1u);
}

static void CalcBlockMipmapSize(u32 block_size, u32 bytes_per_block, u32 base_width, u32 base_height, u32 mip, u32& width, u32& height, u32& pitch, u32& size)
{
	width = std::max<u32>(base_width >> mip, 1u);
	height = std::max<u32>(base_height >> mip, 1u);

	const u32 blocks_wide = GetBlockCount(width, block_size);
	const u32 blocks_high = GetBlockCount(height, block_size);

	// Pitch can't be specified with each mip level, so we have to calculate it ourselves.
	pitch = blocks_wide * bytes_per_block;
	size = blocks_high * pitch;
}

static void ConvertTexture_X8B8G8R8(u32 width, u32 height, std::vector<u8>& data, u32& pitch)
{
	for (u32 row = 0; row < height; row++)
	{
		u8* data_ptr = data.data() + row * pitch;

		for (u32 x = 0; x < width; x++)
		{
			// Set alpha channel to full intensity.
			data_ptr[3] = 0x80;
			data_ptr += sizeof(u32);
		}
	}
}

static void ConvertTexture_A8R8G8B8(u32 width, u32 height, std::vector<u8>& data, u32& pitch)
{
	for (u32 row = 0; row < height; row++)
	{
		u8* data_ptr = data.data() + row * pitch;

		for (u32 x = 0; x < width; x++)
		{
			// Byte swap ABGR -> RGBA
			u32 val;
			memcpy(&val, data_ptr, sizeof(val));
			val = ((val & 0xFF00FF00) | ((val >> 16) & 0xFF) | ((val << 16) & 0xFF0000));
			memcpy(data_ptr, &val, sizeof(u32));
			data_ptr += sizeof(u32);
		}
	}
}

static void ConvertTexture_X8R8G8B8(u32 width, u32 height, std::vector<u8>& data, u32& pitch)
{
	for (u32 row = 0; row < height; row++)
	{
		u8* data_ptr = data.data() + row * pitch;

		for (u32 x = 0; x < width; x++)
		{
			// Byte swap XBGR -> RGBX, and set alpha to full intensity.
			u32 val;
			memcpy(&val, data_ptr, sizeof(val));
			val = ((val & 0x0000FF00) | ((val >> 16) & 0xFF) | ((val << 16) & 0xFF0000)) | 0xFF000000;
			memcpy(data_ptr, &val, sizeof(u32));
			data_ptr += sizeof(u32);
		}
	}
}

static void ConvertTexture_R8G8B8(u32 width, u32 height, std::vector<u8>& data, u32& pitch)
{
	const u32 new_pitch = width * sizeof(u32);
	std::vector<u8> new_data(new_pitch * height);

	for (u32 row = 0; row < height; row++)
	{
		const u8* rgb_data_ptr = data.data() + row * pitch;
		u8* data_ptr = new_data.data() + row * new_pitch;

		for (u32 x = 0; x < width; x++)
		{
			// This is BGR in memory.
			u32 val;
			memcpy(&val, rgb_data_ptr, sizeof(val));
			val = ((val & 0x0000FF00) | ((val >> 16) & 0xFF) | ((val << 16) & 0xFF0000)) | 0xFF000000;
			memcpy(data_ptr, &val, sizeof(u32));
			data_ptr += sizeof(u32);
			rgb_data_ptr += 3;
		}
	}

	data = std::move(new_data);
	pitch = new_pitch;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// PNG Handlers
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///
static void png_rfile_read_data(png_structp png_ptr, png_bytep data, png_size_t length)
{
   RFILE *fp           = (RFILE*)(png_get_io_ptr(png_ptr));
   /* fread() returns 0 on error, so it is OK to store this in a png_size_t
    * instead of an int, which is what fread() actually returns.
    */
   rfread(data, 1, length, fp);
}

bool PNGLoader(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image)
{
	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png_ptr)
		return false;

	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
	{
		png_destroy_read_struct(&png_ptr, nullptr, nullptr);
		return false;
	}

	RFILE *fp = FileSystem::OpenFile(filename.c_str(), "rb");
	if (!fp)
	{
		png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
		return false;
	}

	if (setjmp(png_jmpbuf(png_ptr)))
	{
		png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
		rfclose(fp);
		return false;
	}

	png_set_read_fn(png_ptr, fp, png_rfile_read_data);
	png_read_info(png_ptr, info_ptr);

	png_uint_32 width = 0;
	png_uint_32 height = 0;
	int bitDepth = 0;
	int colorType = -1;
	if (png_get_IHDR(png_ptr, info_ptr, &width, &height, &bitDepth, &colorType, nullptr, nullptr, nullptr) != 1 ||
		width == 0 || height == 0)
	{
		png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
		rfclose(fp);
		return false;
	}

	const u32 pitch = width * sizeof(u32);
	tex->width = width;
	tex->height = height;
	tex->format = GSTexture::Format::Color;
	tex->pitch = pitch;
	tex->data.resize(pitch * height);

	const png_uint_32 row_bytes = png_get_rowbytes(png_ptr, info_ptr);
	std::vector<u8> row_data(row_bytes);

	for (u32 y = 0; y < height; y++)
	{
		png_read_row(png_ptr, static_cast<png_bytep>(row_data.data()), nullptr);

		const u8* row_ptr = row_data.data();
		u8* out_ptr = tex->data.data() + y * pitch;
		if (colorType == PNG_COLOR_TYPE_RGB)
		{
			for (u32 x = 0; x < width; x++)
			{
				u32 pixel = static_cast<u32>(*(row_ptr)++);
				pixel |= static_cast<u32>(*(row_ptr)++) << 8;
				pixel |= static_cast<u32>(*(row_ptr)++) << 16;
				pixel |= 0x80000000u; // make opaque
				memcpy(out_ptr, &pixel, sizeof(pixel));
				out_ptr += sizeof(pixel);
			}
		}
		else if (colorType == PNG_COLOR_TYPE_RGBA)
			memcpy(out_ptr, row_ptr, pitch);
	}

	png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
	rfclose(fp);
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// DDS Handler
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// From https://raw.githubusercontent.com/Microsoft/DirectXTex/master/DirectXTex/DDS.h
//
// This header defines constants and structures that are useful when parsing
// DDS files.  DDS files were originally designed to use several structures
// and constants that are native to DirectDraw and are defined in ddraw.h,
// such as DDSURFACEDESC2 and DDSCAPS2.  This file defines similar
// (compatible) constants and structures so that one can use DDS files
// without needing to include ddraw.h.
//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
// http://go.microsoft.com/fwlink/?LinkId=248926

#pragma pack(push, 1)

static constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "

struct DDS_PIXELFORMAT
{
	uint32_t dwSize;
	uint32_t dwFlags;
	uint32_t dwFourCC;
	uint32_t dwRGBBitCount;
	uint32_t dwRBitMask;
	uint32_t dwGBitMask;
	uint32_t dwBBitMask;
	uint32_t dwABitMask;
};

#define DDS_FOURCC 0x00000004 // DDPF_FOURCC
#define DDS_RGB 0x00000040 // DDPF_RGB
#define DDS_RGBA 0x00000041 // DDPF_RGB | DDPF_ALPHAPIXELS
#define DDS_LUMINANCE 0x00020000 // DDPF_LUMINANCE
#define DDS_LUMINANCEA 0x00020001 // DDPF_LUMINANCE | DDPF_ALPHAPIXELS
#define DDS_ALPHA 0x00000002 // DDPF_ALPHA
#define DDS_PAL8 0x00000020 // DDPF_PALETTEINDEXED8
#define DDS_PAL8A 0x00000021 // DDPF_PALETTEINDEXED8 | DDPF_ALPHAPIXELS
#define DDS_BUMPDUDV 0x00080000 // DDPF_BUMPDUDV

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
	((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | ((uint32_t)(uint8_t)(ch2) << 16) | \
		((uint32_t)(uint8_t)(ch3) << 24))
#endif /* defined(MAKEFOURCC) */

#define DDS_HEADER_FLAGS_TEXTURE \
	0x00001007 // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
#define DDS_HEADER_FLAGS_MIPMAP 0x00020000 // DDSD_MIPMAPCOUNT
#define DDS_HEADER_FLAGS_VOLUME 0x00800000 // DDSD_DEPTH
#define DDS_HEADER_FLAGS_PITCH 0x00000008 // DDSD_PITCH
#define DDS_HEADER_FLAGS_LINEARSIZE 0x00080000 // DDSD_LINEARSIZE
#define DDS_MAX_TEXTURE_SIZE 32768

// Subset here matches D3D10_RESOURCE_DIMENSION and D3D11_RESOURCE_DIMENSION
enum DDS_RESOURCE_DIMENSION
{
	DDS_DIMENSION_TEXTURE1D = 2,
	DDS_DIMENSION_TEXTURE2D = 3,
	DDS_DIMENSION_TEXTURE3D = 4,
};

struct DDS_HEADER
{
	uint32_t dwSize;
	uint32_t dwFlags;
	uint32_t dwHeight;
	uint32_t dwWidth;
	uint32_t dwPitchOrLinearSize;
	uint32_t dwDepth; // only if DDS_HEADER_FLAGS_VOLUME is set in dwFlags
	uint32_t dwMipMapCount;
	uint32_t dwReserved1[11];
	DDS_PIXELFORMAT ddspf;
	uint32_t dwCaps;
	uint32_t dwCaps2;
	uint32_t dwCaps3;
	uint32_t dwCaps4;
	uint32_t dwReserved2;
};

struct DDS_HEADER_DXT10
{
	uint32_t dxgiFormat;
	uint32_t resourceDimension;
	uint32_t miscFlag; // see DDS_RESOURCE_MISC_FLAG
	uint32_t arraySize;
	uint32_t miscFlags2; // see DDS_MISC_FLAGS2
};

#pragma pack(pop)

constexpr DDS_PIXELFORMAT DDSPF_A8R8G8B8 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGBA, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000};
constexpr DDS_PIXELFORMAT DDSPF_X8R8G8B8 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000};
constexpr DDS_PIXELFORMAT DDSPF_A8B8G8R8 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGBA, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000};
constexpr DDS_PIXELFORMAT DDSPF_X8B8G8R8 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0x00000000};
constexpr DDS_PIXELFORMAT DDSPF_R8G8B8 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 24, 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000};

// End of Microsoft code from DDS.h.

static bool DDSPixelFormatMatches(const DDS_PIXELFORMAT& pf1, const DDS_PIXELFORMAT& pf2)
{
	return std::tie(pf1.dwSize, pf1.dwFlags, pf1.dwFourCC, pf1.dwRGBBitCount, pf1.dwRBitMask,
			   pf1.dwGBitMask, pf1.dwGBitMask, pf1.dwBBitMask, pf1.dwABitMask) ==
		   std::tie(pf2.dwSize, pf2.dwFlags, pf2.dwFourCC, pf2.dwRGBBitCount, pf2.dwRBitMask,
			   pf2.dwGBitMask, pf2.dwGBitMask, pf2.dwBBitMask, pf2.dwABitMask);
}

struct DDSLoadInfo
{
	u32 block_size = 1;
	u32 bytes_per_block = 4;
	u32 width = 0;
	u32 height = 0;
	u32 mip_count = 0;
	GSTexture::Format format = GSTexture::Format::Color;
	s64 base_image_offset = 0;
	u32 base_image_size = 0;
	u32 base_image_pitch = 0;

	std::function<void(u32 width, u32 height, std::vector<u8>& data, u32& pitch)> conversion_function;
};

static bool ParseDDSHeader(RFILE* fp, DDSLoadInfo* info)
{
	u32 magic;
	if (rfread(&magic, sizeof(magic), 1, fp) != 1 || magic != DDS_MAGIC)
		return false;

	DDS_HEADER header;
	u32 header_size = sizeof(header);
	if (rfread(&header, header_size, 1, fp) != 1 || header.dwSize < header_size)
		return false;

	// We should check for DDS_HEADER_FLAGS_TEXTURE here, but some tools don't seem
	// to set it (e.g. compressonator). But we can still validate the size.
	if (header.dwWidth == 0 || header.dwWidth >= DDS_MAX_TEXTURE_SIZE ||
		header.dwHeight == 0 || header.dwHeight >= DDS_MAX_TEXTURE_SIZE)
		return false;

	// Image should be 2D.
	if (header.dwFlags & DDS_HEADER_FLAGS_VOLUME)
		return false;

	// Presence of width/height fields is already tested by DDS_HEADER_FLAGS_TEXTURE.
	info->width = header.dwWidth;
	info->height = header.dwHeight;
	if (info->width == 0 || info->height == 0)
		return false;

	// Check for mip levels.
	if (header.dwFlags & DDS_HEADER_FLAGS_MIPMAP)
	{
		info->mip_count = header.dwMipMapCount;
		if (header.dwMipMapCount != 0)
			info->mip_count = header.dwMipMapCount;
		else
			info->mip_count = GSTextureReplacements::CalcMipmapLevelsForReplacement(info->width, info->height);
	}
	else
	{
		info->mip_count = 1;
	}

	// Handle fourcc formats vs uncompressed formats.
	const bool has_fourcc = (header.ddspf.dwFlags & DDS_FOURCC) != 0;
	if (has_fourcc)
	{
		// Handle DX10 extension header.
		u32 dxt10_format = 0;
		if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', '1', '0'))
		{
			DDS_HEADER_DXT10 dxt10_header;
			if (rfread(&dxt10_header, sizeof(dxt10_header), 1, fp) != 1)
				return false;

			// Can't handle array textures here. Doesn't make sense to use them, anyway.
			if (dxt10_header.resourceDimension != DDS_DIMENSION_TEXTURE2D || dxt10_header.arraySize != 1)
				return false;

			header_size += sizeof(dxt10_header);
			dxt10_format = dxt10_header.dxgiFormat;
		}

		const GSDevice::FeatureSupport features(g_gs_device->Features());
		if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '1') || dxt10_format == 71)
		{
			info->format = GSTexture::Format::BC1;
			info->block_size = 4;
			info->bytes_per_block = 8;
			if (!features.dxt_textures)
				return false;
		}
		else if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '2') || header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '3') || dxt10_format == 74)
		{
			info->format = GSTexture::Format::BC2;
			info->block_size = 4;
			info->bytes_per_block = 16;
			if (!features.dxt_textures)
				return false;
		}
		else if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '4') || header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '5') || dxt10_format == 77)
		{
			info->format = GSTexture::Format::BC3;
			info->block_size = 4;
			info->bytes_per_block = 16;
			if (!features.dxt_textures)
				return false;
		}
		else if (dxt10_format == 98)
		{
			info->format = GSTexture::Format::BC7;
			info->block_size = 4;
			info->bytes_per_block = 16;
			if (!features.bptc_textures)
				return false;
		}
		else
		{
			// Leave all remaining formats to SOIL.
			return false;
		}
	}
	else
	{
		if (DDSPixelFormatMatches(header.ddspf, DDSPF_A8R8G8B8))
			info->conversion_function = ConvertTexture_A8R8G8B8;
		else if (DDSPixelFormatMatches(header.ddspf, DDSPF_X8R8G8B8))
			info->conversion_function = ConvertTexture_X8R8G8B8;
		else if (DDSPixelFormatMatches(header.ddspf, DDSPF_X8B8G8R8))
			info->conversion_function = ConvertTexture_X8B8G8R8;
		else if (DDSPixelFormatMatches(header.ddspf, DDSPF_R8G8B8))
			info->conversion_function = ConvertTexture_R8G8B8;
		else if (DDSPixelFormatMatches(header.ddspf, DDSPF_A8B8G8R8))
		{
			// This format is already in RGBA order, so no conversion necessary.
		}
		else
			return false;

		// All these formats are RGBA, just with byte swapping.
		info->format = GSTexture::Format::Color;
		info->block_size = 1;
		info->bytes_per_block = header.ddspf.dwRGBBitCount / 8;
	}

	// Mip levels smaller than the block size are padded to multiples of the block size.
	const u32 blocks_wide = GetBlockCount(info->width, info->block_size);
	const u32 blocks_high = GetBlockCount(info->height, info->block_size);

	// Pitch can be specified in the header, otherwise we can derive it from the dimensions. For
	// compressed formats, both DDS_HEADER_FLAGS_LINEARSIZE and DDS_HEADER_FLAGS_PITCH should be
	// set. See https://msdn.microsoft.com/en-us/library/windows/desktop/bb943982(v=vs.85).aspx
	if (header.dwFlags & DDS_HEADER_FLAGS_PITCH && header.dwFlags & DDS_HEADER_FLAGS_LINEARSIZE)
	{
		// Convert pitch (in bytes) to texels/row length.
		if (header.dwPitchOrLinearSize < info->bytes_per_block)
		{
			// Likely a corrupted or invalid file.
			return false;
		}

		info->base_image_pitch = header.dwPitchOrLinearSize;
		info->base_image_size = info->base_image_pitch * blocks_high;
	}
	else
	{
		// Assume no padding between rows of blocks.
		info->base_image_pitch = blocks_wide * info->bytes_per_block;
		info->base_image_size = info->base_image_pitch * blocks_high;
	}

	// Check for truncated or corrupted files.
	info->base_image_offset = sizeof(magic) + header_size;
	if (info->base_image_offset >= FileSystem::FSize64(fp))
		return false;

	return true;
}

static bool ReadDDSMipLevel(RFILE* fp, const std::string& filename, u32 mip_level, const DDSLoadInfo& info, u32 width, u32 height, std::vector<u8>& data, u32& pitch, u32 size)
{
	// D3D11 cannot handle block compressed textures where the first mip level is
	// not a multiple of the block size.
	if (mip_level == 0 && info.block_size > 1 &&
		((width % info.block_size) != 0 || (height % info.block_size) != 0))
	{
		Console.Error(
			"Invalid dimensions for DDS texture %s. For compressed textures of this format, "
			"the width/height of the first mip level must be a multiple of %u.",
			filename.c_str(), info.block_size);
		return false;
	}

	data.resize(size);
	if (rfread(data.data(), size, 1, fp) != 1)
		return false;

	// Apply conversion function for uncompressed textures.
	if (info.conversion_function)
		info.conversion_function(width, height, data, pitch);

	return true;
}

bool DDSLoader(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image)
{
	RFILE *fp = FileSystem::OpenFile(filename.c_str(), "rb");
	if (!fp)
		return false;

	DDSLoadInfo info;
	if (!ParseDDSHeader(fp, &info))
	{
		filestream_close(fp);
		return false;
	}

	// always load the base image
	if (FileSystem::FSeek64(fp, info.base_image_offset, SEEK_SET) != 0)
	{
		filestream_close(fp);
		return false;
	}

	tex->format = info.format;
	tex->width = info.width;
	tex->height = info.height;
	tex->pitch = info.base_image_pitch;
	if (!ReadDDSMipLevel(fp, filename, 0, info, tex->width, tex->height, tex->data, tex->pitch, info.base_image_size))
	{
		filestream_close(fp);
		return false;
	}

	// Read in any remaining mip levels in the file.
	if (!only_base_image)
	{
		for (u32 level = 1; level <= info.mip_count; level++)
		{
			GSTextureReplacements::ReplacementTexture::MipData md;
			u32 mip_size;
			CalcBlockMipmapSize(info.block_size, info.bytes_per_block, info.width, info.height, level, md.width, md.height, md.pitch, mip_size);
			if (!ReadDDSMipLevel(fp, filename, level, info, md.width, md.height, md.data, md.pitch, mip_size))
				break;

			tex->mips.push_back(std::move(md));
		}
	}

	filestream_close(fp);
	return true;
}
