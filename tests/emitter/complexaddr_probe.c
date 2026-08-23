/* Behavioral probe for the complexaddr fix: verify branch selection and
 * emitted bytes for a low base (disp32 fit) and a high base (no fit). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t s32; typedef intptr_t sptr; typedef uintptr_t uptr;
u8 *x86Ptr;
#include "common/emitter/c89emit.h"
#include "common/emitter/c89ops.h"

static u8 buf[256];
static u8 nearby[64];

static int emit_lut_load(const void *base)
{
   x86Ptr = buf;
   memset(buf, 0, sizeof(buf));
   {
      struct e_mem xm;
      xe_complexaddr_si(xm, XE_CX, base, XE_AX, XE_WORDSIZE);
      xe_mov64_rmem(XE_CX, xm);
   }
   return (int)(x86Ptr - buf);
}

int main(void)
{
   int n, i;
   /* Case 1: low base fits disp32 -> expect no-base SIB form:
    * 48 8B 0C C5 <disp32=0x00001000> */
   n = emit_lut_load((const void *)(uptr)0x1000);
   printf("low  (%2d bytes): ", n);
   for (i = 0; i < n; i++) printf("%02x ", buf[i]);
   printf("\n");
   if (!(n == 8 && buf[0] == 0x48 && buf[1] == 0x8b && buf[2] == 0x0c &&
         buf[3] == 0xc5 && *(u32 *)(buf + 4) == 0x1000u))
   {
      printf("FAIL: low-base encoding wrong\n");
      return 1;
   }
   /* Case 2: high base must NOT truncate -> expect materialization via
    * tmpreg then based [rcx + rax*8] load; the truncated low32 must not
    * appear as a bare disp. */
   n = emit_lut_load((const void *)(uptr)0x7ff812345678ull);
   printf("high (%2d bytes): ", n);
   for (i = 0; i < n; i++) printf("%02x ", buf[i]);
   printf("\n");
   /* A truncated encoding would be the no-base SIB form: ModRM 0x0c,
    * SIB 0xc5. It must not appear anywhere in the far-base emission. */
   for (i = 0; i + 1 < n; i++)
   {
      if (buf[i] == 0x0c && buf[i + 1] == 0xc5)
      {
         printf("FAIL: no-base SIB form emitted for far base\n");
         return 1;
      }
   }
   /* The final instruction must be a based+indexed load, not no-base SIB:
    * mov rcx,[rcx+rax*8] = 48 8B 0C C1 */
   if (!(buf[n-4] == 0x48 && buf[n-3] == 0x8b && buf[n-2] == 0x0c && buf[n-1] == 0xc1))
   {
      printf("FAIL: high-base final load is not [tmpreg + idx*8]\n");
      return 1;
   }
   /* Case 3: base out of disp32 range but rip-reachable from the
    * buffer (a nearby static, the in-image dispatcher case when the
    * image maps high): expect LEA rip-rel + based load. Only run when
    * the linker actually put us out of low disp32 range. */
   if ((uptr)nearby > 0x7fffffffull)
   {
      n = emit_lut_load((const void *)nearby);
      printf("near (%2d bytes): ", n);
      for (i = 0; i < n; i++) printf("%02x ", buf[i]);
      printf("\n");
      if (!(buf[0] == 0x48 && buf[1] == 0x8d && buf[2] == 0x0d &&
            buf[n-4] == 0x48 && buf[n-3] == 0x8b && buf[n-2] == 0x0c && buf[n-1] == 0xc1))
      {
         printf("FAIL: rip-reachable far base did not use LEA rip-rel\n");
         return 1;
      }
   }
   printf("PASS: fit branch exact; far branch materializes, no truncation\n");
   return 0;
}
