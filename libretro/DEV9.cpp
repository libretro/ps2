#include "../pcsx2/DEV9/DEV9.h"

// Our IRQ call.
void (*DEV9irq)(int);

void DEV9configure()
{
}

s32 DEV9init()
{
    Console.WriteLn("Initializing dev9null");
    return 0;
}

void DEV9shutdown()
{
    Console.WriteLn("Shutting down Dev9null.");
}

s32 DEV9open()
{
    Console.WriteLn("Opening Dev9null.");
    // Get anything ready we need to. Opening and creating hard
    // drive files, for example.
    return 0;
}

void DEV9close()
{
    Console.WriteLn("Closing Dev9null.");
    // Close files opened.
}

u8 DEV9read8(u32 addr)
{
    u8 value = 0;

    switch (addr) {
        //        case 0x1F80146E:		// DEV9 hardware type (0x32 for an expansion bay)
        case 0x10000038: /*value = dev9Ru8(addr);*/
            break;       // We need to have at least one case to avoid warnings.
        default:
            break;
    }
    return value;
}

u16 DEV9read16(u32 addr)
{
    u16 value = 0;

    switch (addr) {
        // Addresses you may want to catch here include:
        //			case 0x1F80146E:		// DEV9 hardware type (0x32 for an expansion bay)
        //			case 0x10000002:		// The Smart Chip revision. Should be 0x11
        //			case 0x10000004:		// More type info: bit 0 - smap; bit 1 - hd; bit 5 - flash
        //			case 0x1000000E:		// Similar to the last; bit 1 should be set if a hd is hooked up.
        //			case 0x10000028:			// intr_stat
        //			case 0x10000038:			// hard drives seem to like reading and writing the max dma size per transfer here.
        //			case 0x1000002A:			// intr_mask
        //			case 0x10000040:			// pio_data
        //			case 0x10000044:			// nsector
        //			case 0x10000046:			// sector
        //			case 0x10000048:			// lcyl
        //			case 0x1000004A:			// hcyl
        //			case 0x1000004C:			// select
        //			case 0x1000004E:			// status
        //			case 0x1000005C:			// status
        //			case 0x10000064:			// if_ctrl
        case 0x10000038: /*value = dev9Ru16(addr);*/
            break;
        default:
            break;
    }

    return value;
}

u32 DEV9read32(u32 addr)
{
    u32 value = 0;

    switch (addr) {
        case 0x10000038: /*value = dev9Ru32(addr);*/
            break;
        default:
            break;
    }

    return value;
}

void DEV9write8(u32 addr, u8 value)
{
    switch (addr) {
        case 0x10000038: /*dev9Ru8(addr) = value;*/
            break;
        default:
            break;
    }
}

void DEV9write16(u32 addr, u16 value)
{
    switch (addr) {
        // Remember that list on DEV9read16? You'll want to write to a
        // lot of them, too.
        case 0x10000038: /*dev9Ru16(addr) = value;*/
            break;
        default:
            break;
    }
}

void DEV9write32(u32 addr, u32 value)
{
    switch (addr) {
        case 0x10000038: /*dev9Ru32(addr) = value;*/
            break;
        default:
            break;
    }
}

s32 DEV9dmaRead(s32 channel, u32 *data, u32 bytesLeft, u32 *bytesProcessed)
{
    // You'll want to put your own DMA8 reading code here.
    // Time to interact with your fake (or real) hardware.
    Console.WriteLn("Reading DMA8 Mem.");
    *bytesProcessed = bytesLeft;
    return 0;
}

s32 DEV9dmaWrite(s32 channel, u32 *data, u32 bytesLeft, u32 *bytesProcessed)
{
    // See above.
    Console.WriteLn("Writing DMA8 Mem.");
    *bytesProcessed = bytesLeft;
    return 0;
}

void DEV9dmaInterrupt(s32 channel)
{
    // See above.
}

void DEV9readDMA8Mem(u32 *pMem, int size)
{
    // You'll want to put your own DMA8 reading code here.
    // Time to interact with your fake (or real) hardware.
    Console.WriteLn("Reading DMA8 Mem.");
}

void DEV9writeDMA8Mem(u32 *pMem, int size)
{
    // See above.
    Console.WriteLn("Writing DMA8 Mem.");
}

int DEV9irqHandler(void)
{
    return 0;
}

void DEV9setSettingsDir(const char *dir)
{
}

void DEV9async(u32 cycles)
{
}

void DEV9CheckChanges(const Pcsx2Config& old_config)
{
}
