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

#if defined(_M_ARM64)
/* Apple Silicon uses 16KB pages and 128 byte cache lines. */
#define __pagesize 0x4000
#define __pageshift 14
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

#else

// --------------------------------------------------------------------------------------
//  GCC / Intel Compilers Section
// --------------------------------------------------------------------------------------

#define __assume(cond) do { if (!(cond)) __builtin_unreachable(); } while(0)

// SysV ABI passes vector parameters through registers unconditionally.
#ifndef _WIN32
#define __vectorcall
#ifndef CALLBACK
#define CALLBACK
#endif
#else
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
#define __releaseinline __forceinline

#define __ri __releaseinline
#define __fi __forceinline

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
