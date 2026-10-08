/* Brings equal items of an array together, in place: each group in the
 * order its first item appears, and the items of a group in the order
 * they came. Equality is same(a, b), which must be an equivalence. The
 * order depends only on the array and on same - never on the values of
 * pointers it compares - so a sequence of draws grouped this way keeps
 * its order from run to run. Quadratic in the worst case, for the short
 * lists of copies it groups. C89. */
#ifndef GS_STABLE_GROUP_H
#define GS_STABLE_GROUP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void gs_stable_group(void* items, size_t count, size_t size,
	int (*same)(const void* a, const void* b));

#ifdef __cplusplus
}
#endif

#endif
