/*
 * Which render targets and depth buffers the hardware renderer drops when
 * the ones it holds are over their memory budget.
 *
 * A target's texture is the only copy of what the game drew there: GS
 * memory is not written back as it draws. A target whose every block the
 * game has written since by a transfer, or one out of use that a newer
 * target has been drawn over, holds nothing the game still reads, and
 * goes first. Past those, a target the game has not used for a frame goes, the
 * longest unused first: a game that makes large targets quickly leaves
 * them behind within a frame or two. A target used in the frame just
 * ended is the game's working set - the picture on screen, the one being
 * drawn, the last frame a blur blends back in - and stays whatever the
 * budget: the game reads it again without drawing it first.
 */

#ifndef GS_TARGET_BUDGET_H
#define GS_TARGET_BUDGET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gs_target_info
{
   uint64_t bytes;           /* its texture's memory                    */
   int age;                  /* frames since last used; 0 or 1 is live  */
   unsigned char superseded; /* written over since, as above            */
} gs_target_info_t;

/* Fills `order` with the indices of the targets to drop, in the order to
 * drop them, until `usage` bytes fit `budget`, and gives how many. Stops
 * short of the budget when what is left is live. */
unsigned gs_target_evict_order(const gs_target_info_t *t, unsigned n,
      uint64_t usage, uint64_t budget, unsigned *order);

#ifdef __cplusplus
}
#endif

#endif
