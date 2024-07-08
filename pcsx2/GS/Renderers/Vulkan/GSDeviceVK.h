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

#include "GSTextureVK.h"
#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "VKStreamBuffer.h"
#include "common/HashCombine.h"
#include "vk_mem_alloc.h"
#include <array>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct vk_init_info_t
{
	VkInstance instance;
	VkPhysicalDevice gpu;
	VkDevice device;
	const char **required_device_extensions;
	unsigned num_required_device_extensions;
	const char **required_device_layers;
	unsigned num_required_device_layers;
	const VkPhysicalDeviceFeatures *required_features;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
};

class GSDeviceVK final : public GSDevice
{
public:
        enum : u32
        {
	       NUM_COMMAND_BUFFERS = 3,
	       TEXTURE_BUFFER_SIZE = 64 * 1024 * 1024
        };
	enum FeedbackLoopFlag : u8
	{
		FeedbackLoopFlag_None = 0,
		FeedbackLoopFlag_ReadAndWriteRT = 1,
		FeedbackLoopFlag_ReadDS = 2,
	};

	struct OptionalExtensions
	{
		bool vk_ext_provoking_vertex : 1;
		bool vk_ext_memory_budget : 1;
		bool vk_ext_line_rasterization : 1;
		bool vk_ext_rasterization_order_attachment_access : 1;
		bool vk_khr_driver_properties : 1;
		bool vk_khr_fragment_shader_barycentric : 1;
		bool vk_khr_shader_draw_parameters : 1;
	};

	struct alignas(8) PipelineSelector
	{
		GSHWDrawConfig::PSSelector ps;

		union
		{
			struct
			{
				u32 topology : 2;
				u32 rt : 1;
				u32 ds : 1;
				u32 line_width : 1;
				u32 feedback_loop_flags : 2;
			};

			u32 key;
		};

		GSHWDrawConfig::BlendState bs;
		GSHWDrawConfig::VSSelector vs;
		GSHWDrawConfig::DepthStencilSelector dss;
		GSHWDrawConfig::ColorMaskSelector cms;
		u8 pad;

		__fi bool operator==(const PipelineSelector& p) const { return BitEqual(*this, p); }
		__fi bool operator!=(const PipelineSelector& p) const { return !BitEqual(*this, p); }
		

		__fi PipelineSelector() { memset(this, 0, sizeof(*this)); }

		__fi bool IsRTFeedbackLoop() const { return ((feedback_loop_flags & FeedbackLoopFlag_ReadAndWriteRT) != 0); }
		__fi bool IsTestingAndSamplingDepth() const { return ((feedback_loop_flags & FeedbackLoopFlag_ReadDS) != 0); }
	};

	struct PipelineSelectorHash
	{
		std::size_t operator()(const PipelineSelector& e) const noexcept
		{
			std::size_t hash = 0;
			HashCombine(hash, e.vs.key, e.ps.key_hi, e.ps.key_lo, e.dss.key, e.cms.key, e.bs.key, e.key);
			return hash;
		}
	};

	enum : u32
	{
		NUM_TFX_DYNAMIC_OFFSETS = 2,
		NUM_TFX_DRAW_TEXTURES = 2,
		NUM_TFX_RT_TEXTURES = 2,
		NUM_CONVERT_TEXTURES = 1,
		NUM_CONVERT_SAMPLERS = 1,
		CONVERT_PUSH_CONSTANTS_SIZE = 96,

		VERTEX_BUFFER_SIZE = 32 * 1024 * 1024,
		INDEX_BUFFER_SIZE = 16 * 1024 * 1024,
		VERTEX_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
		FRAGMENT_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024
	};
	enum TFX_DESCRIPTOR_SET : u32
	{
		TFX_DESCRIPTOR_SET_UBO,
		TFX_DESCRIPTOR_SET_TEXTURES,
		TFX_DESCRIPTOR_SET_RT,

		NUM_TFX_DESCRIPTOR_SETS,
	};
	enum TFX_TEXTURES : u32
	{
		TFX_TEXTURE_TEXTURE,
		TFX_TEXTURE_PALETTE,
		TFX_TEXTURE_RT,
		TFX_TEXTURE_PRIMID,

		NUM_TFX_TEXTURES
	};

       // Returns a list of Vulkan-compatible GPUs.
       using GPUList = std::vector<VkPhysicalDevice>;
       using GPUNameList = std::vector<std::string>;
       static GPUNameList EnumerateGPUNames(VkInstance instance);

