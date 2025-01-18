#include "../pcsx2/USB/USB.h"
#include "../pcsx2/SaveState.h"

void USB::CheckForConfigChanges(const Pcsx2Config& old_config) { }
void USBconfigure(void) {}
void USBinit(void) { }
void USBasync(u32 cycles) {}
void USBshutdown(void) { }
void USBclose(void) {}
void USBreset(void) {}
bool USBopen(void) { return true; }
s32 USBfreeze(FreezeAction mode, freezeData* data) { return 0; }
u8 USBread8(u32 addr) { return 0; }
u16 USBread16(u32 addr) { return 0; }
u32 USBread32(u32 addr) { return 0; }
void USBwrite8(u32 addr, u8 value) {}
void USBwrite16(u32 addr, u16 value) {}
void USBwrite32(u32 addr, u32 value) {}
