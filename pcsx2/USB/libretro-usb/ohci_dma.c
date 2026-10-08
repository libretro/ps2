/* See ohci_dma.h. C89. */
#include <string.h>

#include "ohci_dma.h"

static int ohci_dma_part(uint8_t* ram, uint32_t ram_size, uint32_t addr,
	uint8_t* buf, uint32_t len, int write)
{
	if (!OHCI_DMA_FITS(addr, len, ram_size))
		return 1;
	if (write)
		memcpy(ram + addr, buf, len);
	else
		memcpy(buf, ram + addr, len);
	return 0;
}

int ohci_dma_copy(uint8_t* ram, uint32_t ram_size, uint32_t cbp, uint32_t be,
	uint8_t* buf, uint32_t len, int write)
{
	const uint32_t first_room = 0x1000u - (cbp & 0xfffu);
	const uint32_t n = len < first_room ? len : first_room;

	if (ohci_dma_part(ram, ram_size, cbp, buf, n, write))
		return 1;
	if (n == len)
		return 0;
	return ohci_dma_part(ram, ram_size, be & ~0xfffu, buf + n, len - n, write);
}
