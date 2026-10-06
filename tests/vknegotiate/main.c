/* vknegotiate: the Vulkan context negotiation of the GSdx Vulkan renderer,
 * as three frontends drive it.
 *
 *   v2        knows GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT and
 *             creates the device through create_device2 and its wrapper,
 *             which adds an extension the core did not ask for
 *   v2retry   the same, but the wrapper fails the first, explicit-GPU call;
 *             the core must leave nothing behind and succeed on the second
 *             call, where it picks the GPU
 *   v1        has no support query; the core must offer version 1 and
 *             create the device through create_device
 *
 * Each then resets the context, runs frames, takes a savestate round
 * trip, and tears down as RetroArch does. The BIOS is a synthetic image
 * (secondload's) unless LRPS2_BIOS names a real one; with a real one the
 * frames must also arrive as images. VN_RENDERER picks the renderer
 * (Vulkan, the default, or paraLLEl-GS).
 *
 * Usage: vknegotiate <path-to-core> <scratch-dir> <v2|v2retry|v1> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define VK_NO_PROTOTYPES
#include <libretro.h>
#include <libretro_vulkan.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define VN_HANDLE HMODULE
#define vn_dlopen(p) LoadLibraryA(p)
#define vn_mkdir(p) _mkdir(p)
#define VN_VULKAN "vulkan-1.dll"
#else
#include <dlfcn.h>
#include <sys/stat.h>
#define VN_HANDLE void*
#define vn_dlopen(p) dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define vn_mkdir(p) mkdir((p), 0755)
#ifdef __APPLE__
#define VN_VULKAN "libvulkan.1.dylib"
#else
#define VN_VULKAN "libvulkan.so.1"
#endif
#endif

#define VN_FRAMES 120
#define VN_EXTRA_EXT "VK_KHR_swapchain"

typedef void (*vn_fn_t)(void);
typedef void retro_set_environment_t(retro_environment_t);
typedef void retro_set_video_refresh_t(retro_video_refresh_t);
typedef void retro_set_audio_sample_t(retro_audio_sample_t);
typedef void retro_set_audio_sample_batch_t(retro_audio_sample_batch_t);
typedef void retro_set_input_poll_t(retro_input_poll_t);
typedef void retro_set_input_state_t(retro_input_state_t);
typedef void retro_simple_t(void);
typedef bool retro_load_game_t(const struct retro_game_info*);
typedef size_t retro_serialize_size_t(void);
typedef bool retro_serialize_t(void*, size_t);
typedef bool retro_unserialize_t(const void*, size_t);

enum { VN_V2, VN_V2RETRY, VN_V1 };

static int s_mode;
static char s_system_dir[1024];
static const char *s_bios_name = "lrps2_vknegotiate.bin";
static struct retro_hw_render_callback s_hw;
static const struct retro_hw_render_context_negotiation_interface_vulkan *s_iface;
static struct retro_hw_render_interface_vulkan s_vk;
static int s_wrapper_calls;
static int s_wrapper_extra;
static VkDevice s_wrapper_device;
static unsigned s_frames, s_dupes, s_images, s_images_set;
static int s_queue_locked;

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkCreateInstance pvkCreateInstance;
static PFN_vkDestroyInstance pvkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices pvkEnumeratePhysicalDevices;
static PFN_vkEnumerateDeviceExtensionProperties pvkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice pvkCreateDevice;
static PFN_vkDestroyDevice pvkDestroyDevice;
static PFN_vkGetDeviceProcAddr pvkGetDeviceProcAddr;
static PFN_vkDeviceWaitIdle pvkDeviceWaitIdle;

static vn_fn_t vn_sym(VN_HANDLE h, const char* name)
{
#ifdef _WIN32
	return (vn_fn_t)GetProcAddress(h, name);
#else
	void* obj = dlsym(h, name);
	vn_fn_t fn = 0;
	memcpy(&fn, &obj, sizeof(fn));
	return fn;
#endif
}

static void vn_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	/* Warnings and errors; everything with VN_VERBOSE, for the core's
	 * own reports (the profiler's, say). */
	if (level < RETRO_LOG_WARN && !getenv("VN_VERBOSE"))
		return;
	va_start(ap, fmt);
	printf("  [core] ");
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static void vn_video(const void* d, unsigned w, unsigned h, size_t p)
{
	(void)w; (void)h; (void)p;
	s_frames++;
	if (!d)
		s_dupes++;
	if (d == RETRO_HW_FRAME_BUFFER_VALID && s_images_set)
		s_images++;
}
static void vn_audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t vn_audio_batch(const int16_t* d, size_t f) { (void)d; return f; }
static void vn_poll(void) { }
static int16_t vn_input(unsigned p, unsigned d, unsigned i, unsigned id)
{
	(void)p; (void)d; (void)i; (void)id;
	return 0;
}

