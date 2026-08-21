/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

// clang-format off

#ifdef __CYGWIN__
	#define __linux__
#endif

// make sure __POSIX__ is defined for all systems where we assume POSIX
// compliance
#if defined(__linux__) || defined(__APPLE__) || defined(__unix__) || defined(__CYGWIN__) || defined(__LINUX__)
	#ifndef __POSIX__
		#define __POSIX__ 1
	#endif
#endif

#include "Pcsx2Types.h"
#include <cstddef>
#include <cassert>

// The C++ standard doesn't allow `offsetof` to be used on non-constant values (e.g. `offsetof(class, field[i])`)
// Use this in those situations
#define OFFSETOF(a, b) (reinterpret_cast<size_t>(&(static_cast<a*>(0)->b)))

// Coarse CPU family selector used to pick SIMD backends (SSE vs NEON), e.g. the
// GSVector implementation. Mirrors the scheme used by the modern PCSX2/ARMSX2
// tree so the arm64 GSVector headers can be shared.
#if (defined(_M_ARM64) || defined(__aarch64__)) || defined(__aarch64__)
	#define ARCH_ARM64
#elif defined(_M_X86) || defined(__x86_64__) || defined(__i386__)
	#define ARCH_X86
#else
	#error Unsupported architecture
#endif

#if (defined(_M_ARM64) || defined(__aarch64__)) && defined(__APPLE__)
/* Apple Silicon uses 16KB pages and 128 byte cache lines. */
#define __pagesize 0x4000
#define __pageshift 14
#define __cachelinesize 128
#elif (defined(_M_ARM64) || defined(__aarch64__))
/* Linux aarch64 (this port's targets) uses 4KB pages; keep the conservative
   128-byte cache line. A 16K __pagesize here breaks vtlb page protection:
   the fault handler masks si_addr with a 16K mask, mis-attributing faults on
   a 4K kernel (addr can round below eeMem->Main) -> unhandled -> abort. */
#define __pagesize 0x1000
#define __pageshift 12
#define __cachelinesize 128
#else
// X86 uses a 4KB granularity and 64 byte cache lines.
#define __pagesize 0x1000
#define __pageshift 12
#define __cachelinesize 64
#endif
#define __pagemask (__pagesize - 1)

// We use 4KB alignment for globals for both Apple 
// and x86 platforms, since computing the
// address on ARM64 is a single instruction (adrp).
#define __pagealignsize 0x1000

// --------------------------------------------------------------------------------------
//  Microsoft Visual Studio
// --------------------------------------------------------------------------------------
// Initial-exec TLS model: one GOT load + fs/tpidr-relative access instead of
// a __tls_get_addr resolver call on every access (general-dynamic is the
// -fPIC default). Used for the emitter write cursors, which are touched
// multiple times per emitted instruction during recompilation bursts. Safe
// for a dlopen'd core: glibc reserves static-TLS surplus for exactly this.
// No-op on MSVC, whose TLS never goes through a resolver call.
#if defined(_MSC_VER)
	#define PCSX2_TLS_INITIAL_EXEC
#else
	#define PCSX2_TLS_INITIAL_EXEC __attribute__((tls_model("initial-exec")))
#endif

#ifdef _MSC_VER

#ifndef __noinline
#define __noinline __declspec(noinline)
#endif
#ifndef __noreturn
#define __noreturn __declspec(noreturn)
#endif

// Don't know if there are Visual C++ equivalents of these.
#define likely(x) (!!(x))
#define unlikely(x) (!!(x))

#ifndef CALLBACK
#define CALLBACK __stdcall
#endif

// Portable read-prefetch into L1.  GCC/Clang have __builtin_prefetch.
// MSVC routes through <intrin.h>: _mm_prefetch on x86, __prefetch on ARM64.
#include <intrin.h>
#if (defined(_M_ARM64) || defined(__aarch64__))
#define __prefetch_r(p) __prefetch((const void*)(p))
#else
#define __prefetch_r(p) _mm_prefetch((char const*)(p), _MM_HINT_T0)
#endif

