// SPDX-FileCopyrightText: 2024 Hans-Kristian Arntzen
// SPDX-License-Identifier: LGPL-3.0+

#include "GSRendererPGS.h"
#include "GS/GSState.h"
#include "GS.h"
#include "math.hpp"
#include "muglm/muglm_impl.hpp"
#include "PerformanceMetrics.h"
#include "common/Console.h"
#include "logging.hpp"
#include <stdarg.h>

using namespace Vulkan;
using namespace ParallelGS;
using namespace Granite;

static retro_hw_render_interface_vulkan *hw_render_iface;
static std::unique_ptr<Vulkan::Context> vulkan_ctx;
static ImageHandle last_vsync_image;
extern retro_environment_t environ_cb;
extern retro_video_refresh_t video_cb;
extern retro_log_printf_t log_cb;

struct LibretroLog final : Util::LoggingInterface
{
	bool log(const char *tag, const char *fmt, va_list va) override
	{
		if (!log_cb)
			return false;

		char buf[4 * 1024];
		vsnprintf(buf, sizeof(buf), fmt, va);

		retro_log_level level = RETRO_LOG_DUMMY;
		if (strcmp(tag, "[ERROR]: ") == 0)
			level = RETRO_LOG_ERROR;
		else if (strcmp(tag, "[INFO]: ") == 0)
			level = RETRO_LOG_INFO;
		else if (strcmp(tag, "[WARN]: ") == 0)
			level = RETRO_LOG_WARN;

		log_cb(level, "%s", buf);
		return true;
	}
};
static LibretroLog logging_iface;

void pgs_set_hwrender_interface(retro_hw_render_interface_vulkan *iface)
{
	hw_render_iface = iface;
	Util::set_thread_logging_interface(&logging_iface);
}

const VkApplicationInfo *pgs_get_application_info()
{
	static VkApplicationInfo app =
	{
		VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
		"PCSX2", VK_MAKE_VERSION(1, 7, 0),
		"Granite", 0,
		VK_API_VERSION_1_2
	};

	return &app;
}

bool pgs_create_device(retro_vulkan_context *context,
	VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	const char **required_device_extensions, unsigned num_required_device_extensions,
	const char **required_device_layers, unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures *required_features)
{
	// We only expect v2 interface.
	return false;
}

void pgs_destroy_device()
{
	last_vsync_image.reset();
	vulkan_ctx.reset();
	hw_render_iface = nullptr;
}

VkInstance pgs_create_instance(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	const VkApplicationInfo *app,
	retro_vulkan_create_instance_wrapper_t create_instance_wrapper,
	void *opaque)
{
	Util::set_thread_logging_interface(&logging_iface);
	vulkan_ctx.reset();

	// Always force the reload, since the other backends may clobber the volk pointers.
	if (!Context::init_loader(get_instance_proc_addr, true))
		return VK_NULL_HANDLE;

	vulkan_ctx = std::make_unique<Context>();
	Context::SystemHandles handles = {};
	vulkan_ctx->set_system_handles(handles);
	vulkan_ctx->set_num_thread_indices(1);
	vulkan_ctx->set_application_info(app);

	struct Factory final : Vulkan::InstanceFactory
	{
		VkInstance create_instance(const VkInstanceCreateInfo *info) override
		{
			return wrapper(opaque, info);
		}

		retro_vulkan_create_instance_wrapper_t wrapper = nullptr;
		void *opaque = nullptr;
	} factory;

	factory.wrapper = create_instance_wrapper;
	factory.opaque = opaque;
	vulkan_ctx->set_instance_factory(&factory);

	if (!vulkan_ctx->init_instance(nullptr, 0))
	{
		vulkan_ctx.reset();
		return VK_NULL_HANDLE;
	}

	vulkan_ctx->release_instance();
	return vulkan_ctx->get_instance();
}