/* The frontend side of the hardware interface: one sync index, and a
 * queue lock the core must pair. */
static void vn_set_image(void *h, const struct retro_vulkan_image *img,
	uint32_t n, const VkSemaphore *s, uint32_t q)
{
	(void)h; (void)n; (void)s; (void)q;
	s_images_set = img && img->image_view != VK_NULL_HANDLE;
}
static uint32_t vn_sync_index(void *h) { (void)h; return 0; }
static uint32_t vn_sync_mask(void *h) { (void)h; return 1; }
static void vn_set_cmd(void *h, uint32_t n, const VkCommandBuffer *c) { (void)h; (void)n; (void)c; }
static void vn_wait_sync(void *h) { (void)h; if (pvkDeviceWaitIdle) pvkDeviceWaitIdle(s_vk.device); }
static void vn_lock(void *h) { (void)h; s_queue_locked++; }
static void vn_unlock(void *h) { (void)h; s_queue_locked--; }
static void vn_set_sem(void *h, VkSemaphore s) { (void)h; (void)s; }

static bool vn_environment(unsigned cmd, void* data)
{
	struct retro_variable* var;
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
			((struct retro_log_callback*)data)->log = vn_log;
			return true;
		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
			*(const char**)data = s_system_dir;
			return true;
		case RETRO_ENVIRONMENT_GET_LANGUAGE:
			*(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
			return true;
		case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
			*(unsigned*)data = 2;
			return true;
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
			return true;
		case RETRO_ENVIRONMENT_SET_HW_RENDER:
			memcpy(&s_hw, data, sizeof(s_hw));
			return s_hw.context_type == RETRO_HW_CONTEXT_VULKAN;
		case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
			s_iface = (const struct retro_hw_render_context_negotiation_interface_vulkan*)data;
			return true;
		case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
			if (s_mode == VN_V1)
				return false;
			((struct retro_hw_render_context_negotiation_interface*)data)->interface_version =
				RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
			return true;
		case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
			*(const struct retro_hw_render_interface**)data =
				(const struct retro_hw_render_interface*)&s_vk;
			return true;
		case RETRO_ENVIRONMENT_GET_VARIABLE:
			var = (struct retro_variable*)data;
			var->value = NULL;
			if      (!strcmp(var->key, "pcsx2_bios"))     var->value = s_bios_name;
			else if (!strcmp(var->key, "pcsx2_renderer")) var->value = getenv("VN_RENDERER") ? getenv("VN_RENDERER") : "Vulkan";
			else if (!strcmp(var->key, "pcsx2_fastboot")) var->value = "disabled";
			return var->value != NULL;
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
			*(bool*)data = false;
			return true;
		default:
			return false;
	}
}

