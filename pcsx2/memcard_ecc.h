/* PS2 memory card images with and without ECC. C89.
 *
 * A raw card image is pages of 528 bytes: 512 of data, then the ECC of
 * each 128-byte quarter as three bytes (column parity, then the two line
 * parities), then four zero bytes. A .bin image is the data alone. The
 * card is run from a raw image, so a .bin is converted to raw when it is
 * opened and back when it is closed. */
#ifndef LRPS2_MEMCARD_ECC_H
#define LRPS2_MEMCARD_ECC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEMCARD_PAGE_DATA 512
#define MEMCARD_PAGE_RAW  528

/* The ECC of 128 bytes: column parity in bits 0-7, the line parities in
 * bits 8-15 and 16-23. */
uint32_t memcard_ecc(const uint8_t *buf);

/* Writes file_out as the raw image of the .bin image file_in, whole pages
 * only. Returns 1 on success, 0 on failure; both files are closed either
 * way, and file_out may be left partly written. */
int memcard_noecc_to_raw(const char *file_in, const char *file_out);

/* Writes file_out as the .bin image of the raw image file_in, whole pages
 * only. Returns as memcard_noecc_to_raw. */
int memcard_raw_to_noecc(const char *file_in, const char *file_out);

#ifdef __cplusplus
}
#endif

#endif
