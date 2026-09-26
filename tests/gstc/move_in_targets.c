/* A local-memory move worked out on the targets, element by element.
 *
 * GSTextureCache::MoveInTargets does a move between parts of 32-bit
 * targets without local memory: every pixel of a transfer format is a run
 * of nibbles in one 32-bit word, a 32-bit target holds a word per pixel,
 * so the move is a remap, each destination element naming the target
 * pixel and nibble it takes. ps_move_texels applies it to each sample of
 * the target on its own, so every sample moves as its own memory would.
 *
 * Pinned here against the GS's own addressing (PSMCT32, PSMZ32 and PSMT4
 * block and column tables), for a depth buffer moved into a colour one as
 * PSMZ32 -> PSMCT32, then shuffled in place by PSMT4 column moves (the
 * effect this serves reads the result as an 8-bit index of the depth):
 *
 *  1. The remap, built as the texture cache builds it and applied as the
 *     shader applies it to every sample of a 2x target, leaves each sample
 *     holding what its own memory holds after the same moves done one
 *     pixel at a time.
 *  2. After the chain, bits 24-31 of every word are bits 16-23 of the depth
 *     at that pixel: sample for sample, the index is the depth the sample
 *     was drawn at, so a depth test and a lookup on that index agree.
 *
 * Negative control: a remap that takes each element from the source word's
 * first nibble, the place a whole-word move takes it from, fails 1.
 *
 * Build and run, from tests/gstc:
 *   cc -O2 -std=c89 -pedantic -Wall move_in_targets.c -o move_in_targets
 *   ./move_in_targets
 */
#include <stdio.h>
#include <string.h>

typedef unsigned int u32;

static const unsigned char block32[4][8] = {
   { 0, 1, 4, 5, 16, 17, 20, 21 },
   { 2, 3, 6, 7, 18, 19, 22, 23 },
   { 8, 9, 12, 13, 24, 25, 28, 29 },
   { 10, 11, 14, 15, 26, 27, 30, 31 }
};

static const unsigned char column32[8][8] = {
   { 0, 1, 4, 5, 8, 9, 12, 13 },
   { 2, 3, 6, 7, 10, 11, 14, 15 },
   { 16, 17, 20, 21, 24, 25, 28, 29 },
   { 18, 19, 22, 23, 26, 27, 30, 31 },
   { 32, 33, 36, 37, 40, 41, 44, 45 },
   { 34, 35, 38, 39, 42, 43, 46, 47 },
   { 48, 49, 52, 53, 56, 57, 60, 61 },
   { 50, 51, 54, 55, 58, 59, 62, 63 }
};

static const unsigned char block4[8][4] = {
   { 0, 2, 8, 10 },
   { 1, 3, 9, 11 },
   { 4, 6, 12, 14 },
   { 5, 7, 13, 15 },
   { 16, 18, 24, 26 },
   { 17, 19, 25, 27 },
   { 20, 22, 28, 30 },
   { 21, 23, 29, 31 }
};

static const unsigned short column4[16][32] = {
   {   0,   8,  32,  40,  64,  72,  96, 104,   2,  10,  34,  42,  66,  74,  98, 106,   4,  12,  36,  44,  68,  76, 100, 108,   6,  14,  38,  46,  70,  78, 102, 110 },
   {  16,  24,  48,  56,  80,  88, 112, 120,  18,  26,  50,  58,  82,  90, 114, 122,  20,  28,  52,  60,  84,  92, 116, 124,  22,  30,  54,  62,  86,  94, 118, 126 },
   {  65,  73,  97, 105,   1,   9,  33,  41,  67,  75,  99, 107,   3,  11,  35,  43,  69,  77, 101, 109,   5,  13,  37,  45,  71,  79, 103, 111,   7,  15,  39,  47 },
   {  81,  89, 113, 121,  17,  25,  49,  57,  83,  91, 115, 123,  19,  27,  51,  59,  85,  93, 117, 125,  21,  29,  53,  61,  87,  95, 119, 127,  23,  31,  55,  63 },
   { 192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174 },
   { 208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190 },
   { 129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239 },
   { 145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255 },
   { 256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366 },
   { 272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382 },
   { 321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303 },
   { 337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319 },
   { 448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430 },
   { 464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446 },
   { 385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495 },
   { 401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511 }
};

/* ---- the GS: word addresses, and moves one pixel after another ----------- */

#define PAGES 16
#define WORDS (PAGES * 2048)

/* Word of pixel (x, y) of a 32-bit buffer at block bp, bw pages wide; the
 * depth layout numbers its blocks with 0x18 flipped. */
static u32 word32(int x, int y, u32 bp, u32 bw, int depth)
{
   const u32 page = (u32)(y / 32) * bw + (u32)(x / 64);
   const u32 blk = block32[(y % 32) / 8][(x % 64) / 8] ^ (depth ? 0x18u : 0u);
   return ((bp + page * 32 + blk) * 64 + column32[y % 8][x % 8]) % WORDS;
}