bool pgs_create_device2(
	struct retro_vulkan_context *context,
	VkInstance instance,
	VkPhysicalDevice gpu,
	VkSurfaceKHR surface,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	retro_vulkan_create_device_wrapper_t create_device_wrapper,
	void *opaque)
{
	// We are guaranteed that create_instance has been called here.
	if (!vulkan_ctx)
		return false;

	// Sanity check inputs.
	if (vulkan_ctx->get_instance() != instance)
		return false;
	if (Vulkan::Context::get_instance_proc_addr() != get_instance_proc_addr)
		return false;

	struct Factory final : Vulkan::DeviceFactory
	{
		VkDevice create_device(VkPhysicalDevice gpu, const VkDeviceCreateInfo *info) override
		{
			return wrapper(gpu, opaque, info);
		}

		retro_vulkan_create_device_wrapper_t wrapper = nullptr;
		void *opaque = nullptr;
	} factory;

	factory.wrapper = create_device_wrapper;
	factory.opaque = opaque;
	vulkan_ctx->set_device_factory(&factory);

	if (!vulkan_ctx->init_device(gpu, surface, nullptr, 0, Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT))
		return false;

	vulkan_ctx->release_device();
	context->gpu = vulkan_ctx->get_gpu();
	context->device = vulkan_ctx->get_device();
	context->presentation_queue = vulkan_ctx->get_queue_info().queues[Vulkan::QUEUE_INDEX_GRAPHICS];
	context->presentation_queue_family_index = vulkan_ctx->get_queue_info().family_indices[Vulkan::QUEUE_INDEX_GRAPHICS];
	context->queue = vulkan_ctx->get_queue_info().queues[Vulkan::QUEUE_INDEX_GRAPHICS];
	context->queue_family_index = vulkan_ctx->get_queue_info().family_indices[Vulkan::QUEUE_INDEX_GRAPHICS];
	return true;
}

GSRendererPGS::GSRendererPGS(u8 *basemem)
	: priv(reinterpret_cast<PrivRegisterState *>(basemem))
{
}

u8 *GSRendererPGS::GetRegsMem()
{
	return reinterpret_cast<u8 *>(priv);
}

GSRendererPGS::~GSRendererPGS()
{
	last_vsync_image.reset();
}

struct ParsedSuperSampling
{
	SuperSampling super_sampling;
	bool ordered;
};

static ParsedSuperSampling parse_super_sampling_options(u8 super_sampling)
{
	ParsedSuperSampling parsed = {};

	if (super_sampling > 5)
		super_sampling = 5;

	parsed.ordered = super_sampling == 3;

	if (super_sampling >= 3)
		super_sampling--;
	parsed.super_sampling = SuperSampling(1u << super_sampling);

	return parsed;
}

bool GSRendererPGS::Init()
{
	if (!vulkan_ctx)
		return false;

	dev.set_context(*vulkan_ctx);
	// We'll cycle through a lot of these.
	dev.init_frame_contexts(12);
	dev.set_queue_lock([]() {
	                     if (hw_render_iface)
		                     hw_render_iface->lock_queue(hw_render_iface->handle);
	                   },
	                   []() {
	                     if (hw_render_iface)
		                     hw_render_iface->unlock_queue(hw_render_iface->handle);
	                   });

	GSOptions opts = {};
	opts.vram_size = GSLocalMemory::m_vmsize;

	auto parsed                 = parse_super_sampling_options(GSConfig.PGSSuperSampling);
	opts.super_sampling         = parsed.super_sampling;
	opts.ordered_super_sampling = parsed.ordered;
	opts.dynamic_super_sampling = true;
	opts.super_sampled_textures = GSConfig.PGSSuperSampleTextures != 0;
	if (!iface.init(&dev, opts))
		return false;

	current_super_sampling         = opts.super_sampling;
	current_ordered_super_sampling = opts.ordered_super_sampling;

	Hacks hacks;
	hacks.disable_mipmaps = GSConfig.PGSDisableMipmaps;
	hacks.backbuffer_promotion = GSConfig.PGSSharpBackbuffer != 0;
	iface.set_hacks(hacks);

	return true;
}