       // Global state accessors
       __fi VmaAllocator GetAllocator() const { return m_allocator; }
       __fi VkQueue GetGraphicsQueue() const { return m_graphics_queue; }
       __fi u32 GetGraphicsQueueFamilyIndex() const { return m_graphics_queue_family_index; }
       __fi const VkPhysicalDeviceProperties& GetDeviceProperties() const { return m_device_properties; }
       __fi const VkPhysicalDeviceFeatures& GetDeviceFeatures() const { return m_device_features; }
       __fi const VkPhysicalDeviceLimits& GetDeviceLimits() const { return m_device_properties.limits; }
       __fi const OptionalExtensions& GetOptionalExtensions() const { return m_optional_extensions; }

       // Helpers for getting constants
       __fi u32 GetUniformBufferAlignment() const
       {
	       return static_cast<u32>(m_device_properties.limits.minUniformBufferOffsetAlignment);
       }
       __fi u32 GetBufferCopyOffsetAlignment() const
       {
	       return static_cast<u32>(m_device_properties.limits.optimalBufferCopyOffsetAlignment);
       }
       __fi u32 GetBufferCopyRowPitchAlignment() const
       {
	       return static_cast<u32>(m_device_properties.limits.optimalBufferCopyRowPitchAlignment);
       }
       __fi u32 GetMaxImageDimension2D() const
       {
	       return m_device_properties.limits.maxImageDimension2D;
       }

       /// Returns true if running on an NVIDIA GPU.
	__fi bool IsDeviceNVIDIA() const { return (m_device_properties.vendorID == 0x10DE); }

	/// Returns true if running on an AMD GPU.
	__fi bool IsDeviceAMD() const { return (m_device_properties.vendorID == 0x1002); }

       // Creates a simple render pass.
       VkRenderPass GetRenderPass(VkFormat color_format, VkFormat depth_format,
		       VkAttachmentLoadOp color_load_op = VK_ATTACHMENT_LOAD_OP_LOAD,
		       VkAttachmentStoreOp color_store_op = VK_ATTACHMENT_STORE_OP_STORE,
		       VkAttachmentLoadOp depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD,
		       VkAttachmentStoreOp depth_store_op = VK_ATTACHMENT_STORE_OP_STORE,
		       VkAttachmentLoadOp stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		       VkAttachmentStoreOp stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		       bool color_feedback_loop = false, bool depth_sampling = false);

       // Gets a non-clearing version of the specified render pass. Slow, don't call in hot path.
       VkRenderPass GetRenderPassForRestarting(VkRenderPass pass);

       // These command buffers are allocated per-frame. They are valid until the command buffer
       // is submitted, after that you should call these functions again.
       __fi u32 GetCurrentCommandBufferIndex() const { return m_current_frame; }
       __fi VkCommandBuffer GetCurrentCommandBuffer() const { return m_current_command_buffer; }
       __fi VKStreamBuffer& GetTextureUploadBuffer() { return m_texture_upload_buffer; }
       VkCommandBuffer GetCurrentInitCommandBuffer();

       /// Allocates a descriptor set from the pool reserved for the current frame.
       VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout set_layout);

       /// Allocates a descriptor set from the pool reserved for the current frame.
       VkDescriptorSet AllocatePersistentDescriptorSet(VkDescriptorSetLayout set_layout);

       /// Frees a descriptor set allocated from the global pool.
       void FreeGlobalDescriptorSet(VkDescriptorSet set);

       // Fence "counters" are used to track which commands have been completed by the GPU.
       // If the last completed fence counter is greater or equal to N, it means that the work
       // associated counter N has been completed by the GPU. The value of N to associate with
       // commands can be retreived by calling GetCurrentFenceCounter().
       u64 GetCompletedFenceCounter() const { return m_completed_fence_counter; }

       // Gets the fence that will be signaled when the currently executing command buffer is
       // queued and executed. Do not wait for this fence before the buffer is executed.
       u64 GetCurrentFenceCounter() const { return m_frame_resources[m_current_frame].fence_counter; }

       void SubmitCommandBuffer();
       void MoveToNextCommandBuffer();

       // Schedule a vulkan resource for destruction later on. This will occur when the command buffer
       // is next re-used, and the GPU has finished working with the specified resource.
       void DeferBufferDestruction(VkBuffer object);
       void DeferBufferDestruction(VkBuffer object, VmaAllocation allocation);
       void DeferFramebufferDestruction(VkFramebuffer object);
       void DeferImageDestruction(VkImage object);
       void DeferImageDestruction(VkImage object, VmaAllocation allocation);
       void DeferImageViewDestruction(VkImageView object);
       void DeferSamplerDestruction(VkSampler sampler);

       // Wait for a fence to be completed.
       // Also invokes callbacks for completion.
       void WaitForFenceCounter(u64 fence_counter);

       void WaitForGPUIdle();

       // Allocates a temporary CPU staging buffer, fires the callback with it to populate, then copies to a GPU buffer.
       bool AllocatePreinitializedGPUBuffer(u32 size, VkBuffer* gpu_buffer, VmaAllocation* gpu_allocation,
		       VkBufferUsageFlags gpu_usage, const std::function<void(void*)>& fill_callback);