static VkInstance vn_instance_wrapper(void *opaque, const VkInstanceCreateInfo *ci)
{
	VkInstance instance = VK_NULL_HANDLE;
	(void)opaque;
	if (pvkCreateInstance(ci, NULL, &instance) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return instance;
}

/* RetroArch's wrapper in miniature: append what the frontend wants. */
static VkDevice vn_device_wrapper(VkPhysicalDevice gpu, void *opaque,
	const VkDeviceCreateInfo *ci)
{
	VkDeviceCreateInfo info = *ci;
	const char *exts[64];
	VkExtensionProperties props[512];
	uint32_t nprops = 512, i;
	VkDevice dev = VK_NULL_HANDLE;
	int have = 0;
	(void)opaque;
	s_wrapper_calls++;
	if (s_mode == VN_V2RETRY && s_wrapper_calls == 1)
		return VK_NULL_HANDLE;
	if (info.enabledExtensionCount >= 63)
		return VK_NULL_HANDLE;
	memcpy(exts, info.ppEnabledExtensionNames, info.enabledExtensionCount * sizeof(*exts));
	pvkEnumerateDeviceExtensionProperties(gpu, NULL, &nprops, props);
	for (i = 0; i < nprops; i++)
		if (!strcmp(props[i].extensionName, VN_EXTRA_EXT))
			have = 1;
	for (i = 0; i < info.enabledExtensionCount; i++)
		if (!strcmp(exts[i], VN_EXTRA_EXT))
			have = 0;
	if (have)
	{
		exts[info.enabledExtensionCount++] = VN_EXTRA_EXT;
		s_wrapper_extra = 1;
	}
	info.ppEnabledExtensionNames = exts;
	if (pvkCreateDevice(gpu, &info, NULL, &dev) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	s_wrapper_device = dev;
	return dev;
}

#define VN_INST(name) p##name = (PFN_##name)gipa(instance, #name)

static int vn_vulkan_load(void)
{
	VN_HANDLE lib = vn_dlopen(VN_VULKAN);
	if (!lib)
		return 0;
	gipa = (PFN_vkGetInstanceProcAddr)vn_sym(lib, "vkGetInstanceProcAddr");
	if (!gipa)
		return 0;
	pvkCreateInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
	return pvkCreateInstance != NULL;
}

/* A loader with no driver behind it (no GPU, no lavapipe) is a skip. */
static int vn_vulkan_has_device(void)
{
	VkInstanceCreateInfo ici;
	VkInstance instance = VK_NULL_HANDLE;
	uint32_t n = 0;
	memset(&ici, 0, sizeof(ici));
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	if (pvkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
		return 0;
	VN_INST(vkEnumeratePhysicalDevices);
	VN_INST(vkDestroyInstance);
	if (pvkEnumeratePhysicalDevices)
		pvkEnumeratePhysicalDevices(instance, &n, NULL);
	if (pvkDestroyInstance)
		pvkDestroyInstance(instance, NULL);
	return n != 0;
}

static void vn_put32(unsigned char* p, unsigned v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* secondload's synthetic BIOS: a display turned on and a GIF packet sent
 * down the DMA channel forever, so every vsync scans out an image. */
/* MIPS encodings for the reset program: t0 = 8, t1 = 9, t2 = 10. */
#define VN_LUI(rt, imm)       (0x3C000000u | ((rt) << 16) | ((imm) & 0xFFFFu))
#define VN_ORI(rt, rs, imm)   (0x34000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define VN_DSLL32(rd, rt)     (0x0000003Cu | ((rt) << 16) | ((rd) << 11))
#define VN_DSRL32(rd, rt)     (0x0000003Eu | ((rt) << 16) | ((rd) << 11))
#define VN_OR(rd, rs, rt)     (0x00000025u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define VN_SD(rt, off, base)  (0xFC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define VN_SW(rt, off, base)  (0xAC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define VN_LW(rt, off, base)  (0x8C000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define VN_ANDI(rt, rs, imm)  (0x30000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define VN_BNE(rs, rt, off)   (0x14000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define VN_B(off)             (0x10000000u | ((off) & 0xFFFFu))

static unsigned vn_emit(unsigned char* rom, unsigned pc, unsigned op)
{
	vn_put32(rom + pc, op);
	return pc + 4;
}

/* t1 = hi:lo, then store it at offset off from base. */
static unsigned vn_emit_store64(unsigned char* rom, unsigned pc, unsigned base,
	unsigned off, unsigned hi, unsigned lo)
{
	pc = vn_emit(rom, pc, VN_LUI(9, hi >> 16));
	pc = vn_emit(rom, pc, VN_ORI(9, 9, hi));
	pc = vn_emit(rom, pc, VN_DSLL32(9, 9));
	pc = vn_emit(rom, pc, VN_LUI(10, lo >> 16));
	pc = vn_emit(rom, pc, VN_ORI(10, 10, lo));
	pc = vn_emit(rom, pc, VN_DSLL32(10, 10));
	pc = vn_emit(rom, pc, VN_DSRL32(10, 10));
	pc = vn_emit(rom, pc, VN_OR(9, 9, 10));
	return vn_emit(rom, pc, VN_SD(9, off, base));
}

/* A 4MB image the core's BIOS scan accepts. Its reset program turns on a
 * 640x448 interlaced display of read circuit 1, writes a two-quadword GIF
 * packet (an A+D write of TEXFLUSH) to main RAM, and then sends it down
 * the GIF DMA channel forever, waiting for each transfer to end: every
 * vsync is scanned out and the GS ring never stops carrying packets,
 * which is what a BIOS screen does. The ROMDIR follows at 0x1000 and
 * ROMVER holds a USA console version string. */
static int vn_write_bios(const char* path)
{
	static unsigned char rom[4 * 1024 * 1024];
	FILE* f;
	unsigned pc, loop, wait;
	memset(rom, 0, sizeof(rom));
	pc = vn_emit(rom, 0, VN_LUI(8, 0xB200));                         /* t0 = GS privileged regs */
	pc = vn_emit_store64(rom, pc, 8, 0x00, 0, 0xFF25u);              /* PMODE: RC1, alpha FF    */
	pc = vn_emit_store64(rom, pc, 8, 0x20, 0, 1u);                   /* SMODE2: interlaced      */
	pc = vn_emit_store64(rom, pc, 8, 0x70, 0, 10u << 9);             /* DISPFB1: 640 wide       */
	pc = vn_emit_store64(rom, pc, 8, 0x80, 2559u | (447u << 12),     /* DISPLAY1                */
		636u | (50u << 12) | (3u << 23));
	pc = vn_emit(rom, pc, VN_LUI(11, 0xA010));                       /* t3 = RAM 0x100000       */
	pc = vn_emit_store64(rom, pc, 11, 0x00, 0x10000000u, 0x8001u);   /* GIFtag: NLOOP 1, EOP, NREG 1 */
	pc = vn_emit_store64(rom, pc, 11, 0x08, 0, 0xEu);                /* REGS: A+D               */
	pc = vn_emit_store64(rom, pc, 11, 0x10, 0, 0);                   /* data                    */
	pc = vn_emit_store64(rom, pc, 11, 0x18, 0, 0x3Fu);               /* address: TEXFLUSH       */
	pc = vn_emit(rom, pc, VN_LUI(10, 0xB000));                       /* D_CTRL = DMAE           */
	pc = vn_emit(rom, pc, VN_ORI(10, 10, 0xE000));
	pc = vn_emit(rom, pc, VN_ORI(9, 0, 1));
	pc = vn_emit(rom, pc, VN_SW(9, 0, 10));
	pc = vn_emit(rom, pc, VN_LUI(13, 0xB000));                       /* t5 = GIF DMA channel    */
	pc = vn_emit(rom, pc, VN_ORI(13, 13, 0xA000));
	pc = vn_emit(rom, pc, VN_LUI(12, 0x0010));                       /* t4 = MADR 0x100000      */
	pc = vn_emit(rom, pc, VN_ORI(14, 0, 2));                         /* t6 = QWC 2              */
	pc = vn_emit(rom, pc, VN_ORI(15, 0, 0x101));                     /* t7 = CHCR: DIR | STR    */
	loop = pc;
	pc = vn_emit(rom, pc, VN_SW(12, 0x10, 13));
	pc = vn_emit(rom, pc, VN_SW(14, 0x20, 13));
	pc = vn_emit(rom, pc, VN_SW(15, 0x00, 13));
	wait = pc;
	pc = vn_emit(rom, pc, VN_LW(9, 0, 13));
	pc = vn_emit(rom, pc, 0);
	pc = vn_emit(rom, pc, VN_ANDI(9, 9, 0x100));
	pc = vn_emit(rom, pc, VN_BNE(9, 0, (wait - (pc + 4)) >> 2));
	pc = vn_emit(rom, pc, 0);
	pc = vn_emit(rom, pc, VN_B((loop - (pc + 4)) >> 2));
	vn_emit(rom, pc, 0);
	memcpy(rom + 0x1000, "RESET", 5);  vn_put32(rom + 0x1000 + 12, 0x1000);
	memcpy(rom + 0x1010, "ROMDIR", 6); vn_put32(rom + 0x1010 + 12, 0x40);
	memcpy(rom + 0x1020, "ROMVER", 6); vn_put32(rom + 0x1020 + 12, 0x10);
	memcpy(rom + 0x1040, "0160AC20020207", 14);
	f = fopen(path, "wb");
	if (!f)
		return 0;
	if (fwrite(rom, 1, sizeof(rom), f) != sizeof(rom))
	{
		fclose(f);
		return 0;
	}
	return fclose(f) == 0;
}

static int vn_copy(const char *src, const char *dst)
{
	static unsigned char buf[1 << 16];
	size_t n;
	FILE *in = fopen(src, "rb"), *out;
	if (!in)
		return 0;
	out = fopen(dst, "wb");
	if (!out)
	{
		fclose(in);
		return 0;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	return fclose(out) == 0;
}

#define VN_FAIL(...) do { fprintf(stderr, "vknegotiate: " __VA_ARGS__); fputc('\n', stderr); return 1; } while (0)

int main(int argc, char** argv)
{
	char path[1100];
	VN_HANDLE h;
	retro_set_environment_t* set_environment;
	retro_set_video_refresh_t* set_video;
	retro_set_audio_sample_t* set_audio;
	retro_set_audio_sample_batch_t* set_audio_batch;
	retro_set_input_poll_t* set_poll;
	retro_set_input_state_t* set_input;
	retro_simple_t *init_fn, *deinit_fn, *unload_game, *run;
	retro_load_game_t* load_game;
	retro_serialize_size_t* serialize_size;
	retro_serialize_t* serialize;
	retro_unserialize_t* unserialize;
	const char *bios = getenv("LRPS2_BIOS");
	const VkApplicationInfo *app;
	VkInstanceCreateInfo ici;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice gpus[8];
	uint32_t ngpu = 8;
	struct retro_vulkan_context ctx;
	const char *v1_exts[1];
	VkPhysicalDeviceFeatures v1_features;
	bool ok;
	int frame;
	int frames = getenv("VN_FRAMES") ? atoi(getenv("VN_FRAMES")) : VN_FRAMES;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 4)
		VN_FAIL("usage: vknegotiate <path-to-core> <scratch-dir> <v2|v2retry|v1>");
	if      (!strcmp(argv[3], "v2"))      s_mode = VN_V2;
	else if (!strcmp(argv[3], "v2retry")) s_mode = VN_V2RETRY;
	else if (!strcmp(argv[3], "v1"))      s_mode = VN_V1;
	else VN_FAIL("unknown mode %s", argv[3]);
	if (strlen(argv[2]) + 48 > sizeof(s_system_dir))
		VN_FAIL("scratch path too long");
	if (!vn_vulkan_load() || !vn_vulkan_has_device())
	{
		printf("vknegotiate: skip: no Vulkan device\n");
		return 0;
	}

	strcpy(s_system_dir, argv[2]);
	sprintf(path, "%s/pcsx2", s_system_dir);
	vn_mkdir(path);
	sprintf(path, "%s/pcsx2/bios", s_system_dir);
	vn_mkdir(path);
	sprintf(path, "%s/pcsx2/bios/%s", s_system_dir, s_bios_name);
	if (bios ? !vn_copy(bios, path) : !vn_write_bios(path))
		VN_FAIL("cannot write %s", path);

	h = vn_dlopen(argv[1]);
	if (!h)
		VN_FAIL("cannot load %s", argv[1]);
	set_environment = (retro_set_environment_t*)vn_sym(h, "retro_set_environment");
	set_video       = (retro_set_video_refresh_t*)vn_sym(h, "retro_set_video_refresh");
	set_audio       = (retro_set_audio_sample_t*)vn_sym(h, "retro_set_audio_sample");
	set_audio_batch = (retro_set_audio_sample_batch_t*)vn_sym(h, "retro_set_audio_sample_batch");
	set_poll        = (retro_set_input_poll_t*)vn_sym(h, "retro_set_input_poll");
	set_input       = (retro_set_input_state_t*)vn_sym(h, "retro_set_input_state");
	init_fn         = (retro_simple_t*)vn_sym(h, "retro_init");
	deinit_fn       = (retro_simple_t*)vn_sym(h, "retro_deinit");
	unload_game     = (retro_simple_t*)vn_sym(h, "retro_unload_game");
	run             = (retro_simple_t*)vn_sym(h, "retro_run");
	load_game       = (retro_load_game_t*)vn_sym(h, "retro_load_game");
	serialize_size  = (retro_serialize_size_t*)vn_sym(h, "retro_serialize_size");
	serialize       = (retro_serialize_t*)vn_sym(h, "retro_serialize");
	unserialize     = (retro_unserialize_t*)vn_sym(h, "retro_unserialize");
	if (!set_environment || !set_video || !set_audio || !set_audio_batch || !set_poll ||
		!set_input || !init_fn || !deinit_fn || !unload_game || !run || !load_game
		|| !serialize_size || !serialize || !unserialize)
		VN_FAIL("core is missing a libretro export");

	set_environment(vn_environment);
	set_video(vn_video);
	set_audio(vn_audio);
	set_audio_batch(vn_audio_batch);
	set_poll(vn_poll);
	set_input(vn_input);
	init_fn();
	if (!load_game(NULL))
		VN_FAIL("retro_load_game failed");
	if (!s_iface || s_iface->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
		VN_FAIL("no Vulkan negotiation interface");
	if (s_iface->interface_version != (s_mode == VN_V1 ? 1u : 2u))
		VN_FAIL("interface version %u offered to a %s frontend", s_iface->interface_version, argv[3]);
	if (!s_iface->create_device || (s_mode != VN_V1 && !s_iface->create_device2))
		VN_FAIL("entry points missing");

	app = s_iface->get_application_info ? s_iface->get_application_info() : NULL;
	if (s_mode != VN_V1 && s_iface->create_instance)
	{
		/* v2: the core makes the instance through the wrapper. */
		instance = s_iface->create_instance(gipa, app, vn_instance_wrapper, NULL);
		if (instance == VK_NULL_HANDLE)
			VN_FAIL("create_instance failed");
	}
	else
	{
		memset(&ici, 0, sizeof(ici));
		ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		ici.pApplicationInfo = app;
		if (pvkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
			VN_FAIL("vkCreateInstance failed");
	}
	VN_INST(vkDestroyInstance);
	VN_INST(vkEnumeratePhysicalDevices);
	VN_INST(vkEnumerateDeviceExtensionProperties);
	VN_INST(vkCreateDevice);
	VN_INST(vkDestroyDevice);
	VN_INST(vkGetDeviceProcAddr);
	if (pvkEnumeratePhysicalDevices(instance, &ngpu, gpus) < 0 || ngpu == 0)
		VN_FAIL("no physical device");

	memset(&ctx, 0, sizeof(ctx));
	if (s_mode == VN_V1)
	{
		v1_exts[0] = VN_EXTRA_EXT;
		memset(&v1_features, 0, sizeof(v1_features));
		ok = s_iface->create_device(&ctx, instance, gpus[0], VK_NULL_HANDLE, gipa,
			v1_exts, 1, NULL, 0, &v1_features);
		if (!ok)
			VN_FAIL("create_device failed");
		if (s_wrapper_calls)
			VN_FAIL("v1 went through the v2 wrapper");
	}
	else
	{
		ok = s_iface->create_device2(&ctx, instance, gpus[0], VK_NULL_HANDLE, gipa,
			vn_device_wrapper, NULL);
		if (s_mode == VN_V2RETRY)
		{
			if (ok)
				VN_FAIL("create_device2 succeeded although its device creation failed");
			ok = s_iface->create_device2(&ctx, instance, VK_NULL_HANDLE, VK_NULL_HANDLE, gipa,
				vn_device_wrapper, NULL);
		}
		if (!ok)
			VN_FAIL("create_device2 failed");
		if (s_wrapper_calls != (s_mode == VN_V2RETRY ? 2 : 1))
			VN_FAIL("wrapper called %d times", s_wrapper_calls);
		if (ctx.device != s_wrapper_device)
			VN_FAIL("device is not the one the wrapper created");
		printf("  wrapper extension %s %s\n", VN_EXTRA_EXT,
			s_wrapper_extra ? "added" : "not supported here");
	}
	if (ctx.device == VK_NULL_HANDLE || ctx.queue == VK_NULL_HANDLE || ctx.gpu == VK_NULL_HANDLE)
		VN_FAIL("context not filled");
	pvkDeviceWaitIdle = (PFN_vkDeviceWaitIdle)pvkGetDeviceProcAddr(ctx.device, "vkDeviceWaitIdle");

	s_vk.interface_type         = RETRO_HW_RENDER_INTERFACE_VULKAN;
	s_vk.interface_version      = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
	s_vk.handle                 = &s_vk;
	s_vk.instance               = instance;
	s_vk.gpu                    = ctx.gpu;
	s_vk.device                 = ctx.device;
	s_vk.get_device_proc_addr   = pvkGetDeviceProcAddr;
	s_vk.get_instance_proc_addr = gipa;
	s_vk.queue                  = ctx.queue;
	s_vk.queue_index            = ctx.queue_family_index;
	s_vk.set_image              = vn_set_image;
	s_vk.get_sync_index         = vn_sync_index;
	s_vk.get_sync_index_mask    = vn_sync_mask;
	s_vk.set_command_buffers    = vn_set_cmd;
	s_vk.wait_sync_index        = vn_wait_sync;
	s_vk.lock_queue             = vn_lock;
	s_vk.unlock_queue           = vn_unlock;
	s_vk.set_signal_semaphore   = vn_set_sem;
	if (s_hw.context_reset)
		s_hw.context_reset();

	for (frame = 0; frame < frames; frame++)
		run();
	printf("  %d frames run, %u presented, %u duped, %u as images\n", frames, s_frames, s_dupes, s_images);
	/* A savestate round trip between two frames: the drain the GS takes
	 * for it, and the frames after it. */
	{
		size_t size = serialize_size();
		void* state = size ? malloc(size) : NULL;
		if (state && serialize(state, size))
		{
			for (frame = 0; frame < 10; frame++)
				run();
			if (!unserialize(state, size))
				VN_FAIL("retro_unserialize failed");
			for (frame = 0; frame < 30; frame++)
				run();
			printf("  savestate round trip: %u presented in all\n", s_frames);
		}
		else
			printf("  savestate: not available (%u bytes)\n", (unsigned)size);
		free(state);
	}
	if (s_queue_locked != 0)
		VN_FAIL("queue lock unbalanced (%d)", s_queue_locked);
	/* The synthetic BIOS draws nothing under the HW renderer; a real
	 * one boots to its menu, whose frames must arrive as images. */
	if (bios && s_images == 0)
		VN_FAIL("no frame reached the frontend as an image");

	/* RetroArch's order (core_unload_game): context, content, device. */
	if (s_hw.context_destroy)
		s_hw.context_destroy();
	unload_game();
	if (s_iface->destroy_device)
		s_iface->destroy_device();
	pvkDeviceWaitIdle(ctx.device);
	pvkDestroyDevice(ctx.device, NULL);
	pvkDestroyInstance(instance, NULL);
	deinit_fn();

	printf("vknegotiate %s: ok\n", argv[3]);
	return 0;
}