void GSRendererPGS::Reset(bool /*hardware_reset*/)
{
	iface.reset_context_state();
}

void GSRendererPGS::UpdateConfig()
{
	auto parsed = parse_super_sampling_options(GSConfig.PGSSuperSampling);

	if (parsed.super_sampling != current_super_sampling || parsed.ordered != current_ordered_super_sampling ||
	    current_super_sample_textures != bool(GSConfig.PGSSuperSampleTextures))
	{
		iface.set_super_sampling_rate(parsed.super_sampling, parsed.ordered, GSConfig.PGSSuperSampleTextures != 0);
		current_super_sampling = parsed.super_sampling;
		current_ordered_super_sampling = parsed.ordered;
		current_super_sample_textures = bool(GSConfig.PGSSuperSampleTextures);
	}

	Hacks hacks;
	hacks.disable_mipmaps = GSConfig.PGSDisableMipmaps != 0;
	hacks.backbuffer_promotion = GSConfig.PGSSharpBackbuffer != 0;
	iface.set_hacks(hacks);
}

int GSRendererPGS::GetSaveStateSize()
{
	return GSState::GetSaveStateSize();
}

static void write_data(u8*& dst, const void* src, size_t size)
{
	memcpy(dst, src, size);
	dst += size;
}

template <typename T>
static void write_reg(u8*& data, T t)
{
	write_data(data, &t, sizeof(t));
}

static void read_data(const u8*& src, void* dst, size_t size)
{
	memcpy(dst, src, size);
	src += size;
}

template <typename T>
static void read_reg(const u8*& src, T &t)
{
	read_data(src, &t, sizeof(t));
}

int GSRendererPGS::Freeze(freezeData* data, bool sizeonly)
{
	if (sizeonly)
	{
		data->size = GetSaveStateSize();
		return 0;
	}

	if (!data->data || data->size < GetSaveStateSize())
		return -1;

	const void *vram = iface.map_vram_read(0, GSLocalMemory::m_vmsize);
	auto &regs = iface.get_register_state();

	u8 *ptr = data->data;

	write_reg(ptr, GSState::STATE_VERSION);
	write_reg(ptr, regs.prim);
	write_reg(ptr, regs.prmodecont);
	write_reg(ptr, regs.texclut);
	write_reg(ptr, regs.scanmsk);
	write_reg(ptr, regs.texa);
	write_reg(ptr, regs.fogcol);
	write_reg(ptr, regs.dimx);
	write_reg(ptr, regs.dthe);
	write_reg(ptr, regs.colclamp);
	write_reg(ptr, regs.pabe);
	write_reg(ptr, regs.bitbltbuf);
	write_reg(ptr, regs.trxdir);
	write_reg(ptr, regs.trxpos);
	write_reg(ptr, regs.trxreg);
	write_reg(ptr, regs.trxreg); // Dummy value

	for (const auto &ctx : regs.ctx)
	{
		write_reg(ptr, ctx.xyoffset);
		write_reg(ptr, ctx.tex0);
		write_reg(ptr, ctx.tex1);
		write_reg(ptr, ctx.clamp);
		write_reg(ptr, ctx.miptbl_1_3);
		write_reg(ptr, ctx.miptbl_4_6);
		write_reg(ptr, ctx.scissor);
		write_reg(ptr, ctx.alpha);
		write_reg(ptr, ctx.test);
		write_reg(ptr, ctx.fba);
		write_reg(ptr, ctx.frame);
		write_reg(ptr, ctx.zbuf);
	}

	write_reg(ptr, regs.rgbaq);
	write_reg(ptr, regs.st);
	write_reg(ptr, regs.uv.words[0]);
	write_reg(ptr, regs.fog.words[0]);
	// XYZ register, fill with dummy.
	write_reg(ptr, Reg64<XYZBits>{0});

	write_reg(ptr, UINT32_MAX); // Dummy GIFReg
	write_reg(ptr, UINT32_MAX);

	// Dummy transfer X/Y
	write_reg(ptr, uint32_t(0));
	write_reg(ptr, uint32_t(0));

	write_data(ptr, vram, GSLocalMemory::m_vmsize);

	// 4 GIF paths
	for (int i = 0; i < 4; i++)
	{
		auto gif_path = iface.get_gif_path(i);
		gif_path.tag.NLOOP -= gif_path.loop;
		write_data(ptr, &gif_path.tag, sizeof(gif_path.tag));
		write_reg(ptr, gif_path.reg);
	}

	// internal_Q
	write_reg(ptr, regs.internal_q);
	return 0;
}