private:
       union RenderPassCacheKey
       {
	       struct
	       {
		       u32 color_format : 8;
		       u32 depth_format : 8;
		       u32 color_load_op : 2;
		       u32 color_store_op : 1;
		       u32 depth_load_op : 2;
		       u32 depth_store_op : 1;
		       u32 stencil_load_op : 2;
		       u32 stencil_store_op : 1;
		       u32 color_feedback_loop : 1;
		       u32 depth_sampling : 1;
	       };

	       u32 key;
       };

       using ExtensionList = std::vector<const char*>;
       bool SelectDeviceExtensions(ExtensionList* extension_list);
       void SelectDeviceFeatures(const VkPhysicalDeviceFeatures* required_features);
       bool CreateDevice(const char** required_device_extensions,
		       u32 num_required_device_extensions, const char** required_device_layers, u32 num_required_device_layers,
		       const VkPhysicalDeviceFeatures* required_features);
       bool ProcessDeviceExtensions();

       bool CreateAllocator();
       bool CreateCommandBuffers();
       void DestroyCommandBuffers();
       bool CreateGlobalDescriptorPool();
       bool CreateTextureStreamBuffer();

       VkRenderPass CreateCachedRenderPass(RenderPassCacheKey key);

       void CommandBufferCompleted(u32 index);
       void ActivateCommandBuffer(u32 index);
       void WaitForCommandBufferCompletion(u32 index);

       struct FrameResources
       {
	       // [0] - Init (upload) command buffer, [1] - draw command buffer
	       VkCommandPool command_pool = VK_NULL_HANDLE;
	       std::array<VkCommandBuffer, 2> command_buffers{VK_NULL_HANDLE, VK_NULL_HANDLE};
	       VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
	       VkFence fence = VK_NULL_HANDLE;
	       u64 fence_counter = 0;
	       bool init_buffer_used = false;

	       std::vector<std::function<void()>> cleanup_resources;
       };

       VmaAllocator m_allocator = VK_NULL_HANDLE;

       VkCommandBuffer m_current_command_buffer = VK_NULL_HANDLE;

       VkDescriptorPool m_global_descriptor_pool = VK_NULL_HANDLE;

       VkQueue m_graphics_queue = VK_NULL_HANDLE;
       u32 m_graphics_queue_family_index = 0;

       std::array<FrameResources, NUM_COMMAND_BUFFERS> m_frame_resources;
       u64 m_next_fence_counter = 1;
       u64 m_completed_fence_counter = 0;
       u32 m_current_frame = 0;

       VKStreamBuffer m_texture_upload_buffer;

       bool m_last_submit_failed = false;

       std::map<u32, VkRenderPass> m_render_pass_cache;

       VkPhysicalDeviceFeatures m_device_features = {};
       VkPhysicalDeviceProperties m_device_properties = {};
       VkPhysicalDeviceDriverPropertiesKHR m_device_driver_properties = {};
       OptionalExtensions m_optional_extensions = {};
	VkDescriptorSetLayout m_utility_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_utility_pipeline_layout = VK_NULL_HANDLE;

	VkDescriptorSetLayout m_tfx_ubo_ds_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_tfx_sampler_ds_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_tfx_rt_texture_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_tfx_pipeline_layout = VK_NULL_HANDLE;

	VKStreamBuffer m_vertex_stream_buffer;
	VKStreamBuffer m_index_stream_buffer;
	VKStreamBuffer m_vertex_uniform_stream_buffer;
	VKStreamBuffer m_fragment_uniform_stream_buffer;
	VkBuffer m_expand_index_buffer = VK_NULL_HANDLE;
	VmaAllocation m_expand_index_buffer_allocation = VK_NULL_HANDLE;

	VkSampler m_point_sampler = VK_NULL_HANDLE;
	VkSampler m_linear_sampler = VK_NULL_HANDLE;

	std::unordered_map<u32, VkSampler> m_samplers;

	std::array<VkPipeline, static_cast<int>(ShaderConvert::Count)> m_convert{};
	std::array<VkPipeline, 32> m_color_copy{};
	std::array<VkPipeline, 2> m_merge{};
	std::array<VkPipeline, NUM_INTERLACE_SHADERS> m_interlace{};
	VkPipeline m_hdr_setup_pipelines[2][2] = {}; // [depth][feedback_loop]
	VkPipeline m_hdr_finish_pipelines[2][2] = {}; // [depth][feedback_loop]
	VkRenderPass m_date_image_setup_render_passes[2][2] = {}; // [depth][clear]
	VkPipeline m_date_image_setup_pipelines[2][4] = {}; // [depth][datm]

	std::unordered_map<u32, VkShaderModule> m_tfx_vertex_shaders;
	std::unordered_map<GSHWDrawConfig::PSSelector, VkShaderModule, GSHWDrawConfig::PSSelectorHash> m_tfx_fragment_shaders;
	std::unordered_map<PipelineSelector, VkPipeline, PipelineSelectorHash> m_tfx_pipelines;

	VkRenderPass m_utility_color_render_pass_load = VK_NULL_HANDLE;
	VkRenderPass m_utility_color_render_pass_clear = VK_NULL_HANDLE;
	VkRenderPass m_utility_color_render_pass_discard = VK_NULL_HANDLE;
	VkRenderPass m_utility_depth_render_pass_load = VK_NULL_HANDLE;
	VkRenderPass m_utility_depth_render_pass_clear = VK_NULL_HANDLE;
	VkRenderPass m_utility_depth_render_pass_discard = VK_NULL_HANDLE;
	VkRenderPass m_date_setup_render_pass = VK_NULL_HANDLE;

	VkRenderPass m_tfx_render_pass[2][2][2][3][2][2][3][3] = {}; // [rt][ds][hdr][date][fbl][dsp][rt_op][ds_op]

	GSHWDrawConfig::VSConstantBuffer m_vs_cb_cache;
	GSHWDrawConfig::PSConstantBuffer m_ps_cb_cache;

	GSTexture* CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format) override;

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE,
		const GSRegEXTBUF& EXTBUF, u32 c, const bool linear) final;
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb) final;

	VkSampler GetSampler(GSHWDrawConfig::SamplerSelector ss);
	void ClearSamplerCache() final;

	VkShaderModule GetTFXVertexShader(GSHWDrawConfig::VSSelector sel);
	VkShaderModule GetTFXFragmentShader(const GSHWDrawConfig::PSSelector& sel);
	VkPipeline CreateTFXPipeline(const PipelineSelector& p);
	VkPipeline GetTFXPipeline(const PipelineSelector& p);

	VkShaderModule GetUtilityVertexShader(const char *source, const char* replace_main);
	VkShaderModule GetUtilityFragmentShader(const char *source, const char* replace_main);
	VkShaderModule GetUtilityVertexShader(const std::string& source, const char* replace_main);
	VkShaderModule GetUtilityFragmentShader(const std::string& source, const char* replace_main);

	bool CreateDeviceAndSwapChain();
	bool CheckFeatures();
	bool CreateNullTexture();
	bool CreateBuffers();
	bool CreatePipelineLayouts();
	bool CreateRenderPasses();

	bool CompileConvertPipelines();
	bool CompileInterlacePipelines();
	bool CompileMergePipelines();

	void DestroyResources();

