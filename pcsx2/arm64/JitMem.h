/*
 * JitMem - code-cache allocation for the arm64 recompilers.
 *
 * Apple Silicon refuses a plain PROT_READ|PROT_WRITE|PROT_EXEC mapping
 * (EACCES, even with the allow-unsigned-executable-memory entitlement):
 * code memory must be mapped with MAP_JIT, and it is then write-xor-execute
 * per thread, switched with memjit_write_begin/end (pthread_jit_write_protect_np).
 * Everywhere else MAP_JIT is 0 and the begin/end calls do nothing.
 */

#ifndef PCSX2_ARM64_JITMEM_H
#define PCSX2_ARM64_JITMEM_H

#include <sys/mman.h>
#include <string.h>

#if defined(__APPLE__) && defined(__aarch64__)
#define JIT_MMAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT)
#else
#define JIT_MMAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS)
#endif

/* Zero a private anonymous mapping, cheaply, whatever the pages held.
 * madvise(MADV_DONTNEED) does that on Linux, but on Darwin it is only a hint
 * and the old contents stay -- a block LUT "cleared" that way keeps pointing
 * into code cache that is about to be reused. Mapping fresh zero pages over
 * the range is the same cost there. */
static inline void jit_zero_pages(void* p, size_t size)
{
#if defined(__APPLE__)
	if (mmap(p, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
		memset(p, 0, size);
#else
	madvise(p, size, MADV_DONTNEED);
#endif
}

#endif