/* Nibble address of pixel (x, y) of a PSMT4 buffer: a page is 128x128. */
static u32 nib4(int x, int y, u32 bp, u32 bw)
{
   const u32 page = (u32)(y / 128) * (bw / 2) + (u32)(x / 128);
   const u32 blk = bp + page * 32 + block4[(y % 128) / 16][(x % 128) / 32];
   return (blk * 512 + column4[y % 16][x % 32]) % (WORDS * 8);
}

enum { Z32, CT32, T4 };

struct move
{
   u32 sbp, sbw, dbp, dbw;
   int spsm, dpsm, sx, sy, dx, dy, w, h;
};

/* An element as (word, first nibble, nibbles). */
static void element(int psm, u32 bp, u32 bw, int x, int y, u32* word, u32* nib, u32* count)
{
   if (psm == T4)
   {
      const u32 a = nib4(x, y, bp, bw);
      *word = a >> 3;
      *nib = a & 7;
      *count = 1;
   }
   else
   {
      *word = word32(x, y, bp, bw, psm == Z32);
      *nib = 0;
      *count = 8;
   }
}

static u32 get_nibbles(const u32* mem, u32 word, u32 nib, u32 count)
{
   return count == 8 ? mem[word] : (mem[word] >> (nib * 4)) & ((1u << (count * 4)) - 1u);
}

static void put_nibbles(u32* mem, u32 word, u32 nib, u32 count, u32 v)
{
   if (count == 8)
      mem[word] = v;
   else
   {
      const u32 m = ((1u << (count * 4)) - 1u) << (nib * 4);
      mem[word] = (mem[word] & ~m) | ((v << (nib * 4)) & m);
   }
}

static void gs_move(u32* mem, const struct move* m)
{
   int i, j;
   for (j = 0; j < m->h; j++)
      for (i = 0; i < m->w; i++)
      {
         u32 sw, sn, sc, dw, dn, dc;
         element(m->spsm, m->sbp, m->sbw, m->sx + i, m->sy + j, &sw, &sn, &sc);
         element(m->dpsm, m->dbp, m->dbw, m->dx + i, m->dy + j, &dw, &dn, &dc);
         put_nibbles(mem, dw, dn, dc, get_nibbles(mem, sw, sn, sc));
      }
}

/* ---- the targets: a word per pixel, S x S samples ------------------------- */

#define TW 128
#define TH 64
#define S 2

struct target
{
   u32 tbp, tbw;
   int depth;
   u32 px[TH * S][TW * S]; /* sample (sx, sy) of pixel (x, y) at (x*S+sx, y*S+sy) */
};

static struct target zt, ct;
static unsigned short inverse[2][2048];

static void build_inverse(void)
{
   int x, y, l;
   for (l = 0; l < 2; l++)
      for (y = 0; y < 32; y++)
         for (x = 0; x < 64; x++)
            inverse[l][word32(x, y, 0, 1, l) % 2048] = (unsigned short)(x | (y << 6));
}

/* TargetPixelOfWord. */
static int pixel_of_word(const struct target* t, u32 word, int* x, int* y)
{
   const u32 page = ((word >> 11) - (t->tbp >> 5)) & 511u;
   const u32 in = inverse[t->depth][word & 2047u];
   *x = (int)((page % t->tbw) * 64 + (in & 63u));
   *y = (int)((page / t->tbw) * 32 + (in >> 6));
   return *x < TW && *y < TH;
}

/* BuildMoveRemap and ps_move_texels, over the whole target. Returns 0 when
 * the remap cannot be built. */
static u32 remap[TH][TW * 8];
static u32 stage[TH * S][TW * S];

