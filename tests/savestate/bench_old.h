/* Declaration of the reference run. The old class itself is C++, so it
 * lives in bench_old.cpp; only this line crosses into the C benchmark. */

#ifndef SAVESTATE_BENCH_OLD_H
#define SAVESTATE_BENCH_OLD_H

#include "../../common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

double bench_old_run(int reps, int nblocks, const int *sizes, u8 *scratch);

#ifdef __cplusplus
}
#endif

#endif