public:
	GSDeviceVK();
	~GSDeviceVK() override;

	__fi static GSDeviceVK* GetInstance() { return static_cast<GSDeviceVK*>(g_gs_device.get()); }

	static void GetAdapters(std::vector<std::string>* adapters);

	/// Returns true if Vulkan is suitable as a default for the devices in the system.
	static bool IsSuitableDefaultRenderer();

	__fi VkRenderPass GetTFXRenderPass(bool rt, bool ds, bool hdr, bool stencil, bool fbl, bool dsp,
		VkAttachmentLoadOp rt_op, VkAttachmentLoadOp ds_op) const
	{
		return m_tfx_render_pass[rt][ds][hdr][stencil][fbl][dsp][rt_op][ds_op];
	}

	RenderAPI GetRenderAPI() const override;

	bool Create() override;
	void Destroy() override;

	PresentResult BeginPresent(bool frame_skip) override;
	void EndPresent() override;

	void ResetAPIState() override;
	void RestoreAPIState() override;

	void DrawPrimitive();
	void DrawIndexedPrimitive();
	void DrawIndexedPrimitive(int offset, int count);

	std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) override;

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override;

	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvert shader = ShaderConvert::COPY, bool linear = true) override;
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red,
		bool green, bool blue, bool alpha, ShaderConvert shader = ShaderConvert::COPY) override;
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect) override;
	void DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader) override;
	void DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTextureVK* dTex, ShaderConvert shader);

	void BeginRenderPassForStretchRect(
		GSTextureVK* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard = true);
	void DoStretchRect(GSTextureVK* sTex, const GSVector4& sRect, GSTextureVK* dTex, const GSVector4& dRect,
		VkPipeline pipeline, bool linear, bool allow_discard);
	void DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds);

	void UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize) override;
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM) override;
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect) override;

	void SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox);
	GSTextureVK* SetupPrimitiveTrackingDATE(GSHWDrawConfig& config);

	void IASetVertexBuffer(const void* vertex, size_t stride, size_t count);
	void IASetIndexBuffer(const void* index, size_t count);

	void PSSetShaderResource(int i, GSTexture* sr, bool check_state);
	void PSSetSampler(GSHWDrawConfig::SamplerSelector sel);

	void OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i& scissor,
		FeedbackLoopFlag feedback_loop = FeedbackLoopFlag_None);

	void SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb);
	void SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb);
	bool BindDrawPipeline(const PipelineSelector& p);

	void RenderHW(GSHWDrawConfig& config) override;
	void UpdateHWPipelineSelector(GSHWDrawConfig& config, PipelineSelector& pipe);
	void UploadHWDrawVerticesAndIndices(const GSHWDrawConfig& config);
	VkImageMemoryBarrier GetColorBufferBarrier(GSTextureVK* rt) const;
	void SendHWDraw(const GSHWDrawConfig& config, GSTextureVK* draw_rt, bool skip_first_barrier);

	//////////////////////////////////////////////////////////////////////////
	// Vulkan State
	//////////////////////////////////////////////////////////////////////////

