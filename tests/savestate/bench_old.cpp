/* SaveStateBase as it stood before the C89 conversion, reproduced so the
 * two can be timed against each other. Copied from origin/master's
 * SaveState.h and SaveState.cpp; do not "fix" anything here -- the point is
 * that it is what it was. */

#include <vector>
#include <cstring>
#include <ctime>

#include "bench_old.h"

class OldSaveStateBase
{
public:
	std::vector<u8>& m_memory;
	char  m_tagspace[32];
	int   m_idx = 0;
	bool  m_error = false;
	bool  m_is_saving;

	OldSaveStateBase(std::vector<u8>& memblock, bool is_saving)
		: m_memory(memblock) { m_is_saving = is_saving; }

	/* --codegen needs this to survive as a symbol to disassemble, so that
	 * build pins it out of line. --bench does not define the macro: the
	 * class then gets inlined into its own loop, which the C side across
	 * a TU boundary does not, and the comparison stays the conservative
	 * one rather than the flattering one. */
#ifdef BENCH_OLD_NOINLINE
	__attribute__((noinline))
#endif
	void FreezeMem(void* data, int size)
	{
		if (!size)
			return;

		if (m_is_saving)
		{
			const int new_size = m_idx + size;
			if ((u32)new_size > m_memory.size())
				m_memory.resize((u32)new_size);

			memcpy(&m_memory[m_idx], data, size);
			m_idx += size;
			return;
		}

		if (m_error)
		{
			memset(data, 0, size);
			return;
		}

		{
			const u8 * const src = &m_memory[m_idx];
			m_idx += size;
			memcpy(data, src, size);
		}
	}
};

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

extern "C" double bench_old_run(int reps, int nblocks, const int *sizes,
                                u8 *scratch)
{
	double t0 = now();

	for (int r = 0; r < reps; r++)
	{
		std::vector<u8> mem;

		OldSaveStateBase s(mem, true);
		for (int i = 0; i < nblocks; i++)
			s.FreezeMem(scratch, sizes[i]);

		/* The same bytes back out, over the same vector, so nothing
		 * but FreezeMem is being timed. */
		OldSaveStateBase l(mem, false);
		for (int i = 0; i < nblocks; i++)
			l.FreezeMem(scratch, sizes[i]);
	}

	return now() - t0;
}
