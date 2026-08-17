// Stub layer: satisfies the IPU objects' externs without the emulator core.
#include <cstring>
#include <limits.h>
#include "Common.h"
#include "IPU/IPU.h"
#include "IPU/IPUdma.h"

alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware];
alignas(16) cpuRegisters cpuRegs;

void hwIntcIrq(int n) { (void)n; }
void CPU_INT(EE_EventType n, s32 c) { (void)n; (void)c; }
void ipuDmaReset(void) {}
bool SaveStateBase::FreezeTag(const char* src) { (void)src; return true; }