#else

// --------------------------------------------------------------------------------------
//  GCC / Intel Compilers Section
// --------------------------------------------------------------------------------------

#define __assume(cond) do { if (!(cond)) __builtin_unreachable(); } while(0)

// Portable read-prefetch into L1.  See MSVC counterpart above.
#define __prefetch_r(p) __builtin_prefetch((const void*)(p))

// SysV ABI passes vector parameters through registers unconditionally.
#ifndef _WIN32
#define __vectorcall
#ifndef CALLBACK
#define CALLBACK
#endif
#else
// MinGW / clang do not implement __vectorcall.  The Win64 ABI already passes
// the first four FP/vector arguments through XMM registers, so dropping the
// attribute keeps the calling convention compatible with anything compiled
// in the same toolchain (which is what matters for the libretro core).
#if !defined(_MSC_VER)
#define __vectorcall
#endif
#ifndef CALLBACK
#define CALLBACK __attribute__((stdcall))
#endif
#endif

// Inlining note: GCC needs ((unused)) attributes defined 
// on inlined functions to suppress warnings when a static 
// inlined function isn't used in the scope of a single file (which
// happens *by design* like all the friggen time >_<)

#ifndef _inline
#define _inline __inline__ __attribute__((unused))
#endif

// __forceinline / __fi: the codebase applies these to NON-static, NON-inline
// function *definitions* in .cpp files, with `extern` declarations in headers.
//
// MSVC, Linux gcc, and macOS clang all handle `__forceinline` correctly:
// the always_inline attribute is applied at every callsite AND an out-of-line
// copy is still emitted for the cross-TU callers.  On those toolchains the
// historical decoration is what we want and yields the inlining gains the
// hot paths (SPU2, vtlb, x86Emitter, dmaSIF...) were designed around.
//
// mingw-w64 is the odd one out.  Its `_mingw.h` defines `__forceinline` as
// `extern __inline__ __attribute__((__always_inline__,__gnu_inline__))`,
// which under GNU inline rules means "inline at every callsite AND DO NOT
// emit an out-of-line copy".  That last part breaks the libretro non-LTO
// Makefile build: every cross-TU call (e.g. dmaSIF1, vtlb_GetPhyPtr,
// xPUSH, SPU2 Mix/TimeUpdate/spu2M_Write/UpdateSpdifMode...)
// becomes an undefined reference.  cmake builds avoid this via PCSX2_LTO=ON
// merging all TUs at link time, but the libretro Makefile builds do not LTO.
//
// We must NOT override `__forceinline` on mingw - the system headers
// (winbase.h, processthreadsapi.h, synchapi.h, _mingw.h) declare and
// define many functions like strnlen_s / _InterlockedIncrement /
// NtCurrentTeb as `__forceinline ...` and rely on the gnu_inline
// semantics for one-definition-rule compliance across TUs.  Redefining
// `__forceinline` to empty there would cause "multiple definition of
// strnlen_s" link errors.
//
// Instead, leave `__forceinline` alone everywhere, and bind the project's
// `__fi` / `__ri` / `__releaseinline` decorations to empty ONLY on mingw.
// On every other toolchain they keep their historical meaning of
// `__forceinline`, preserving the inlining and the perf characteristics
// that working PCSX2 builds depend on.
#ifdef NDEBUG
#ifndef __forceinline
#define __forceinline __attribute__((always_inline, unused))
#endif
#else
#ifndef __forceinline
#define __forceinline __attribute__((unused))
#endif
#endif

#ifndef __noinline
#define __noinline __attribute__((noinline))
#endif

#ifndef __noreturn
#define __noreturn __attribute__((noreturn))
#endif

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#endif

