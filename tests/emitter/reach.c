/* Branches, calls and the fastcall fallback at distances rel8 and rel32
 * cannot express. A displacement that does not fit must not be emitted
 * as its low bits - that is a jump somewhere else - so the near forms
 * stop the emitter, and the fastcall to a far function loads the whole
 * address. A forward jump patched once its target is known writes its
 * displacement wherever the instruction put it, at any alignment. C89;
 * builds and runs on Windows and Linux; build.sh adds the alignment
 * sanitizer where the compiler has it. */

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t s32; typedef intptr_t sptr; typedef uintptr_t uptr;
u8 *x86Ptr;
#define Jcc_Unconditional (-1) /* the C++ JccComparisonType's */
#include "common/emitter/c89emit.h"
#include "common/emitter/c89ops.h"

static u8 buf[64];
static int fails;
static jmp_buf s_env;

static void on_abort(int sig)
{
   (void)sig;
   longjmp(s_env, 1);
}

#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

/* Runs one emission; returns 1 if it stopped the emitter. */
#define STOPS(stmt, result) do { \
   signal(SIGABRT, on_abort); \
   x86Ptr = buf; \
   if (setjmp(s_env) == 0) { stmt; (result) = 0; } \
   else (result) = 1; \
   signal(SIGABRT, SIG_DFL); } while (0)

int main(void)
{
   const uptr base = (uptr)buf;
   u8 *far_fn;
   u8 *near_fn;
   int stopped;
   int n;

   if (sizeof(void *) != 8)
   {
      printf("reach: 64-bit hosts only; skipped\n");
      return 0;
   }
   far_fn  = (u8 *)(base + ((uptr)1 << 32) + 0x10u);
   near_fn = (u8 *)(base + 0x1000u);

   /* In reach: short and near forms. */
   STOPS(xe_jmp_to(buf + 16), stopped);
   CHECK(!stopped && buf[0] == 0xeb && buf[1] == 14, "short jmp in reach");
   STOPS(xe_jmp_to(near_fn), stopped);
   CHECK(!stopped && buf[0] == 0xe9, "near jmp in reach");
   STOPS(xe_call_ptr(near_fn), stopped);
   CHECK(!stopped && buf[0] == 0xe8, "near call in reach");

   /* 4 GB away: neither a byte nor a dword holds the distance. */
   STOPS(xe_jmp_to(far_fn), stopped);
   CHECK(stopped, "jmp 4 GB away stops the emitter");
   STOPS(xe_jmp_to(far_fn - 0x10u + 5u - 2u), stopped);
   CHECK(stopped, "jmp whose low 32 bits look like a short jump stops too");
   STOPS(xe_jcc_known(E_CC_Z, far_fn), stopped);
   CHECK(stopped, "jcc 4 GB away stops the emitter");
   STOPS(xe_call_ptr(far_fn), stopped);
   CHECK(stopped, "call 4 GB away stops the emitter");

   /* The fastcall falls back to the whole address in rax. */
   STOPS(xe_fastcall0(far_fn), stopped);
   n = (int)(x86Ptr - buf);
   CHECK(!stopped, "fastcall to a far function is emitted");
   CHECK(n == 12 && buf[0] == 0x48 && buf[1] == 0xb8
         && memcmp(buf + 2, &far_fn, 8) == 0
         && buf[10] == 0xff && buf[11] == 0xd0,
         "as mov rax, imm64; call rax");

   /* Forward jumps at every alignment, patched 9 bytes on. */
   for (n = 0; n < 4; n++)
   {
      u8 *slot;
      u8 *start = buf + n;
      int32_t disp;
      memset(buf, 0xcc, sizeof(buf));
      x86Ptr = start;
      xe_fwd_jcc32(E_CC_Z, slot);
      x86Ptr += 9;
      xe_fwd_set32(slot);
      memcpy(&disp, slot, 4);
      CHECK(start[0] == 0x0f && start[1] == 0x84 && disp == 9,
            "forward jcc patched at any alignment");
      memset(buf, 0xcc, sizeof(buf));
      x86Ptr = start;
      xe_fwd_jcc32(Jcc_Unconditional, slot);
      xe_fwd_set32_aligned(slot);
      memcpy(&disp, slot, 4);
      CHECK(start[0] == 0xe9 && ((uptr)(slot + 4 + disp) & 0xf) == 0,
            "forward jmp patched to an aligned target");
   }

   printf(fails ? "reach: FAILED (%d)\n" : "reach: ok\n", fails);
   return fails != 0;
}
