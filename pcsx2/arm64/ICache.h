// SPDX-FileCopyrightText: lrps2 arm64 port
// SPDX-License-Identifier: LGPL-3.0+
//
// Instruction cache flush for freshly emitted code.

#pragma once

#include <cstddef>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

// clang lowers __builtin___clear_cache() to a call to ___clear_cache on iOS and
// tvOS - a compiler-rt symbol those SDKs do not ship, so the link ends in
// "Undefined symbols for architecture arm64". sys_icache_invalidate() is
// Apple's own call and what the builtin becomes on macOS anyway.
static inline void ClearICacheRange(void* start, std::size_t size)
{
#if defined(__APPLE__)
	sys_icache_invalidate(start, size);
#else
	char* const p = static_cast<char*>(start);
	__builtin___clear_cache(p, p + size);
#endif
}