int GSRendererPGS::Defrost(freezeData* data)
{
	if (!data || !data->data || data->size == 0)
		return -1;

	if (data->size < GetSaveStateSize())
		return -1;

	const u8* ptr = data->data;
	auto &regs = iface.get_register_state();

	u32 version;
	read_reg(ptr, version);

	if (version != GSState::STATE_VERSION)
	{
		Console.Error("GS: Savestate version is incompatible.  Load aborted.");
		return -1;
	}

	read_reg(ptr, regs.prim);
	read_reg(ptr, regs.prmodecont);
	read_reg(ptr, regs.texclut);
	read_reg(ptr, regs.scanmsk);
	read_reg(ptr, regs.texa);
	read_reg(ptr, regs.fogcol);
	read_reg(ptr, regs.dimx);
	read_reg(ptr, regs.dthe);
	read_reg(ptr, regs.colclamp);
	read_reg(ptr, regs.pabe);
	read_reg(ptr, regs.bitbltbuf);
	read_reg(ptr, regs.trxdir);
	read_reg(ptr, regs.trxpos);
	read_reg(ptr, regs.trxreg);
	// Dummy value
	ptr += sizeof(uint64_t);

	for (auto &ctx : regs.ctx)
	{
		read_reg(ptr, ctx.xyoffset);
		read_reg(ptr, ctx.tex0);
		read_reg(ptr, ctx.tex1);
		read_reg(ptr, ctx.clamp);
		read_reg(ptr, ctx.miptbl_1_3);
		read_reg(ptr, ctx.miptbl_4_6);
		read_reg(ptr, ctx.scissor);
		read_reg(ptr, ctx.alpha);
		read_reg(ptr, ctx.test);
		read_reg(ptr, ctx.fba);
		read_reg(ptr, ctx.frame);
		read_reg(ptr, ctx.zbuf);
	}

	read_reg(ptr, regs.rgbaq);
	read_reg(ptr, regs.st);
	read_reg(ptr, regs.uv.words[0]);
	read_reg(ptr, regs.fog.words[0]);
	// XYZ register, fill with dummy.
	ptr += sizeof(uint64_t);

	// Dummy GIFReg
	ptr += 2 * sizeof(uint32_t);

	// Dummy transfer X/Y
	ptr += 2 * sizeof(uint32_t);

	void *vram = iface.map_vram_write(0, GSLocalMemory::m_vmsize);
	read_data(ptr, vram, GSLocalMemory::m_vmsize);
	iface.end_vram_write(0, GSLocalMemory::m_vmsize);

	// 4 GIF paths
	for (int i = 0; i < 4; i++)
	{
		auto gif_path = iface.get_gif_path(i);
		gif_path.tag.NLOOP -= gif_path.loop;
		read_data(ptr, &gif_path.tag, sizeof(gif_path.tag));
		gif_path.loop = 0;
		read_reg(ptr, gif_path.reg);
	}

	// internal_Q
	read_reg(ptr, iface.get_register_state().internal_q);

	iface.clobber_register_state();
	return 0;
}

extern s8 setting_hint_widescreen;

