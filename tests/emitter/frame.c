/* The Win64 frame generated code keeps around itself (c89frame.h) hands
 * xmm6-xmm15, which Win64 makes callee-saved, back to its caller as they
 * were, when the code inside overwrites every xmm register.
 *
 * The frame is built with XE_FRAME_WIN64 set, so it runs on any x86-64
 * host: the code is entered from inline asm that loads xmm6-xmm15 with a
 * pattern, calls it, and stores them back. build.sh also builds it with
 * the SysV frame, which keeps no xmm register, and requires that to fail:
 * it is what shows the check sees a clobber.
 *
 * GCC and Clang, x86-64; skipped elsewhere. C89 apart from the asm. */
#ifndef XE_FRAME_WIN64
#define XE_FRAME_WIN64 1
#endif

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#if defined(__x86_64__) && defined(__GNUC__)
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t s32; typedef intptr_t sptr; typedef uintptr_t uptr;
u8* x86Ptr;
#include "common/emitter/c89emit.h"
#include "common/emitter/c89ops.h"
#include "common/emitter/c89frame.h"

static void* exec_alloc(size_t size)
{
#ifdef _WIN32
	return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
	void* p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? NULL : p;
#endif
}

static void enter(void* fn, unsigned char* in, unsigned char* out)
{
	__asm__ volatile(
		"movups   0(%1), %%xmm6\n\t"
		"movups  16(%1), %%xmm7\n\t"
		"movups  32(%1), %%xmm8\n\t"
		"movups  48(%1), %%xmm9\n\t"
		"movups  64(%1), %%xmm10\n\t"
		"movups  80(%1), %%xmm11\n\t"
		"movups  96(%1), %%xmm12\n\t"
		"movups 112(%1), %%xmm13\n\t"
		"movups 128(%1), %%xmm14\n\t"
		"movups 144(%1), %%xmm15\n\t"
		"call *%0\n\t"
		"movups %%xmm6,    0(%2)\n\t"
		"movups %%xmm7,   16(%2)\n\t"
		"movups %%xmm8,   32(%2)\n\t"
		"movups %%xmm9,   48(%2)\n\t"
		"movups %%xmm10,  64(%2)\n\t"
		"movups %%xmm11,  80(%2)\n\t"
		"movups %%xmm12,  96(%2)\n\t"
		"movups %%xmm13, 112(%2)\n\t"
		"movups %%xmm14, 128(%2)\n\t"
		"movups %%xmm15, 144(%2)\n\t"
		:
		: "a"(fn), "b"(in), "c"(out)
		: "memory", "cc", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
		  "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
		  "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

int main(void)
{
	unsigned char in[160], out[160];
	u8* code = (u8*)exec_alloc(4096);
	int m_offset, i;

	if (!code)
	{
		printf("frame: no executable memory\n");
		return 1;
	}
	x86Ptr = code;
	SCOPED_STACK_FRAME_BEGIN(m_offset);
	for (i = 0; i < 16; i++)
		xe_pxor_xx(i, i);
	SCOPED_STACK_FRAME_END(m_offset);
	xe_ret();

	for (i = 0; i < 160; i++)
		in[i] = (unsigned char)(0xa5 ^ (i * 29));
	memset(out, 0, sizeof(out));
	enter(code, in, out);
	for (i = 0; i < 10; i++)
		if (memcmp(in + i * 16, out + i * 16, 16))
		{
			printf("frame: xmm%d comes back changed (%s frame)\n", 6 + i,
				XE_FRAME_WIN64 ? "Win64" : "SysV");
			return 1;
		}
	printf("frame: xmm6-xmm15 kept across code that overwrites them (%s frame)\n",
		XE_FRAME_WIN64 ? "Win64" : "SysV");
	return 0;
}
#else
int main(void)
{
	printf("frame: GCC or Clang on x86-64 only; skipped\n");
	return 0;
}
#endif
