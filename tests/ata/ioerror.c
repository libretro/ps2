/* The DEV9 HDD when the host fails it, on the real ATA code: an image
 * that cannot be read or written ends the guest's command with an ATA
 * error rather than taking the process down.
 *
 *   - a read, synchronous or asynchronous, of sectors the host cannot
 *     read ends with ERR and an uncorrectable-data error, and the data
 *     phase never starts;
 *   - a queued write the host cannot write is reported by the flush that
 *     follows - ERR, write fault and abort - and only by that flush: the
 *     next flush, its writes landed, reports nothing;
 *   - reads and writes work again once the host does.
 * The image handle is swapped under the device for one the host cannot
 * use: a copy of the image cut short for the reads, the image opened
 * read-only for the writes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <streams/file_stream.h>

#include "DEV9/ATA/ATA.h"

#define IMAGE_SECTORS 64
#define SHORT_SECTORS 8

static void harness_log(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}

retro_log_printf_t log_cb = harness_log;

void _DEV9irq(int cause, int cycles) { (void)cause; (void)cycles; }
void dev9_irq_cause_clear(int bits) { (void)bits; }
int dev9_ata_dma_enabled(void) { return 0; }

static unsigned failures;
static int data_phase;

static void check(int ok, const char* what)
{
	if (!ok)
	{
		fprintf(stderr, "  FAIL: %s\n", what);
		failures++;
	}
}

static int make_image(const char* path, unsigned sectors)
{
	uint8_t sector[512];
	unsigned s;
	FILE* f = fopen(path, "wb");
	if (!f)
		return 0;
	for (s = 0; s < sectors; s++)
	{
		memset(sector, (int)(s + 1), sizeof(sector));
		fwrite(sector, 1, sizeof(sector), f);
	}
	return fclose(f) == 0;
}

static void on_data(ata_state_t* ata)
{
	(void)ata;
	data_phase = 1;
}

/* A command starts as ata_pre_cmd leaves the registers. */
static void start(ata_state_t* ata, int lba, int count)
{
	ata->regStatus = ATA_STAT_READY | ATA_STAT_BUSY;
	ata->regError  = 0;
	ata->regSelect = 0x40;
	ata->lba48     = false;
	ata_hdd_set_lba(ata, lba);
	ata->nsector   = count;
	data_phase     = 0;
}

static void swap_image(ata_state_t* ata, const char* path, unsigned mode)
{
	filestream_close(ata->hddImage);
	ata->hddImage = filestream_open(path, mode, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!ata->hddImage)
	{
		fprintf(stderr, "  FAIL: cannot reopen %s\n", path);
		exit(1);
	}
}

static void async_read(ata_state_t* ata)
{
	ata_hdd_read_async(ata, on_data);
	while (retro_atomic_load_acquire_int(&ata->ioRead) || ata->waitingCmd)
	{
		ata_async(ata, 0);
		sthread_yield();
	}
}

static void queue_write(ata_state_t* ata, int lba, uint8_t fill)
{
	ata_write_entry_t entry;
	entry.data = (uint8_t*)malloc(512);
	if (!entry.data)
		abort();
	memset(entry.data, fill, 512);
	entry.sector = (uint64_t)lba;
	entry.length = 512;
	ata_write_queue_enqueue(&ata->writeQueue, &entry);
}

/* The flush command's wait: until the queue is written and it completes. */
static void flush(ata_state_t* ata)
{
	ata->regStatus  = ATA_STAT_READY | ATA_STAT_BUSY;
	ata->regError   = 0;
	ata->awaitFlush = true;
	while (ata->awaitFlush)
	{
		ata_async(ata, 0);
		sthread_yield();
	}
}

static int read_back(const char* path, int lba)
{
	uint8_t b = 0;
	FILE* f = fopen(path, "rb");
	if (!f)
		return -1;
	fseek(f, (long)lba * 512, SEEK_SET);
	if (fread(&b, 1, 1, f) != 1)
		b = 0;
	fclose(f);
	return b;
}

int main(void)
{
	const char* path  = "ata_ioerror.img";
	const char* spath = "ata_ioerror_short.img";
	ata_state_t* ata;
	const int far = 40; /* inside the device, past the short copy */

	if (!make_image(path, IMAGE_SECTORS) || !make_image(spath, SHORT_SECTORS))
	{
		fprintf(stderr, "  FAIL: cannot create the images\n");
		return 1;
	}
	ata = ata_new();
	if (!ata || ata_open(ata, path, IMAGE_SECTORS) != 0)
	{
		fprintf(stderr, "  FAIL: ata_open\n");
		return 1;
	}

	/* Reads the host cannot serve. */
	swap_image(ata, spath, RETRO_VFS_FILE_ACCESS_READ);
	start(ata, far, 4);
	ata_hdd_read_sync(ata, on_data);
	check(!data_phase, "a synchronous read the host failed started its data phase");
	check((ata->regStatus & ATA_STAT_ERR) && (ata->regError & ATA_ERR_ECC),
			"a synchronous read the host failed ended without an uncorrectable-data error");
	check(!(ata->regStatus & ATA_STAT_BUSY), "a synchronous read the host failed did not complete");

	start(ata, far, 4);
	async_read(ata);
	check(!data_phase, "an asynchronous read the host failed started its data phase");
	check((ata->regStatus & ATA_STAT_ERR) && (ata->regError & ATA_ERR_ECC),
			"an asynchronous read the host failed ended without an uncorrectable-data error");
	check(!(ata->regStatus & ATA_STAT_BUSY), "an asynchronous read the host failed did not complete");

	/* And served again. */
	swap_image(ata, path, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING);
	start(ata, far, 4);
	async_read(ata);
	check(data_phase && !(ata->regStatus & ATA_STAT_ERR) && ata->readBuffer[0] == far + 1,
			"a read after the host recovered did not complete with the sector");

	/* Writes the host cannot make. */
	swap_image(ata, path, RETRO_VFS_FILE_ACCESS_READ);
	queue_write(ata, 3, 0xa5);
	flush(ata);
	check((ata->regStatus & (ATA_STAT_ERR | ATA_STAT_WRERR)) == (ATA_STAT_ERR | ATA_STAT_WRERR)
			&& (ata->regError & ATA_ERR_ABORT),
			"a flush after a write the host failed reported no error");

	/* And made again: the next flush reports nothing. */
	swap_image(ata, path, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING);
	queue_write(ata, 5, 0x5a);
	flush(ata);
	check(!(ata->regStatus & ATA_STAT_ERR) && !ata->regError,
			"a flush after the host recovered reported the earlier failure");

	ata_free(ata);
	check(read_back(path, 5) == 0x5a, "a write after the host recovered did not land");
	check(read_back(path, 3) == 4, "a write the host failed changed the image");
	remove(path);
	remove(spath);

	if (failures)
	{
		fprintf(stderr, "  %u failures\n", failures);
		return 1;
	}
	printf("  ok: host read and write failures end the guest's commands with ATA errors\n");
	return 0;
}