public:
	VkFormat LookupNativeFormat(GSTexture::Format format) const;
	__fi VkFramebuffer GetCurrentFramebuffer() const { return m_current_framebuffer; }

	/// Ends any render pass, executes the command buffer, and invalidates cached state.
	void ExecuteCommandBuffer(bool wait_for_completion);
	void ExecuteCommandBufferAndRestartRenderPass(bool wait_for_completion);

	/// Set dirty flags on everything to force re-bind at next draw time.
	void InvalidateCachedState();

	/// Binds all dirty state to the command buffer.
	bool ApplyUtilityState(bool already_execed = false);
	bool ApplyTFXState(bool already_execed = false);

	void SetIndexBuffer(VkBuffer buffer);
	void SetBlendConstants(u8 color);
	void SetLineWidth(float width);

	void SetUtilityTexture(GSTexture* tex, VkSampler sampler);
	void SetUtilityPushConstants(const void* data, u32 size);
	void UnbindTexture(GSTextureVK* tex);

	// Ends a render pass if we're currently in one.
	// When Bind() is next called, the pass will be restarted.
	// Calling this function is allowed even if a pass has not begun.
	bool InRenderPass();
	void BeginRenderPass(VkRenderPass rp, const GSVector4i& rect);
	void BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, const VkClearValue* cv, u32 cv_count);
	void BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, u32 clear_color);
	void BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, float depth, u8 stencil);
	void EndRenderPass();

	void SetViewport(const VkViewport& viewport);
	void SetScissor(const GSVector4i& scissor);
	void SetPipeline(VkPipeline pipeline);