// --------------------------------------------------------------------------------------
// __releaseinline / __ri -- a forceinline macro that is enabled for RELEASE/PUBLIC builds ONLY.
// --------------------------------------------------------------------------------------
// This is useful because forceinline can make certain types of debugging problematic since
// functions that look like they should be called won't breakpoint since their code is
// inlined, and it can make stack traces confusing or near useless.
//
// Use __releaseinline for things which are generally large functions where trace debugging
// from Devel builds is likely useful; but which should be inlined in an optimized Release
// environment.
//
// On mingw these expand to nothing; see the `__forceinline` comment block
// above for the gnu_inline rationale.  Everywhere else they keep their
// historical meaning of `__forceinline`.
#ifdef __MINGW32__
#define __releaseinline
#define __ri
#define __fi
#else
#define __releaseinline __forceinline
#define __ri __releaseinline
#define __fi __forceinline
#endif

// __forceinline_odr is for inline functions defined in headers (e.g. the
// GSVertex/quad helpers). MSVC needs __forceinline; GCC/Clang need a plain
// `inline` alongside the always_inline attribute for ODR correctness across
// translation units. On MinGW we follow the same gnu_inline rationale as
// __fi/__ri above and let it collapse to `inline`.
// Fallthrough annotation that degrades instead of failing to parse:
// [[fallthrough]] is C++17 syntax and nothing older -- not C89, not C at
// all, not pre-17 C++ -- so a bare attribute is a landmine for any file
// that migrates toward C or an older standard. GCC>=7 and clang accept
// the attribute spelling in C too; everything else gets a no-op, which
// costs only the warning the annotation was silencing.
#ifndef PCSX2_FALLTHROUGH
#if defined(__cplusplus) && (__cplusplus >= 201703L)
#define PCSX2_FALLTHROUGH [[fallthrough]]
#elif defined(__GNUC__) && (__GNUC__ >= 7)
#define PCSX2_FALLTHROUGH __attribute__((fallthrough))
#elif defined(__clang__)
#define PCSX2_FALLTHROUGH __attribute__((fallthrough))
#else
#define PCSX2_FALLTHROUGH ((void)0)
#endif
#endif

#ifndef __forceinline_odr
#if defined(_MSC_VER)
// MSVC's __forceinline is a builtin that already carries inline linkage;
// spelling `inline` beside it is warning C4141 in every including TU --
// seven per inclusion of GSVertex.h alone. Forced inlining that needs a
// per-compiler alphabet of spellings, one of which warns and another of
// which is only a suggestion the optimizer may ignore, is exactly the
// guarantee problem function-style inlining has always had; a macro would
// not need any of this. Where that trade matters most in this codebase
// (the emitter core), macros are what we use.
#define __forceinline_odr __forceinline
#elif defined(__MINGW32__)
#define __forceinline_odr inline
#else
#define __forceinline_odr __forceinline inline
#endif
#endif

// Makes sure that if anyone includes xbyak, it doesn't do anything bad
#define XBYAK_ENABLE_OMITTED_OPERAND

#if defined(__x86_64__) && !defined(_M_AMD64)
	#define _M_AMD64
#endif

#ifndef RESTRICT
	#ifdef __INTEL_COMPILER
		#define RESTRICT restrict
	#elif defined(_MSC_VER)
		#define RESTRICT __restrict
	#elif defined(__GNUC__)
		#define RESTRICT __restrict__
	#else
		#define RESTRICT
	#endif
#endif

#ifndef __has_attribute
	#define __has_attribute(x) 0
#endif

#ifndef __has_builtin
	#define __has_builtin(x) 0
#endif

#ifdef __cpp_constinit
	#define CONSTINIT constinit
#elif __has_attribute(require_constant_initialization)
	#define CONSTINIT __attribute__((require_constant_initialization))
#else
	#define CONSTINIT
#endif

#ifdef __cplusplus

