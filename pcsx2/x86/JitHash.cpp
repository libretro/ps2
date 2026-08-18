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

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)

#include "Memory.h"
#include "VirtualMemory.h"

#include <cstdio>
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

extern "C" __attribute__((visibility("default"))) void pcsx2_jithash_dump(void)
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
	fflush(stderr);
}

#endif // x86-64
