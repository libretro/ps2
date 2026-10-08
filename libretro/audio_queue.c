/* See audio_queue.h. C89.
 *
 * The queue is a retro_spsc byte ring: SPU2 mixes straight into it when
 * the contiguous run at the head is long enough, and into a staging array
 * that is copied in when it is not, which happens once per lap of the
 * ring. Every commit is a whole number of stereo frames, so the head only
 * moves by multiples of four bytes and every span is frame-aligned.
 *
 * Bounded: if the frontend stops taking audio for longer than the ring
 * holds, about 1.3 s, further samples are dropped and counted. */
#include <string.h>

#include <retro_spsc.h>

#include "audio_queue.h"

#define AUDIO_QUEUE_BYTES   (1 << 18) /* 256 KB, ~1.37 s stereo at 48 kHz */
#define AUDIO_STAGING_INT16 16384     /* above SPU2's largest reserve, ~9.6K */
#define AUDIO_FRAME_BYTES   ((size_t)(2 * sizeof(int16_t)))

static retro_spsc_t s_queue;
static int          s_queue_ok;

/* The producer's: the staging array, whether the reserve in flight is in
 * the ring or in staging, and the bytes committed so far. */
static int16_t      s_staging[AUDIO_STAGING_INT16];
static int          s_reserve_in_ring;
static uint32_t     s_written;

/* The consumer's: the bytes handed over so far. */
static uint32_t     s_read;

uint32_t audio_queue_dropped;

int audio_queue_init(void)
{
	if (!s_queue_ok)
		s_queue_ok = retro_spsc_init(&s_queue, AUDIO_QUEUE_BYTES) ? 1 : 0;
	else
		retro_spsc_clear(&s_queue);
	s_written           = 0;
	s_read              = 0;
	audio_queue_dropped = 0;
	return s_queue_ok;
}

void audio_queue_free(void)
{
	if (s_queue_ok)
	{
		retro_spsc_free(&s_queue);
		s_queue_ok = 0;
	}
}

int16_t *retro_audio_reserve(int32_t max_samples)
{
	void  *span;
	size_t span_bytes;
	const size_t need = (size_t)max_samples * sizeof(int16_t);

	if (max_samples > (int32_t)AUDIO_STAGING_INT16 || !s_queue_ok)
		return NULL;
	span_bytes = retro_spsc_write_begin(&s_queue, &span);
	if (span_bytes >= need)
	{
		s_reserve_in_ring = 1;
		return (int16_t*)span;
	}
	s_reserve_in_ring = 0;
	return s_staging;
}

void retro_audio_commit(int32_t samples)
{
	const size_t bytes = (size_t)samples * sizeof(int16_t);

	if (!samples)
	{
		if (s_reserve_in_ring)
			retro_spsc_write_end(&s_queue, 0);
		return;
	}
	if (s_reserve_in_ring)
	{
		retro_spsc_write_end(&s_queue, bytes);
		s_written += (uint32_t)bytes;
		return;
	}
	if (retro_spsc_write_avail(&s_queue) < bytes)
	{
		audio_queue_dropped += (uint32_t)samples;
		return;
	}
	retro_spsc_write(&s_queue, s_staging, bytes);
	s_written += (uint32_t)bytes;
}

uint32_t retro_audio_mark(void)
{
	return s_written;
}

void audio_queue_upload(uint32_t mark, retro_audio_sample_batch_t batch)
{
	const void *span;
	size_t      span_bytes;
	size_t      want;
	const int32_t ahead = (int32_t)(mark - s_read);

	/* A mark at or behind what was handed over has nothing left in it. */
	if (!s_queue_ok || ahead <= 0)
		return;
	want = (size_t)ahead;
	while (want >= AUDIO_FRAME_BYTES
	    && (span_bytes = retro_spsc_read_begin(&s_queue, &span)) >= AUDIO_FRAME_BYTES)
	{
		size_t take = span_bytes < want ? span_bytes : want;
		take &= ~(AUDIO_FRAME_BYTES - 1);
		batch((const int16_t*)span, take / AUDIO_FRAME_BYTES);
		retro_spsc_read_end(&s_queue, take);
		s_read += (uint32_t)take;
		want   -= take;
	}
}

void audio_queue_discard(void)
{
	if (s_queue_ok)
		retro_spsc_clear(&s_queue);
	s_read = s_written;
}

size_t audio_queue_save(void *dst, size_t cap, uint32_t mark,
		uint32_t *before_mark)
{
	size_t  n = 0;
	int32_t ahead;

	if (s_queue_ok)
	{
		n = retro_spsc_read_avail(&s_queue);
		if (n > cap)
			n = cap;
		n &= ~(AUDIO_FRAME_BYTES - 1);
		n  = retro_spsc_peek(&s_queue, dst, n);
	}
	ahead = (int32_t)(mark - s_read);
	if (ahead < 0)
		ahead = 0;
	if ((size_t)ahead > n)
		ahead = (int32_t)n;
	*before_mark = (uint32_t)ahead;
	return n;
}

uint32_t audio_queue_load(const void *src, size_t bytes,
		uint32_t before_mark)
{
	bytes &= ~(AUDIO_FRAME_BYTES - 1);
	s_read    = 0;
	s_written = 0;
	if (!s_queue_ok)
		return 0;
	retro_spsc_clear(&s_queue);
	if (bytes > retro_spsc_write_avail(&s_queue))
		bytes = retro_spsc_write_avail(&s_queue) & ~(AUDIO_FRAME_BYTES - 1);
	s_written = (uint32_t)retro_spsc_write(&s_queue, src, bytes);
	if (before_mark > s_written)
		before_mark = s_written;
	return before_mark;
}
