/* The core's audio output: SPU2 on the EE thread produces, retro_run on
 * the frontend's thread hands it to the frontend. C89.
 *
 * Every vsync carries a mark - how many bytes SPU2 had produced when the
 * EE reached it - and retro_run uploads up to the mark of the frame it
 * presents, so each retro_run hands over exactly that frame's samples:
 * no more because the EE has already started on the next one, and none
 * lost between two frames. */
#ifndef LRPS2_AUDIO_QUEUE_H
#define LRPS2_AUDIO_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocates the queue once; later calls keep it and empty it. 0 when
 * the allocation fails, and then nothing is queued. */
int  audio_queue_init(void);
void audio_queue_free(void);

/* Producer, the EE thread. Reserve room for max_samples int16s and fill
 * as many as wanted, then commit the count actually written. */
int16_t *retro_audio_reserve(int32_t max_samples);
void     retro_audio_commit(int32_t samples);

/* Producer: the mark for a vsync, the bytes committed so far. */
uint32_t retro_audio_mark(void);

/* Consumer, the frontend's thread: hand the frontend every stereo frame
 * queued before mark. */
void audio_queue_upload(uint32_t mark, retro_audio_sample_batch_t batch);

/* Both sides stopped (the EE paused). Empties the queue. */
void audio_queue_discard(void);

/* Both sides stopped. What is queued and not yet handed over, for a
 * savestate: copies up to cap bytes into dst and returns how many, and
 * stores in *before_mark how many of them come before mark. */
size_t audio_queue_save(void *dst, size_t cap, uint32_t mark,
		uint32_t *before_mark);

/* Both sides stopped. Replaces what is queued with the bytes a savestate
 * carried, and returns the mark that falls before_mark bytes in. */
uint32_t audio_queue_load(const void *src, size_t bytes,
		uint32_t before_mark);

/* Samples dropped because the frontend stopped taking them for longer
 * than the queue holds. */
extern uint32_t audio_queue_dropped;

#ifdef __cplusplus
}
#endif

#endif
