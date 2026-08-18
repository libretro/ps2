// JIT emission oracle.
//
// The call-site migration from the C++ emitter API to the C89 macro API can
// only be trusted if each converted file provably emits the same bytes. JIT
// emission for a fixed content trace is deterministic, and every code cache
// lives at a fixed offset inside the code arena (HostMemoryMap), so hashing
// each region after a fixed number of frames gives a per-cache fingerprint:
// run the same trace on two builds, and any divergence in emitted code --
// including patched displacements, since this hashes final bytes rather than
// an emission stream -- moves at least one hash.
//
// Enabled at runtime by PCSX2_JITHASH=1 in the environment; prints one line
// per region to stderr at shutdown. Zero cost when the variable is unset.

#include "common/Pcsx2Defs.h"

#include <cstdio>
#include <cstdlib>

// The exported entry point and the A/B mode flags exist on every
// architecture and compiler: libretro/main.cpp calls the dump from
// retro_deinit unconditionally, and the aarch64 link failed the moment
// this TU compiled empty there. Only the hashing internals are x86-64.
#if defined(_MSC_VER)
#define JITHASH_EXPORT extern "C" __declspec(dllexport)
#else
#define JITHASH_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)

#include "Memory.h"
#include "VirtualMemory.h"
#include <cstdlib>

namespace
{
	struct JitRegion
	{
		const char* name;
		u32 offset;
		u32 size;
	};

	// Offsets and sizes per HostMemoryMap and the Assign() calls in the
	// respective recompilers. Sizes are the reserve sizes, not high-water
	// marks: unwritten arena pages read as zero, so hashing the full
	// reserve is deterministic and needs no per-cache cursor plumbing.
	const JitRegion s_regions[] = {
		{"EErec", HostMemoryMap::EErecOffset, 64 * _1mb},
		{"IOPrec", HostMemoryMap::IOPrecOffset, 32 * _1mb},
		{"VIF0rec", HostMemoryMap::VIF0recOffset, 8 * _1mb},
		{"VIF1rec", HostMemoryMap::VIF1recOffset, 8 * _1mb},
		{"mVU0rec", HostMemoryMap::mVU0recOffset, 64 * _1mb},
		{"mVU1rec", HostMemoryMap::mVU1recOffset, 64 * _1mb},
		{"VIFUnpackRec", HostMemoryMap::VIFUnpackRecOffset, _1mb},
	};
} // namespace

// Selects the C++ twin inside converted xe_* macros; see c89ops.h. Read
// once at load so the choice is in place before any block compiles.
extern "C" int xe_cpp_mode;
int xe_cpp_mode;
extern "C" unsigned long long xe_site_hits;
unsigned long long xe_site_hits;
static struct XeModeInit
{
	XeModeInit()
	{
		const char* e = getenv("PCSX2_XE_CPP");
		xe_cpp_mode = (e && e[0] == '1') ? 1 : 0;
	}
} s_xe_mode_init;

JITHASH_EXPORT void pcsx2_jithash_dump(void)
{
	const char* env = getenv("PCSX2_JITHASH");
	if (!env || env[0] == '0')
		return;

	const u8* base = GetVmMemory().CodeMemory()->GetBase();
	if (!base)
		return;

	for (const JitRegion& r : s_regions)
	{
		// FNV-1a 64, plus a count of nonzero bytes as an independent
		// sensitivity check (a hash collision and an equal nonzero count
		// together are not a plausible accident).
		u64 h = 14695981039346656037ull;
		u64 nz = 0;
		const u8* p = base + r.offset;
		for (u32 i = 0; i < r.size; i++)
		{
			h = (h ^ p[i]) * 1099511628211ull;
			nz += (p[i] != 0);
		}
		fprintf(stderr, "[JITHASH] %-12s %016llx nz=%llu\n",
			r.name, (unsigned long long)h, (unsigned long long)nz);
	}
	// Coverage guard for the A/B methodology: identical hashes prove
	// nothing if the trace never reached a converted site. Both modes
	// count, so this line must be nonzero AND equal across modes.
	fprintf(stderr, "[JITHASH] xe_site_hits %llu\n", xe_site_hits);
	fflush(stderr);
}

#else // not x86-64: keep the contract, hash nothing

extern "C" int xe_cpp_mode;
int xe_cpp_mode;
extern "C" unsigned long long xe_site_hits;
unsigned long long xe_site_hits;

JITHASH_EXPORT void pcsx2_jithash_dump(void)
{
}

#endif // x86-64
