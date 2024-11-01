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

#include <cstring>

#include <file/file_path.h>

#include "VKShaderCache.h"
#include "GSDeviceVK.h"
#include "GS/GS.h"

#include "Config.h"
#include "ShaderCacheVersion.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/MD5Digest.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <cstring>
#include <memory>

// glslang includes
#include "SPIRV/GlslangToSpv.h"
#include "StandAlone/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"

std::unique_ptr<VKShaderCache> g_vulkan_shader_cache;

// Registers itself for cleanup via atexit
static bool glslang_initialized = false;

static bool InitializeGlslang(void)
{
	if (glslang_initialized)
		return true;

	if (!glslang::InitializeProcess())
		return false;

	std::atexit(DeinitializeGlslang);
	glslang_initialized = true;
	return true;
}

void DeinitializeGlslang(void)
{
	if (!glslang_initialized)
		return;

	glslang::FinalizeProcess();
	glslang_initialized = false;
}

static std::optional<SPIRVCodeVector> CompileShaderToSPV(
		EShLanguage stage, const char* stage_filename, std::string_view source, bool debug)
{
	std::unique_ptr<glslang::TProgram> program;
	glslang::TShader::ForbidIncluder includer;
	std::string full_source_code;
	if (!InitializeGlslang())
		return std::nullopt;

	std::unique_ptr<glslang::TShader> shader = std::make_unique<glslang::TShader>(stage);
	const EProfile profile       = ECoreProfile;
	const EShMessages messages   = static_cast<EShMessages>(EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules | (debug ? EShMsgDebugInfo : 0));
	const int default_version    = 450;

	const char* pass_source_code = source.data();
	int pass_source_code_length  = static_cast<int>(source.size());
	shader->setStringsWithLengths(&pass_source_code, &pass_source_code_length, 1);

	if (!shader->parse(&glslang::DefaultTBuiltInResource,
		default_version, profile, false, true, messages, includer))
	{
		Console.Error("Failed to parse shader");
		return std::nullopt;
	}

	// Even though there's only a single shader, we still need to link it to generate SPV
	program = std::make_unique<glslang::TProgram>();
	program->addShader(shader.get());
	if (!program->link(messages))
	{
		Console.Error("Failed to link program");
		return std::nullopt;
	}

	glslang::TIntermediate* intermediate = program->getIntermediate(stage);
	if (!intermediate)
	{
		Console.Error("Failed to generate SPIR-V");
		return std::nullopt;
	}

	SPIRVCodeVector out_code;
	spv::SpvBuildLogger logger;
	glslang::SpvOptions options;
	options.generateDebugInfo = debug;
	glslang::GlslangToSpv(*intermediate, out_code, &logger, &options);

	// Write out messages
	const char *info_log_msg     = shader->getInfoLog();
	const char *info_dbg_log_msg = shader->getInfoDebugLog();
	size_t info_log_size         = std::strlen(info_log_msg);
	size_t info_dbg_log_size     = std::strlen(info_dbg_log_msg);
	if (info_log_size > 0)
		Console.Warning("Shader info log: %s", info_log_msg);
	if (info_dbg_log_size > 0)
		Console.Warning("Shader debug info log: %s", info_dbg_log_msg);
	if (info_log_size > 0)
		Console.Warning("Program info log: %s", info_log_msg);
	if (info_dbg_log_size > 0)
		Console.Warning("Program debug info log: %s", info_dbg_log_msg);
	std::string spv_messages = logger.getAllMessages();
	if (!spv_messages.empty())
		Console.Warning("SPIR-V conversion messages: %s", spv_messages.c_str());

	return out_code;
}

std::optional<SPIRVCodeVector> CompileShader(ShaderType type, std::string_view source_code, bool debug)
{
	switch (type)
	{
		case ShaderType::Vertex:
			return CompileShaderToSPV(EShLangVertex, "vs", source_code, debug);

		case ShaderType::Fragment:
			return CompileShaderToSPV(EShLangFragment, "ps", source_code, debug);

		case ShaderType::Compute:
			return CompileShaderToSPV(EShLangCompute, "cs", source_code, debug);

		default:
			break;
	}
	return std::nullopt;
}

