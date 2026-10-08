/* The OHCI controller's DMA to and from IOP RAM. C89.
 *
 * A transfer descriptor names its buffer by the first byte (cbp) and the
 * last (be). The buffer may cross one 4 KB page boundary, and the part
 * past it continues from the start of be's page, wherever that is: the
 * first part runs from cbp to the end of its page, the rest from be's
 * page. Every address is checked against the size of RAM as a range,
 * never as a sum that can wrap. */
#ifndef OHCI_DMA_H
#define OHCI_DMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whether bytes at addr fit inside a memory of size bytes. */
#define OHCI_DMA_FITS(addr, bytes, size) \
	((uint32_t)(addr) <= (uint32_t)(size) && (uint32_t)(bytes) <= (uint32_t)(size) - (uint32_t)(addr))

/* Copies len bytes between buf and the descriptor's buffer in ram:
 * into ram when write is nonzero, out of it otherwise. 0 when done, 1
 * when a part lies outside ram, in which case nothing past that part is
 * copied. */
int ohci_dma_copy(uint8_t* ram, uint32_t ram_size, uint32_t cbp, uint32_t be,
	uint8_t* buf, uint32_t len, int write);

#ifdef __cplusplus
}
#endif

#endif
