/* ohci_dma_copy (ohci_dma.h), the USB controller's DMA into IOP RAM, on
 * a 2 MB RAM whose every byte is checked afterwards:
 *   - a buffer inside one page moves exactly its bytes;
 *   - one crossing a page moves the first part to the end of cbp's page
 *     and the rest to the start of be's page, and nothing else;
 *   - both directions;
 *   - a buffer past the end of RAM, or at an address near 4 GB whose sum
 *     with the length wraps, moves nothing and reports it;
 *   - a second page outside RAM leaves the first part moved and reports.
 * Built with ASan where the compiler has it, so a copy outside the RAM
 * block is caught as well. C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "USB/libretro-usb/ohci_dma.h"

#define RAM_SIZE (2u * 1024u * 1024u)

static unsigned char* ram;
static unsigned char* want;
static int failures;

static void reset_ram(void)
{
	memset(ram, 0x11, RAM_SIZE);
	memset(want, 0x11, RAM_SIZE);
}

static void expect(int got, int ret, const char* what)
{
	if (got != ret)
	{
		printf("  FAIL: %s: returned %d, wanted %d\n", what, got, ret);
		failures++;
	}
	if (memcmp(ram, want, RAM_SIZE))
	{
		size_t i;
		for (i = 0; i < RAM_SIZE && ram[i] == want[i]; i++)
			;
		printf("  FAIL: %s: RAM differs first at %06lx\n", what, (unsigned long)i);
		failures++;
	}
}

int main(void)
{
	unsigned char buf[0x2000], back[0x2000];
	unsigned i;
	int r;

	ram  = (unsigned char*)malloc(RAM_SIZE);
	want = (unsigned char*)malloc(RAM_SIZE);
	if (!ram || !want)
		return 1;
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (unsigned char)(i * 7 + 3);

	/* Inside one page. */
	reset_ram();
	memcpy(want + 0x10100, buf, 0x200);
	r = ohci_dma_copy(ram, RAM_SIZE, 0x10100, 0x102ff, buf, 0x200, 1);
	expect(r, 0, "a write inside one page");
	memset(back, 0, sizeof(back));
	r = ohci_dma_copy(ram, RAM_SIZE, 0x10100, 0x102ff, back, 0x200, 0);
	expect(r, 0, "a read inside one page");
	if (memcmp(back, buf, 0x200))
	{
		printf("  FAIL: a read inside one page brought back other bytes\n");
		failures++;
	}

	/* Across a page: 0x100 bytes to the end of 0x20000's page, the rest
	 * from the start of be's page, which is elsewhere. */
	reset_ram();
	memcpy(want + 0x20f00, buf, 0x100);
	memcpy(want + 0x35000, buf + 0x100, 0x300);
	r = ohci_dma_copy(ram, RAM_SIZE, 0x20f00, 0x352ff, buf, 0x400, 1);
	expect(r, 0, "a write across a page");
	memset(back, 0, sizeof(back));
	r = ohci_dma_copy(ram, RAM_SIZE, 0x20f00, 0x352ff, back, 0x400, 0);
	expect(r, 0, "a read across a page");
	if (memcmp(back, buf, 0x400))
	{
		printf("  FAIL: a read across a page brought back other bytes\n");
		failures++;
	}

	/* The last 0x10 bytes of RAM, then a page past its end: the part
	 * inside moves, the rest does not. */
	reset_ram();
	memcpy(want + RAM_SIZE - 0x10, buf, 0x10);
	r = ohci_dma_copy(ram, RAM_SIZE, RAM_SIZE - 0x10, RAM_SIZE + 0x10, buf, 0x20, 1);
	expect(r, 1, "a write running off the end of RAM");

	/* Past the end of RAM, and wrapping past 4 GB: nothing moves. */
	reset_ram();
	r = ohci_dma_copy(ram, RAM_SIZE, RAM_SIZE + 0x100, RAM_SIZE + 0x1ff, buf, 0x100, 1);
	expect(r, 1, "a write past the end of RAM");
	r = ohci_dma_copy(ram, RAM_SIZE, 0xffffff00u, 0xffffffffu, buf, 0x100, 1);
	expect(r, 1, "a write at an address whose end wraps");
	r = ohci_dma_copy(ram, RAM_SIZE, 0xffffff00u, 0xffffffffu, back, 0x100, 0);
	expect(r, 1, "a read at an address whose end wraps");

	/* The second page outside RAM: the first part moves, then it stops. */
	reset_ram();
	memcpy(want + 0x30f80, buf, 0x80);
	r = ohci_dma_copy(ram, RAM_SIZE, 0x30f80, 0xfff000ffu, buf, 0x180, 1);
	expect(r, 1, "a write whose second page lies outside RAM");

	free(ram);
	free(want);
	printf(failures ? "ohcidma: FAILED (%d)\n" : "ohcidma: ok\n", failures);
	return failures != 0;
}
