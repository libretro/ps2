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

#if defined(__APPLE__) && defined(__aarch64__)
#define JIT_MMAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT)
#else
#define JIT_MMAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS)
#endif

#endif