// --------------------------------------------------------------------------------------
//  ImplementEnumOperators  (macro)
// --------------------------------------------------------------------------------------
// This macro implements ++/-- operators for any conforming enumeration.  In order for an
// enum to conform, it must have _FIRST and _COUNT members defined, and must have a full
// compliment of sequential members (no custom assignments) --- looking like so:
//
// enum Dummy {
//    Dummy_FIRST,
//    Dummy_Item = Dummy_FIRST,
//    Dummy_Crap,
//    Dummy_COUNT
// };
//
// The macro also defines utility functions for bounds checking enumerations:
//   EnumIsValid(value);   // returns TRUE if the enum value is between FIRST and COUNT.
//   EnumAssert(value);
//
// It also defines a *prototype* for converting the enumeration to a string.  Note that this
// method is not implemented!  You must implement it yourself if you want to use it:
//   EnumToString(value);
//
#define ImplementEnumOperators(enumName) \
	static __fi enumName& operator++(enumName& src) \
	{ \
		src = (enumName)((int)src + 1); \
		return src; \
	} \
\
	static __fi enumName& operator--(enumName& src) \
	{ \
		src = (enumName)((int)src - 1); \
		return src; \
	} \
\
	static __fi enumName operator++(enumName& src, int) \
	{ \
		enumName orig = src; \
		src = (enumName)((int)src + 1); \
		return orig; \
	} \
\
	static __fi enumName operator--(enumName& src, int) \
	{ \
		enumName orig = src; \
		src = (enumName)((int)src - 1); \
		return orig; \
	} \
\
	static __fi bool operator<(const enumName& left, const pxEnumEnd_t&) { return (int)left < enumName##_COUNT; } \
	static __fi bool operator!=(const enumName& left, const pxEnumEnd_t&) { return (int)left != enumName##_COUNT; } \
	static __fi bool operator==(const enumName& left, const pxEnumEnd_t&) { return (int)left == enumName##_COUNT; } \
\
	static __fi bool EnumIsValid(enumName id) \
	{ \
		return ((int)id >= enumName##_FIRST) && ((int)id < enumName##_COUNT); \
	} \
\
	extern const char* EnumToString(enumName id)

class pxEnumEnd_t
{
};
static const pxEnumEnd_t pxEnumEnd = {};

// --------------------------------------------------------------------------------------
//  DeclareNoncopyableObject
// --------------------------------------------------------------------------------------
// This macro provides an easy and clean method for ensuring objects are not copyable.
// Simply add the macro to the head or tail of your class declaration, and attempts to
// copy the class will give you a moderately obtuse compiler error.
//
#ifndef DeclareNoncopyableObject
#define DeclareNoncopyableObject(classname) \
public: \
	classname(const classname&) = delete; \
	classname& operator=(const classname&) = delete
#endif

#endif

// --------------------------------------------------------------------------------------
//  Handy Human-readable constants for common immediate values (_16kb -> _4gb)

static constexpr sptr _1kb = 1024 * 1;
static constexpr sptr _4kb = _1kb * 4;
static constexpr sptr _16kb = _1kb * 16;
static constexpr sptr _32kb = _1kb * 32;
static constexpr sptr _64kb = _1kb * 64;
static constexpr sptr _128kb = _1kb * 128;
static constexpr sptr _256kb = _1kb * 256;

static constexpr s64 _1mb = 1024 * 1024;
static constexpr s64 _8mb = _1mb * 8;
static constexpr s64 _16mb = _1mb * 16;
static constexpr s64 _32mb = _1mb * 32;
static constexpr s64 _64mb = _1mb * 64;
static constexpr s64 _256mb = _1mb * 256;
static constexpr s64 _1gb = _1mb * 1024;
static constexpr s64 _4gb = _1gb * 4;

// Disable some spammy warnings which wx appeared to disable.
// We probably should fix these at some point.
#ifdef _MSC_VER
#pragma warning(disable: 4244) // warning C4244: 'initializing': conversion from 'uptr' to 'uint', possible loss of data
#pragma warning(disable: 4267) // warning C4267: 'initializing': conversion from 'size_t' to 'uint', possible loss of data
#endif