void GSRendererPGS::VSync(u32 field, bool registers_written)
{
	iface.flush();
	iface.get_priv_register_state() = *priv;

	VSyncInfo info = {};

	info.phase = field;

	// Apparently this is needed for game-fixes.
	if (GSConfig.InterlaceMode != GSInterlaceMode::Automatic)
		info.phase ^= (static_cast<int>(GSConfig.InterlaceMode) - 2) & 1;

	info.anti_blur = GSConfig.PCRTCAntiBlur;
	info.force_progressive = true;
	info.overscan = GSConfig.PCRTCOverscan;
	info.crtc_offsets = GSConfig.PCRTCOffsets;
	info.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
	info.dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	info.dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	// The scaling blur is technically a blur ...
	info.adapt_to_internal_horizontal_resolution = GSConfig.PCRTCAntiBlur;
	info.raw_circuit_scanout                     = true;
	info.high_resolution_scanout                 = GSConfig.PGSHighResScanout != 0;
	auto vsync                                   = iface.vsync(info);

	auto stats = iface.consume_flush_stats();
	if (GSConfig.SkipDuplicateFrames && has_presented_in_current_swapchain &&
	    !registers_written && stats.num_render_passes == 0 && stats.num_copies == 0)
	{
		video_cb(nullptr, 0, 0, 0);
		return;
	}

	if (vsync.image)
	{
		last_internal_width = vsync.internal_width;
		last_internal_height = vsync.internal_height;
	}

	if (hw_render_iface)
	{
		if (vsync.image)
		{
			static retro_game_geometry geom  = {};
			bool geom_changed                = false;
			static float last_aspect         = 0.0f;
			static uint32_t last_base_width  = 0;
			static uint32_t last_base_height = 0;
			uint32_t new_base_width          = vsync.image->get_width();
			uint32_t new_base_height         = vsync.image->get_height();
			retro_vulkan_image vkimage       = {};
			vkimage.image_view = vsync.image->get_view().get_unorm_view();
			vkimage.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkimage.create_info = {
				VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,
				vsync.image->get_image(), VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
				{ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
				{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
			};

			if (new_base_width != last_base_width)
			{
				geom_changed     = true;
				geom.base_width  = new_base_width;
			}
			if (new_base_height != last_base_height)
			{
				geom_changed     = true;
				geom.base_height = new_base_height;
			}

			switch (setting_hint_widescreen)
			{
				case 1:
					geom.aspect_ratio = 16.0f / 9.0f;
					break;
				case 2:
					geom.aspect_ratio = 16.0f / 10.0f;
					break;
				case 3:
					geom.aspect_ratio = 21.0f / 9.0f;
					break;
				case 4:
					geom.aspect_ratio = 32.0f / 9.0f;
					break;
				case 0:
				default:
					geom.aspect_ratio = 4.0f / 3.0f;
					break;
			}
			float horizontal_scanout_ratio = float(vsync.internal_width) / float(vsync.mode_width);
			float vertical_scanout_ratio = float(vsync.internal_height) / float(vsync.mode_height);
			geom.aspect_ratio *= horizontal_scanout_ratio / vertical_scanout_ratio;
			if (last_aspect != geom.aspect_ratio)
				geom_changed = true;
			if (geom_changed)
				environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);

			hw_render_iface->set_image(hw_render_iface->handle, &vkimage, 0, nullptr, hw_render_iface->queue_index);
			video_cb(RETRO_HW_FRAME_BUFFER_VALID, new_base_width, new_base_height, 0);
			hw_render_iface->set_image(hw_render_iface->handle, nullptr, 0, nullptr, hw_render_iface->queue_index);
			last_vsync_image = vsync.image;
			last_base_width  = new_base_width;
			last_base_height = new_base_height;
			last_aspect      = geom.aspect_ratio;
		}
		else
			video_cb(nullptr, 0, 0, 0);
	}

	dev.next_frame_context();
}

void GSRendererPGS::Transfer(const u8* mem, u32 size)
{
	size *= 16;
	iface.gif_transfer(3, mem, size);
}

void GSRendererPGS::ReadFIFO(u8 *mem, u32 size)
{
	iface.read_transfer_fifo(mem, size);
}

void GSRendererPGS::GetInternalResolution(int *width, int *height)
{
	*width = int(last_internal_width);
	*height = int(last_internal_height);
}
