/* The SMAP's receive path, on the real net.cpp and smap.cpp.
 *
 * The host adapter's recv runs on net.cpp's RX thread and returns
 * numbered frames of every length up to the Ethernet maximum, with an
 * empty poll now and then. This thread is the EE: it ticks smap_async,
 * which hands the SMAP what it has room for, and models the IOP driver
 * through the SMAP's own registers - read a frame's buffer descriptor,
 * read the frame out of the RX FIFO, give the descriptor back, and
 * decrement the frame count - pausing every so often so the FIFO and
 * the descriptors fill and frames wait.
 *
 * Every frame must arrive once, in order, with its length and bytes
 * intact, each delivery must raise RXEND, and under TSan the RX thread
 * must touch nothing the EE reads: the SMAP's state belongs to the
 * thread that emulates it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include <libretro.h>
#include <rthreads/rthreads.h>

#define MAX_FRAME   1514
#define MIN_FRAME   60
#define TIME_LIMIT  120

/* glue.cpp */
int smaprx_rxend_irqs(void);
int smaprx_bd_count(void);
unsigned smaprx_bd_empty_flag(void);
void smaprx_open(void);
void smaprx_close(void);
void smaprx_tick(void);
unsigned smaprx_frame_count(void);
unsigned smaprx_bd_read(int bd, int word);
void smaprx_bd_write(int bd, int word, unsigned v);
unsigned smaprx_fifo_read32(void);
void smaprx_frame_dec(void);

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

static unsigned total_frames;

static int frame_len(unsigned seq)
{
	return MIN_FRAME + (int)((seq * 97u) % (unsigned)(MAX_FRAME - MIN_FRAME + 1));
}

static unsigned char frame_byte(unsigned seq, int i)
{
	if (i < 4)
		return (unsigned char)(seq >> (8 * i));
	return (unsigned char)((seq * 31u) + (unsigned)i * 7u);
}

/* The host, on the RX thread: only it touches these. */
static unsigned host_next;
static unsigned host_polls;

int smaprx_host_recv(char* buf, int max)
{
	int len, i;
	if (host_next >= total_frames || (++host_polls % 7u) == 0)
		return 0;
	len = frame_len(host_next);
	if (len > max)
		return 0;
	for (i = 0; i < len; i++)
		buf[i] = (char)frame_byte(host_next, i);
	host_next++;
	return len;
}

static unsigned failures;

static void fail(const char* what, unsigned seq)
{
	if (failures++ < 8)
		fprintf(stderr, "  FAIL: %s (frame %u)\n", what, seq);
}

int main(int argc, char** argv)
{
	static unsigned char frame[MAX_FRAME + 4];
	const unsigned empty = smaprx_bd_empty_flag();
	const int bd_count = smaprx_bd_count();
	unsigned expect = 0;
	unsigned long iter = 0;
	int bd = 0;
	time_t start;

	total_frames = (argc > 1) ? (unsigned)atoi(argv[1]) : 20000u;

	smaprx_open();
	start = time(NULL);

	while (expect < total_frames && failures == 0)
	{
		smaprx_tick();
		iter++;

		/* The driver is busy elsewhere: the FIFO and the descriptors
		 * fill, and frames wait in the ring. */
		if ((iter % 64u) < 12u)
		{
			sthread_yield();
			continue;
		}

		while (smaprx_frame_count() > 0 && failures == 0)
		{
			unsigned ctrl = smaprx_bd_read(bd, 0);
			int len = (int)smaprx_bd_read(bd, 2);
			int w, i;

			if (ctrl & empty)
			{
				fail("frame counted but its descriptor is still empty", expect);
				break;
			}
			if (len != frame_len(expect))
			{
				fail("frame arrived with the wrong length", expect);
				break;
			}
			for (w = 0; w < (len + 3) / 4; w++)
			{
				unsigned v = smaprx_fifo_read32();
				memcpy(frame + w * 4, &v, 4);
			}
			for (i = 0; i < len; i++)
			{
				if (frame[i] != frame_byte(expect, i))
				{
					fail("frame arrived out of order or changed", expect);
					break;
				}
			}
			smaprx_bd_write(bd, 0, empty);
			smaprx_frame_dec();
			bd = (bd + 1) % bd_count;
			expect++;
		}

		if (time(NULL) - start > TIME_LIMIT)
		{
			fail("timed out waiting for frames", expect);
			break;
		}
		sthread_yield();
	}

	smaprx_close();

	if (smaprx_rxend_irqs() == 0)
		fail("no RXEND was raised", expect);

	if (failures)
	{
		fprintf(stderr, "  %u failures, %u of %u frames received\n", failures, expect, total_frames);
		return 1;
	}
	printf("  ok: %u frames received in order and intact, %d RXEND interrupts\n",
		expect, smaprx_rxend_irqs());
	return 0;
}
