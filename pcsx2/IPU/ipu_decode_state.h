/* What the slice decoders keep between calls besides ipu_cmd: the
 * macroblock address increment IDEC has run up, and whether IDEC and
 * BDEC have yet to take their pause before handing a macroblock to the
 * output FIFO. Defined in IPU_MultiISA.cpp, reset with the IPU, and
 * carried by the savestate's extension block (libretro/state_ext.h); a
 * state without one loads the values an idle IPU has. C. */
#ifndef IPU_DECODE_STATE_H
#define IPU_DECODE_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

struct ipu_decode_state
{
	int mba_count;
	int idec_ready;
	int bdec_ready;
};
extern struct ipu_decode_state ipu_decode;

#ifdef __cplusplus
}
#endif

#endif