#pragma pack(push, 4)
struct VK_PIPELINE_CACHE_HEADER
{
	u32 header_length;
	u32 header_version;
	u32 vendor_id;
	u32 device_id;
	u8 uuid[VK_UUID_SIZE];
};

struct CacheIndexEntry
{
	u64 source_hash_low;
	u64 source_hash_high;
	u32 source_length;
	u32 shader_type;
	u32 file_offset;
	u32 blob_size;
};
#pragma pack(pop)

static bool ValidatePipelineCacheHeader(const VK_PIPELINE_CACHE_HEADER& header)
{
	if (header.header_length < sizeof(VK_PIPELINE_CACHE_HEADER))
	{
		Console.Error("Pipeline cache failed validation: Invalid header length");
		return false;
	}

	if (header.header_version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
	{
		Console.Error("Pipeline cache failed validation: Invalid header version");
		return false;
	}

	if (header.vendor_id != GSDeviceVK::GetInstance()->GetDeviceProperties().vendorID)
	{
		Console.Error("Pipeline cache failed validation: Incorrect vendor ID (file: 0x%X, device: 0x%X)",
				header.vendor_id, GSDeviceVK::GetInstance()->GetDeviceProperties().vendorID);
		return false;
	}

	if (header.device_id != GSDeviceVK::GetInstance()->GetDeviceProperties().deviceID)
	{
		Console.Error("Pipeline cache failed validation: Incorrect device ID (file: 0x%X, device: 0x%X)",
				header.device_id, GSDeviceVK::GetInstance()->GetDeviceProperties().deviceID);
		return false;
	}

	if (memcmp(header.uuid, GSDeviceVK::GetInstance()->GetDeviceProperties().pipelineCacheUUID, VK_UUID_SIZE) != 0)
	{
		Console.Error("Pipeline cache failed validation: Incorrect UUID");
		return false;
	}

	return true;
}

static void FillPipelineCacheHeader(VK_PIPELINE_CACHE_HEADER* header)
{
	header->header_length  = sizeof(VK_PIPELINE_CACHE_HEADER);
	header->header_version = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
	header->vendor_id      = GSDeviceVK::GetInstance()->GetDeviceProperties().vendorID;
	header->device_id      = GSDeviceVK::GetInstance()->GetDeviceProperties().deviceID;
	memcpy(header->uuid, GSDeviceVK::GetInstance()->GetDeviceProperties().pipelineCacheUUID, VK_UUID_SIZE);
}

VKShaderCache::VKShaderCache() = default;

VKShaderCache::~VKShaderCache()
{
	CloseShaderCache();
	FlushPipelineCache();
	ClosePipelineCache();
}

bool VKShaderCache::CacheIndexKey::operator==(const CacheIndexKey& key) const
{
	return (source_hash_low == key.source_hash_low && source_hash_high == key.source_hash_high &&
			source_length == key.source_length && shader_type == key.shader_type);
}

bool VKShaderCache::CacheIndexKey::operator!=(const CacheIndexKey& key) const
{
	return (source_hash_low != key.source_hash_low || source_hash_high != key.source_hash_high ||
			source_length != key.source_length || shader_type != key.shader_type);
}

void VKShaderCache::Create(std::string_view base_path, u32 version, bool debug)
{
	g_vulkan_shader_cache.reset(new VKShaderCache());
	g_vulkan_shader_cache->Open();
}

void VKShaderCache::Destroy() { g_vulkan_shader_cache.reset(); }

void VKShaderCache::Open()
{
	if (!GSConfig.DisableShaderCache)
	{
		m_pipeline_cache_filename = GetPipelineCacheBaseFileName(GSConfig.UseDebugDevice);

		const std::string base_filename = GetShaderCacheBaseFileName(GSConfig.UseDebugDevice);
		const std::string index_filename = base_filename + ".idx";
		const std::string blob_filename = base_filename + ".bin";

		if (!ReadExistingShaderCache(index_filename, blob_filename))
			CreateNewShaderCache(index_filename, blob_filename);

		if (!ReadExistingPipelineCache())
			CreateNewPipelineCache();
	}
	else
	{
		CreateNewPipelineCache();
	}
}

VkPipelineCache VKShaderCache::GetPipelineCache(bool set_dirty /*= true*/)
{
	if (m_pipeline_cache == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	m_pipeline_cache_dirty |= set_dirty;
	return m_pipeline_cache;
}

bool VKShaderCache::CreateNewShaderCache(const std::string& index_filename, const std::string& blob_filename)
{
	if (path_is_valid(index_filename.c_str()))
	{
		Console.Warning("Removing existing index file '%s'", index_filename.c_str());
		FileSystem::DeleteFilePath(index_filename.c_str());
	}
	if (path_is_valid(blob_filename.c_str()))
	{
		Console.Warning("Removing existing blob file '%s'", blob_filename.c_str());
		FileSystem::DeleteFilePath(blob_filename.c_str());
	}

	m_index_file = FileSystem::OpenFile(index_filename.c_str(), "wb");
	if (!m_index_file)
	{
		Console.Error("Failed to open index file '%s' for writing", index_filename.c_str());
		return false;
	}

	const u32 file_version = SHADER_CACHE_VERSION;
	VK_PIPELINE_CACHE_HEADER header;
	FillPipelineCacheHeader(&header);

	if (rfwrite(&file_version, sizeof(file_version), 1, m_index_file) != 1 ||
		rfwrite(&header, sizeof(header), 1, m_index_file) != 1)
	{
		Console.Error("Failed to write header to index file '%s'", index_filename.c_str());
		rfclose(m_index_file);
		m_index_file = nullptr;
		FileSystem::DeleteFilePath(index_filename.c_str());
		return false;
	}

	m_blob_file = FileSystem::OpenFile(blob_filename.c_str(), "w+b");
	if (!m_blob_file)
	{
		Console.Error("Failed to open blob file '%s' for writing", blob_filename.c_str());
		rfclose(m_index_file);
		m_index_file = nullptr;
		FileSystem::DeleteFilePath(index_filename.c_str());
		return false;
	}

	return true;
}

bool VKShaderCache::ReadExistingShaderCache(const std::string& index_filename, const std::string& blob_filename)
{
	m_index_file = FileSystem::OpenFile(index_filename.c_str(), "r+b");
	if (!m_index_file)
	{
		// special case here: when there's a sharing violation (i.e. two instances running),
		// we don't want to blow away the cache. so just continue without a cache.
		if (errno == EACCES)
		{
			Console.WriteLn("Failed to open shader cache index with EACCES, are you running two instances?");
			return true;
		}

		return false;
	}

	u32 file_version = 0;
	if (rfread(&file_version, sizeof(file_version), 1, m_index_file) != 1 || file_version != SHADER_CACHE_VERSION)
	{
		Console.Error("Bad file/data version in '%s'", index_filename.c_str());
		rfclose(m_index_file);
		m_index_file = nullptr;
		return false;
	}

	VK_PIPELINE_CACHE_HEADER header;
	if (rfread(&header, sizeof(header), 1, m_index_file) != 1 || !ValidatePipelineCacheHeader(header))
	{
		Console.Error("Mismatched pipeline cache header in '%s' (GPU/driver changed?)", index_filename.c_str());
		rfclose(m_index_file);
		m_index_file = nullptr;
		return false;
	}

	m_blob_file = FileSystem::OpenFile(blob_filename.c_str(), "a+b");
	if (!m_blob_file)
	{
		Console.Error("Blob file '%s' is missing", blob_filename.c_str());
		rfclose(m_index_file);
		m_index_file = nullptr;
		return false;
	}

	rfseek(m_blob_file, 0, SEEK_END);
	const u32 blob_file_size = static_cast<u32>(rftell(m_blob_file));

	for (;;)
	{
		CacheIndexEntry entry;
		if (rfread(&entry, sizeof(entry), 1, m_index_file) != 1 ||
				(entry.file_offset + entry.blob_size) > blob_file_size)
		{
			if (filestream_eof(m_index_file))
				break;

			Console.Error("Failed to read entry from '%s', corrupt file?", index_filename.c_str());
			m_index.clear();
			rfclose(m_blob_file);
			m_blob_file = nullptr;
			rfclose(m_index_file);
			m_index_file = nullptr;
			return false;
		}

		const CacheIndexKey key{entry.source_hash_low, entry.source_hash_high, entry.source_length,
			static_cast<ShaderType>(entry.shader_type)};
		const CacheIndexData data{entry.file_offset, entry.blob_size};
		m_index.emplace(key, data);
	}

	// ensure we don't write before seeking
	rfseek(m_index_file, 0, SEEK_END);

	Console.WriteLn("Read %zu entries from '%s'", m_index.size(), index_filename.c_str());
	return true;
}

void VKShaderCache::CloseShaderCache()
{
	if (m_index_file)
	{
		rfclose(m_index_file);
		m_index_file = nullptr;
	}
	if (m_blob_file)
	{
		rfclose(m_blob_file);
		m_blob_file = nullptr;
	}
}

bool VKShaderCache::CreateNewPipelineCache()
{
	if (!m_pipeline_cache_filename.empty() && path_is_valid(m_pipeline_cache_filename.c_str()))
	{
		Console.Warning("Removing existing pipeline cache '%s'", m_pipeline_cache_filename.c_str());
		FileSystem::DeleteFilePath(m_pipeline_cache_filename.c_str());
	}

	const VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, 0, nullptr};
	VkResult res = vkCreatePipelineCache(vk_init_info.device, &ci, nullptr, &m_pipeline_cache);
	if (res != VK_SUCCESS)
		return false;

	m_pipeline_cache_dirty = true;
	return true;
}

bool VKShaderCache::ReadExistingPipelineCache()
{
	std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(m_pipeline_cache_filename.c_str());
	if (!data.has_value())
		return false;

	if (data->size() < sizeof(VK_PIPELINE_CACHE_HEADER))
	{
		Console.Error("Pipeline cache at '%s' is too small", m_pipeline_cache_filename.c_str());
		return false;
	}

	VK_PIPELINE_CACHE_HEADER header;
	memcpy(&header, data->data(), sizeof(header));
	if (!ValidatePipelineCacheHeader(header))
		return false;

	const VkPipelineCacheCreateInfo ci{
		VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, data->size(), data->data()};
	VkResult res = vkCreatePipelineCache(vk_init_info.device, &ci, nullptr, &m_pipeline_cache);
	if (res != VK_SUCCESS)
		return false;

	return true;
}

bool VKShaderCache::FlushPipelineCache()
{
	if (m_pipeline_cache == VK_NULL_HANDLE || !m_pipeline_cache_dirty || m_pipeline_cache_filename.empty())
		return false;

	size_t data_size;
	VkResult res = vkGetPipelineCacheData(vk_init_info.device, m_pipeline_cache, &data_size, nullptr);
	if (res != VK_SUCCESS)
		return false;

	std::vector<u8> data(data_size);
	res = vkGetPipelineCacheData(vk_init_info.device, m_pipeline_cache, &data_size, data.data());
	if (res != VK_SUCCESS)
		return false;

	data.resize(data_size);

	// Save disk writes if it hasn't changed, think of the poor SSDs.
	int32_t sd_size = path_get_size(m_pipeline_cache_filename.c_str());
	if (sd_size == -1 || sd_size != static_cast<s64>(data_size))
	{
		Console.WriteLn("Writing %zu bytes to '%s'", data_size, m_pipeline_cache_filename.c_str());
		if (!FileSystem::WriteBinaryFile(m_pipeline_cache_filename.c_str(), data.data(), data.size()))
		{
			Console.Error("Failed to write pipeline cache to '%s'", m_pipeline_cache_filename.c_str());
			return false;
		}
	}
	else
	{
		Console.WriteLn(
				"Skipping updating pipeline cache '%s' due to no changes.", m_pipeline_cache_filename.c_str());
	}

	m_pipeline_cache_dirty = false;
	return true;
}

void VKShaderCache::ClosePipelineCache()
{
	if (m_pipeline_cache == VK_NULL_HANDLE)
		return;

	vkDestroyPipelineCache(vk_init_info.device, m_pipeline_cache, nullptr);
	m_pipeline_cache = VK_NULL_HANDLE;
}

std::string VKShaderCache::GetShaderCacheBaseFileName(bool debug)
{
	std::string base_filename = "vulkan_shaders";

	if (debug)
		base_filename += "_debug";

	return Path::Combine(EmuFolders::Cache, base_filename);
}

std::string VKShaderCache::GetPipelineCacheBaseFileName(bool debug)
{
	std::string base_filename = "vulkan_pipelines";
	if (debug)
		base_filename += "_debug";
	base_filename += ".bin";
	return Path::Combine(EmuFolders::Cache, base_filename);
}

VKShaderCache::CacheIndexKey VKShaderCache::GetCacheKey(ShaderType type, const std::string_view& shader_code)
{
	union HashParts
	{
		struct
		{
			u64 hash_low;
			u64 hash_high;
		};
		u8 hash[16];
	};
	HashParts h;

	MD5Digest digest;
	digest.Update(shader_code.data(), static_cast<u32>(shader_code.length()));
	digest.Final(h.hash);

	return CacheIndexKey{h.hash_low, h.hash_high, static_cast<u32>(shader_code.length()), type};
}

std::optional<SPIRVCodeVector> VKShaderCache::GetShaderSPV(
		ShaderType type, std::string_view shader_code)
{
	const auto key = GetCacheKey(type, shader_code);
	auto iter = m_index.find(key);
	if (iter == m_index.end())
		return CompileAndAddShaderSPV(key, shader_code);

	SPIRVCodeVector spv(iter->second.blob_size);
	if (rfseek(m_blob_file, iter->second.file_offset, SEEK_SET) != 0 ||
			rfread(spv.data(), sizeof(SPIRVCodeType), iter->second.blob_size, m_blob_file) !=
			iter->second.blob_size)
	{
		Console.Error("Read blob from file failed, recompiling");
		return CompileShader(type, shader_code, GSConfig.UseDebugDevice);
	}

	return spv;
}

VkShaderModule VKShaderCache::GetShaderModule(ShaderType type, std::string_view shader_code)
{
	std::optional<SPIRVCodeVector> spv = GetShaderSPV(type, shader_code);
	if (!spv.has_value())
		return VK_NULL_HANDLE;

	const VkShaderModuleCreateInfo ci{
		VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, spv->size() * sizeof(SPIRVCodeType), spv->data()};

	VkShaderModule mod;
	VkResult res = vkCreateShaderModule(vk_init_info.device, &ci, nullptr, &mod);
	if (res != VK_SUCCESS)
		return VK_NULL_HANDLE;

	return mod;
}

VkShaderModule VKShaderCache::GetVertexShader(std::string_view shader_code)
{
	return GetShaderModule(ShaderType::Vertex, std::move(shader_code));
}

VkShaderModule VKShaderCache::GetFragmentShader(std::string_view shader_code)
{
	return GetShaderModule(ShaderType::Fragment, std::move(shader_code));
}

VkShaderModule VKShaderCache::GetComputeShader(std::string_view shader_code)
{
	return GetShaderModule(ShaderType::Compute, std::move(shader_code));
}

std::optional<SPIRVCodeVector> VKShaderCache::CompileAndAddShaderSPV(
		const CacheIndexKey& key, std::string_view shader_code)
{
	std::optional<SPIRVCodeVector> spv = CompileShader(key.shader_type, shader_code, GSConfig.UseDebugDevice);
	if (!spv.has_value())
		return {};

	if (!m_blob_file || rfseek(m_blob_file, 0, SEEK_END) != 0)
		return spv;

	CacheIndexData data;
	data.file_offset = static_cast<u32>(rftell(m_blob_file));
	data.blob_size = static_cast<u32>(spv->size());

	CacheIndexEntry entry = {};
	entry.source_hash_low = key.source_hash_low;
	entry.source_hash_high = key.source_hash_high;
	entry.source_length = key.source_length;
	entry.shader_type = static_cast<u32>(key.shader_type);
	entry.blob_size = data.blob_size;
	entry.file_offset = data.file_offset;

	if (rfwrite(spv->data(), sizeof(SPIRVCodeType), entry.blob_size, m_blob_file) != entry.blob_size ||
			filestream_flush(m_blob_file) != 0 || rfwrite(&entry, sizeof(entry), 1, m_index_file) != 1 ||
			filestream_flush(m_index_file) != 0)
	{
		Console.Error("Failed to write shader blob to file");
		return spv;
	}

	m_index.emplace(key, data);
	return spv;
}
