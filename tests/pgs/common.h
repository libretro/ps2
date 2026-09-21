/* Shared prelude for the paraLLEl-GS vertex harness: the real shared struct
 * layouts and the real register bit layouts, without the Vulkan stack. */
#include <cstdint>
typedef uint64_t VkDeviceAddress;
#include "shaders/data_structures.h"
#include "gs_registers.hpp"
using namespace ParallelGS;
struct Regs { Reg64<STBits> st; Reg64<RGBAQBits> rgbaq; Reg64<UVBits> uv; Reg64<FOGBits> fog; };
