/* The savestate's extension block: what a state needs beyond the
 * emulator's own blocks so that loading it and running on gives the
 * frames, audio and input timing that running on from where it was taken
 * gives. C89.
 *
 * It carries the frame a drain left for the next retro_run (MTGSOwner.h),
 * the audio queued and not yet handed to the frontend with where that
 * frame's audio ends (audio_queue.h), and the IPU slice decoders' state
 * outside ipu_cmd (ipu_decode_state.h).
 *
 * It is the last thing in the state and ends in a footer - its size, a
 * version and a magic - so a state is recognised as having one from its
 * last bytes, and a state written before it existed loads as it did,
 * with the block's contents set to an idle machine's. Fixed size, so
 * retro_serialize_size stays the same from frame to frame. */
#ifndef LRPS2_STATE_EXT_H
#define LRPS2_STATE_EXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes the block takes, footer included. */
size_t state_ext_size(void);

/* Writes the block, state_ext_size() bytes, at dst. EE paused. */
void state_ext_write(void *dst);

/* The size of the state without its extension block: size itself when
 * the state has none. */
size_t state_ext_strip(const void *state, size_t size);

/* Loads the block at the end of a state of size bytes, or an idle
 * machine's values when it has none. EE paused. */
void state_ext_read(const void *state, size_t size);

#ifdef __cplusplus
}
#endif

#endif
