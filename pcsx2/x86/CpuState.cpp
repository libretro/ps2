#include "common/Pcsx2Types.h"
#include "R5900.h"
#include "R3000A.h"

// These were objects with a reference alias over them (cpuRegs_ plus
// cpuRegs& = cpuRegs_). The aliases had no consumer -- nothing outside this
// file ever named the underlying objects -- but every access through one cost
// an extra dependent load, because a reference at namespace scope is a
// pointer in memory that has to be read before the member offset can be
// applied. Defining them directly removes that load from ~2,900 access sites.
alignas(16) cpuRegisters cpuRegs;
alignas(16) fpuRegisters fpuRegs;
alignas(16) psxRegisters psxRegs;
bool iopIsDelaySlot; /* declared bool in R3000A.h (outside pcsx2/x86) */
