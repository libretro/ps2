/* See state_ext.h. C89. */
#include <string.h>

#include "state_ext.h"
#include "audio_queue.h"
#include "../pcsx2/MTGSOwner.h"
#include "../pcsx2/IPU/ipu_decode_state.h"

/* Room for the queued audio a state carries: the pending frame's, at
 * most 960 stereo samples, and what the EE mixed after its vsync before
 * the pause took. More than that is queued only while the frontend is
 * not taking audio, and then the oldest is kept. */
#define STATE_EXT_AUDIO   16384

#define STATE_EXT_VERSION 1
#define STATE_EXT_MAGIC   "LRPS2EXT"

/* Offsets into the block. Every value is a 32-bit word in the host's
 * order, as in the rest of the state. */
#define EXT_REGS          0
#define EXT_WORDS         (EXT_REGS + sizeof(mtgs_pending.regs))
#define EXT_W_PENDING     0
#define EXT_W_FIELD       1
#define EXT_W_REGS_WRITTEN 2
#define EXT_W_AUDIO_BYTES 3
#define EXT_W_AUDIO_MARK  4
#define EXT_W_IPU_MBA     5
#define EXT_W_IPU_IDEC    6
#define EXT_W_IPU_BDEC    7
#define EXT_NWORDS        8
#define EXT_AUDIO         (EXT_WORDS + EXT_NWORDS * 4)
#define EXT_FOOTER        (EXT_AUDIO + STATE_EXT_AUDIO)
#define EXT_SIZE          (EXT_FOOTER + 16)

static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }

size_t state_ext_size(void)
{
	return EXT_SIZE;
}

void state_ext_write(void *dst)
{
	unsigned char *p = (unsigned char*)dst;
	unsigned char *w = p + EXT_WORDS;
	uint32_t before_mark;
	size_t   audio;

	memset(p, 0, EXT_SIZE);
	memcpy(p + EXT_REGS, mtgs_pending.regs, sizeof(mtgs_pending.regs));
	audio = audio_queue_save(p + EXT_AUDIO, STATE_EXT_AUDIO,
			mtgs_pending.audio_mark, &before_mark);

	put32(w + 4 * EXT_W_PENDING,      mtgs_pending.pending);
	put32(w + 4 * EXT_W_FIELD,        mtgs_pending.field);
	put32(w + 4 * EXT_W_REGS_WRITTEN, mtgs_pending.registers_written);
	put32(w + 4 * EXT_W_AUDIO_BYTES,  (uint32_t)audio);
	put32(w + 4 * EXT_W_AUDIO_MARK,   before_mark);
	put32(w + 4 * EXT_W_IPU_MBA,      (uint32_t)ipu_decode.mba_count);
	put32(w + 4 * EXT_W_IPU_IDEC,     (uint32_t)ipu_decode.idec_ready);
	put32(w + 4 * EXT_W_IPU_BDEC,     (uint32_t)ipu_decode.bdec_ready);

	put32(p + EXT_FOOTER,     (uint32_t)EXT_SIZE);
	put32(p + EXT_FOOTER + 4, STATE_EXT_VERSION);
	memcpy(p + EXT_FOOTER + 8, STATE_EXT_MAGIC, 8);
}

/* The block at the end of the state, or NULL. */
static const unsigned char *state_ext_find(const void *state, size_t size)
{
	const unsigned char *end = (const unsigned char*)state + size;

	if (size < EXT_SIZE)
		return NULL;
	if (memcmp(end - 8, STATE_EXT_MAGIC, 8)
	 || get32(end - 16) != EXT_SIZE
	 || get32(end - 12) != STATE_EXT_VERSION)
		return NULL;
	return end - EXT_SIZE;
}

size_t state_ext_strip(const void *state, size_t size)
{
	return state_ext_find(state, size) ? size - EXT_SIZE : size;
}

void state_ext_read(const void *state, size_t size)
{
	const unsigned char *p = state_ext_find(state, size);
	const unsigned char *w;
	uint32_t audio;

	if (!p)
	{
		mtgs_pending.pending  = 0;
		audio_queue_discard();
		ipu_decode.mba_count  = 0;
		ipu_decode.idec_ready = 1;
		ipu_decode.bdec_ready = 1;
		return;
	}
	w = p + EXT_WORDS;
	memcpy(mtgs_pending.regs, p + EXT_REGS, sizeof(mtgs_pending.regs));
	mtgs_pending.pending           = get32(w + 4 * EXT_W_PENDING) != 0;
	mtgs_pending.field             = get32(w + 4 * EXT_W_FIELD);
	mtgs_pending.registers_written = get32(w + 4 * EXT_W_REGS_WRITTEN);
	audio = get32(w + 4 * EXT_W_AUDIO_BYTES);
	if (audio > STATE_EXT_AUDIO)
		audio = STATE_EXT_AUDIO;
	mtgs_pending.audio_mark = audio_queue_load(p + EXT_AUDIO, audio,
			get32(w + 4 * EXT_W_AUDIO_MARK));
	ipu_decode.mba_count  = (int)get32(w + 4 * EXT_W_IPU_MBA);
	ipu_decode.idec_ready = get32(w + 4 * EXT_W_IPU_IDEC) != 0;
	ipu_decode.bdec_ready = get32(w + 4 * EXT_W_IPU_BDEC) != 0;
}