private:
	enum DIRTY_FLAG : u32
	{
		DIRTY_FLAG_TFX_SAMPLERS_DS = (1 << 0),
		DIRTY_FLAG_TFX_RT_TEXTURE_DS = (1 << 1),
		DIRTY_FLAG_TFX_UBO = (1 << 2),
		DIRTY_FLAG_UTILITY_TEXTURE = (1 << 3),
		DIRTY_FLAG_BLEND_CONSTANTS = (1 << 4),
		DIRTY_FLAG_LINE_WIDTH = (1 << 5),
		DIRTY_FLAG_INDEX_BUFFER = (1 << 6),
		DIRTY_FLAG_VIEWPORT = (1 << 7),
		DIRTY_FLAG_SCISSOR = (1 << 8),
		DIRTY_FLAG_PIPELINE = (1 << 9),
		DIRTY_FLAG_VS_CONSTANT_BUFFER = (1 << 10),
		DIRTY_FLAG_PS_CONSTANT_BUFFER = (1 << 11),

		DIRTY_BASE_STATE = DIRTY_FLAG_INDEX_BUFFER | DIRTY_FLAG_PIPELINE | DIRTY_FLAG_TFX_UBO |
						   DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR | DIRTY_FLAG_BLEND_CONSTANTS | DIRTY_FLAG_LINE_WIDTH,
		DIRTY_TFX_STATE = DIRTY_BASE_STATE | DIRTY_FLAG_TFX_SAMPLERS_DS | DIRTY_FLAG_TFX_RT_TEXTURE_DS,
		DIRTY_UTILITY_STATE = DIRTY_BASE_STATE | DIRTY_FLAG_UTILITY_TEXTURE,
		DIRTY_CONSTANT_BUFFER_STATE = DIRTY_FLAG_VS_CONSTANT_BUFFER | DIRTY_FLAG_PS_CONSTANT_BUFFER,
		ALL_DIRTY_STATE = DIRTY_BASE_STATE | DIRTY_TFX_STATE | DIRTY_UTILITY_STATE | DIRTY_CONSTANT_BUFFER_STATE,
	};

	enum class PipelineLayout
	{
		Undefined,
		TFX,
		Utility
	};

	void InitializeState();
	bool CreatePersistentDescriptorSets();

	void SetInitialState(VkCommandBuffer cmdbuf);
	void ApplyBaseState(u32 flags, VkCommandBuffer cmdbuf);

	// Which bindings/state has to be updated before the next draw.
	u32 m_dirty_flags = 0;
	FeedbackLoopFlag m_current_framebuffer_feedback_loop = FeedbackLoopFlag_None;
	bool m_has_feedback_loop_layout = false;

	VkBuffer m_index_buffer = VK_NULL_HANDLE;

	GSTextureVK* m_current_render_target = nullptr;
	GSTextureVK* m_current_depth_target = nullptr;
	VkFramebuffer m_current_framebuffer = VK_NULL_HANDLE;
	VkRenderPass m_current_render_pass = VK_NULL_HANDLE;
	GSVector4i m_current_render_pass_area = GSVector4i::zero();

	VkViewport m_viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
	float m_current_line_width = 1.0f;
	GSVector4i m_scissor = GSVector4i::zero();
	u8 m_blend_constant_color = 0;

	std::array<const GSTextureVK*, NUM_TFX_TEXTURES> m_tfx_textures{};
	VkSampler m_tfx_sampler = VK_NULL_HANDLE;
	u32 m_tfx_sampler_sel = 0;
	VkDescriptorSet m_tfx_ubo_descriptor_set = VK_NULL_HANDLE;
	VkDescriptorSet m_tfx_texture_descriptor_set = VK_NULL_HANDLE;
	VkDescriptorSet m_tfx_rt_descriptor_set = VK_NULL_HANDLE;
	std::array<u32, NUM_TFX_DYNAMIC_OFFSETS> m_tfx_dynamic_offsets{};

	const GSTextureVK* m_utility_texture = nullptr;
	VkSampler m_utility_sampler = VK_NULL_HANDLE;
	VkDescriptorSet m_utility_descriptor_set = VK_NULL_HANDLE;

	PipelineLayout m_current_pipeline_layout = PipelineLayout::Undefined;
	VkPipeline m_current_pipeline = VK_NULL_HANDLE;

	std::unique_ptr<GSTextureVK> m_null_texture;

	// current pipeline selector - we save this in the struct to avoid re-zeroing it every draw
	PipelineSelector m_pipeline_selector = {};
};

extern struct vk_init_info_t vk_init_info;