static int model_move(struct target* st, struct target* dt, const struct move* m, int first_nibble_only)
{
   const u32 dcount = (m->dpsm == T4) ? 1u : 8u;
   const u32 entries = 8u / dcount;
   int i, j, x, y, sxs, sys;
   memset(remap, 0, sizeof(remap));
   for (j = 0; j < m->h; j++)
      for (i = 0; i < m->w; i++)
      {
         u32 sw, sn, sc, dw, dn, dc;
         int tsx, tsy, tdx, tdy;
         element(m->spsm, m->sbp, m->sbw, m->sx + i, m->sy + j, &sw, &sn, &sc);
         element(m->dpsm, m->dbp, m->dbw, m->dx + i, m->dy + j, &dw, &dn, &dc);
         if (!pixel_of_word(st, sw, &tsx, &tsy) || !pixel_of_word(dt, dw, &tdx, &tdy) || dn % dc)
            return 0;
         if (first_nibble_only)
            sn = 0;
         remap[tdy][tdx * entries + dn / dc] = 0x80000000u | (sn << 26) | ((u32)tsy << 13) | (u32)tsx;
      }
   /* the shader reads a copy: an in-place move reads what was there */
   memcpy(stage, st->px, sizeof(stage));
   for (y = 0; y < TH; y++)
      for (x = 0; x < TW; x++)
         for (sys = 0; sys < S; sys++)
            for (sxs = 0; sxs < S; sxs++)
            {
               u32 word = dt->px[y * S + sys][x * S + sxs];
               const u32 mask = dcount >= 8 ? 0xFFFFFFFFu : (1u << (dcount * 4)) - 1u;
               u32 e;
               for (e = 0; e < entries; e++)
               {
                  const u32 ent = remap[y][x * entries + e];
                  u32 v, shift;
                  if (!(ent & 0x80000000u))
                     continue;
                  v = (stage[((ent >> 13) & 0x1FFFu) * S + sys][(ent & 0x1FFFu) * S + sxs] >> (((ent >> 26) & 7u) * 4)) & mask;
                  shift = e * dcount * 4;
                  word = (word & ~(mask << shift)) | (v << shift);
               }
               dt->px[y * S + sys][x * S + sxs] = word;
            }
   return 1;
}

/* ---- the check ------------------------------------------------------------ */

static u32 mem[S * S][WORDS];
static u32 depth_at[S * S][TH][TW];

static u32 rng = 12345u;
static u32 next(void)
{
   rng = rng * 1103515245u + 12345u;
   return (rng >> 8) ^ (rng << 20);
}

static void load_target(struct target* t)
{
   int x, y, k;
   for (k = 0; k < S * S; k++)
      for (y = 0; y < TH; y++)
         for (x = 0; x < TW; x++)
            t->px[y * S + k / S][x * S + k % S] = mem[k][word32(x, y, t->tbp, t->tbw, t->depth)];
}

static int run(int first_nibble_only, int* index_ok)
{
   /* depth 128x64 at 0x000, colour at 0x100, both two pages wide; the
    * PSMT4 view of the colour buffer is one page wide, four pages tall */
   static struct move chain[1 + 4 * 2];
   int n = 0, i, k, x, y, differ = 0;
   chain[n].sbp = 0x000; chain[n].sbw = 2; chain[n].dbp = 0x100; chain[n].dbw = 2;
   chain[n].spsm = Z32; chain[n].dpsm = CT32;
   chain[n].sx = chain[n].sy = chain[n].dx = chain[n].dy = 0; chain[n].w = TW; chain[n].h = TH;
   n++;
   for (i = 0; i < 4; i++)
   {
      chain[n].sbp = chain[n].dbp = 0x100; chain[n].sbw = chain[n].dbw = 2;
      chain[n].spsm = chain[n].dpsm = T4;
      chain[n].sx = 16 + 32 * i; chain[n].dx = 24 + 32 * i; chain[n].sy = chain[n].dy = 0;
      chain[n].w = 8; chain[n].h = 512;
      n++;
   }

   rng = 12345u;
   for (k = 0; k < S * S; k++)
      for (i = 0; i < WORDS; i++)
         mem[k][i] = next();
   for (k = 0; k < S * S; k++)
      for (y = 0; y < TH; y++)
         for (x = 0; x < TW; x++)
            depth_at[k][y][x] = mem[k][word32(x, y, 0x000, 2, 1)];

   zt.tbp = 0x000; zt.tbw = 2; zt.depth = 1;
   ct.tbp = 0x100; ct.tbw = 2; ct.depth = 0;
   load_target(&zt);
   load_target(&ct);

   for (i = 0; i < n; i++)
   {
      for (k = 0; k < S * S; k++)
         gs_move(mem[k], &chain[i]);
      if (!model_move(chain[i].spsm == Z32 ? &zt : &ct, &ct, &chain[i], first_nibble_only))
      {
         printf("  move %d: no remap\n", i);
         return 1;
      }
   }

   *index_ok = 1;
   for (k = 0; k < S * S; k++)
      for (y = 0; y < TH; y++)
         for (x = 0; x < TW; x++)
         {
            const u32 t = ct.px[y * S + k / S][x * S + k % S];
            differ += t != mem[k][word32(x, y, 0x100, 2, 0)];
            if ((t >> 24) != ((depth_at[k][y][x] >> 16) & 255u))
               *index_ok = 0;
         }
   return differ != 0;
}

int main(void)
{
   int fail = 0, index_ok = 0, neg_index;
   build_inverse();
   if (run(0, &index_ok))
   {
      printf("  the targets' samples differ from their memories after the moves\n");
      fail++;
   }
   if (!index_ok)
   {
      printf("  the index is not the depth's bits 16-23 at every sample\n");
      fail++;
   }
   if (!run(1, &neg_index))
   {
      printf("  negative: whole-word nibble placement matched the GS\n");
      fail++;
   }
   printf("move in targets: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
