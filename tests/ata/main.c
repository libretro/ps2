/* The DEV9 HDD's IO thread handshake, on the real ATA code.
 *
 * The EE side queues sector writes and has the IO thread flush them,
 * starts asynchronous reads and collects them from ata_async(), and
 * reads synchronously, which withdraws any pending write and waits for
 * the IO thread to be idle before it seeks the image itself. Both
 * threads seek the one RFILE, so a write the IO thread starts during a
 * synchronous read moves the read's file position under it.
 *
 * The image has two halves. The first takes only writes, the second is
 * a fixed pattern that only reads touch, so any read that comes back
 * different saw the IO thread's seek or write mid-read. After each
 * round the device is closed with writes still queued, which the IO
 * thread must flush before it exits, and the first half is checked
 * against the last write queued for every sector.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <streams/file_stream.h>

#include "DEV9/ATA/ATA.h"

#define REGION_SECTORS 256
#define IMAGE_SECTORS  (REGION_SECTORS * 2)
#define MAX_RUN        8
#define ROUNDS         6
#define OPS_PER_ROUND  3000

static void harness_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	if (level < RETRO_LOG_WARN)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

retro_log_printf_t log_cb = harness_log;

void _DEV9irq(int cause, int cycles) { (void)cause; (void)cycles; }
void dev9_irq_cause_clear(int bits) { (void)bits; }
int dev9_ata_dma_enabled(void) { return 0; }

static unsigned rng_state = 12345u;

static unsigned rng(void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return (rng_state >> 8) & 0xffffffu;
}

static uint8_t fixed_byte(unsigned sector, unsigned i)
{
	return (uint8_t)(((sector * 131u) + (i * 7u)) ^ 0x5au);
}

static uint8_t written_byte(unsigned stamp, unsigned i)
{
	/* stamp 0 is the image's initial contents, an all-zero sector. A
	 * zero sector also exercises the sparse path where the file
	 * system can punch holes. */
	if (stamp == 0 || (stamp % 5u) == 0)
		return 0;
	return (uint8_t)((stamp * 29u) + i);
}

/* Last stamp queued for each sector of the write half. */
static unsigned model[REGION_SECTORS];
static unsigned stamp_next = 1;

static unsigned failures;
static unsigned reads_checked;
static int      pending_lba;
static int      pending_count;
static int      async_done;

static void fail(const char* what, int lba, int i)
{
	if (failures++ < 8)
		fprintf(stderr, "  FAIL: %s at sector %d byte %d\n", what, lba, i);
}

static void check_fixed(ata_state_t* ata, int lba, int count)
{
	int s, i;
	for (s = 0; s < count; s++)
	{
		const uint8_t* p = ata->readBuffer + s * 512;
		for (i = 0; i < 512; i++)
		{
			if (p[i] != fixed_byte((unsigned)(lba + s), (unsigned)i))
			{
				fail("read of the fixed half came back changed", lba + s, i);
				return;
			}
		}
	}
	reads_checked++;
}

static void on_async_read(ata_state_t* ata)
{
	check_fixed(ata, pending_lba, pending_count);
	async_done = 1;
}

static void on_sync_read(ata_state_t* ata)
{
	check_fixed(ata, pending_lba, pending_count);
}

static void set_read(ata_state_t* ata)
{
	pending_count = 1 + (int)(rng() % MAX_RUN);
	pending_lba   = REGION_SECTORS + (int)(rng() % (REGION_SECTORS - MAX_RUN));
	ata->regSelect = 0x40;
	ata->lba48     = false;
	ata_hdd_set_lba(ata, pending_lba);
	ata->nsector   = pending_count;
}

static void queue_write(ata_state_t* ata)
{
	ata_write_entry_t entry;
	int count = 1 + (int)(rng() % MAX_RUN);
	int lba   = (int)(rng() % (REGION_SECTORS - MAX_RUN));
	int s, i;

	entry.data   = (uint8_t*)malloc((size_t)count * 512);
	entry.sector = (uint64_t)lba;
	entry.length = (uint32_t)count * 512;
	if (!entry.data)
		abort();
	for (s = 0; s < count; s++)
	{
		unsigned stamp = stamp_next++;
		for (i = 0; i < 512; i++)
			entry.data[s * 512 + i] = written_byte(stamp, (unsigned)i);
		model[lba + s] = stamp;
	}
	ata_write_queue_enqueue(&ata->writeQueue, &entry);
}

static void tick(ata_state_t* ata)
{
	ata->regStatus = ATA_STAT_READY;
	ata_async(ata, 0);
}

static void op(ata_state_t* ata)
{
	switch (rng() % 4)
	{
		case 0:
		case 1:
			queue_write(ata);
			tick(ata);
			break;
		case 2:
			set_read(ata);
			ata_hdd_read_sync(ata, on_sync_read);
			tick(ata);
			break;
		default:
			set_read(ata);
			async_done = 0;
			ata_hdd_read_async(ata, on_async_read);
			while (!async_done)
			{
				tick(ata);
				sthread_yield();
			}
			break;
	}
}

static int make_image(const char* path)
{
	uint8_t sector[512];
	unsigned s, i;
	FILE* f = fopen(path, "wb");
	if (!f)
		return 0;
	memset(sector, 0, sizeof(sector));
	for (s = 0; s < REGION_SECTORS; s++)
		fwrite(sector, 1, sizeof(sector), f);
	for (s = REGION_SECTORS; s < IMAGE_SECTORS; s++)
	{
		for (i = 0; i < 512; i++)
			sector[i] = fixed_byte(s, i);
		fwrite(sector, 1, sizeof(sector), f);
	}
	fclose(f);
	return 1;
}

static void check_image(const char* path, int round)
{
	uint8_t sector[512];
	unsigned s, i;
	FILE* f = fopen(path, "rb");
	if (!f)
	{
		fail("image could not be reopened", -1, -1);
		return;
	}
	for (s = 0; s < IMAGE_SECTORS; s++)
	{
		if (fread(sector, 1, sizeof(sector), f) != sizeof(sector))
		{
			fail("image came back short", (int)s, -1);
			break;
		}
		for (i = 0; i < 512; i++)
		{
			uint8_t want = s < REGION_SECTORS ? written_byte(model[s], i) : fixed_byte(s, i);
			if (sector[i] != want)
			{
				fprintf(stderr, "  round %d:", round);
				fail(s < REGION_SECTORS ? "queued write did not land" : "fixed half was overwritten",
					(int)s, (int)i);
				s = IMAGE_SECTORS;
				break;
			}
		}
	}
	fclose(f);
}

int main(void)
{
	const char* path = "ata_test.img";
	int round, n;

	if (!make_image(path))
	{
		fprintf(stderr, "  FAIL: could not create %s\n", path);
		return 1;
	}

	for (round = 0; round < ROUNDS; round++)
	{
		ata_state_t* ata = ata_new();
		if (!ata || ata_open(ata, path, IMAGE_SECTORS) != 0)
		{
			fprintf(stderr, "  FAIL: ata_open\n");
			return 1;
		}

		for (n = 0; n < OPS_PER_ROUND; n++)
			op(ata);

		/* Close with writes still queued: the IO thread flushes them
		 * on its way out. */
		for (n = 0; n < 16; n++)
			queue_write(ata);
		ata_free(ata);

		check_image(path, round);
	}

	remove(path);

	if (failures)
	{
		fprintf(stderr, "  %u failures\n", failures);
		return 1;
	}
	printf("  ok: %u reads unchanged across %d rounds, every queued write landed\n",
		reads_checked, ROUNDS);
	return 0;
}
