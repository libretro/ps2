/* paraLLEl-GS's motion-adaptive deinterlacer: when a missing line moves.
 *
 * The scanout weaves the previous field's line where the picture holds
 * still and takes the mean of the current field's lines around it where it
 * moves (gs/shaders/weave.frag). The lines above and below are compared
 * with the same field two back, the missing line with its own field two
 * back. Taking the smaller of the two neighbours' differences called a
 * line still when only one of them moved, as along the edge of anything
 * moving vertically, and wove it: the edge combed. The line moves if
 * either neighbour or the line itself moved.
 *
 * Pinned here: the decision for an edge crossing only the line above, only
 * the line below, both, the line itself, and a still picture; and the
 * shader embedded in slangmosh.hpp combining the three differences with
 * two maxima and no minimum (GLSL.std.450 FMax 40, FMin 37).
 *
 * Build and run, from tests/gsinterlace:
 *   cc -O2 -std=c89 -pedantic -Wall fastmad_rule.c -o fastmad_rule -lm
 *   ./fastmad_rule ../../pcsx2/GS/parallel-gs/gs/shaders/slangmosh.hpp
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double smoothstep(double lo, double hi, double x)
{
   double t = (x - lo) / (hi - lo);
   if (t < 0.0)
      t = 0.0;
   if (t > 1.0)
      t = 1.0;
   return t * t * (3.0 - 2.0 * t);
}

/* weave.frag: the bob factor from the three luma differences. */
static double bob_factor(double mh, double ml, double mc)
{
   double d = mh > ml ? mh : ml;
   if (mc > d)
      d = mc;
   return smoothstep(0.04, 0.06, d);
}

static int check_rule(void)
{
   static const struct
   {
      double mh, ml, mc, want;
   } cases[] = {
      { 0.30, 0.00, 0.00, 1.0 }, /* an edge crossing the line above */
      { 0.00, 0.30, 0.00, 1.0 }, /* the line below */
      { 0.30, 0.30, 0.00, 1.0 }, /* both */
      { 0.00, 0.00, 0.30, 1.0 }, /* the missing line itself */
      { 0.01, 0.02, 0.01, 0.0 }  /* a still picture, with noise */
   };
   int fail = 0;
   size_t i;
   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      const double b = bob_factor(cases[i].mh, cases[i].ml, cases[i].mc);
      if (fabs(b - cases[i].want) > 1e-9)
      {
         printf("  case %u: bob %g, wanted %g\n", (unsigned)i, b, cases[i].want);
         fail++;
      }
   }
   return fail;
}

/* The weave shader's words, out of slangmosh.hpp's spirv_bank at the
 * offset and size its request_shader call gives. */
static unsigned long *load_weave(const char *path, size_t *count)
{
   FILE *f = fopen(path, "rb");
   char *text, *bank, *call, *p;
   long len;
   unsigned long *words = NULL;
   unsigned long offset, bytes, n = 0;
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   len = ftell(f);
   fseek(f, 0, SEEK_SET);
   text = (char *)malloc((size_t)len + 1);
   if (!text || fread(text, 1, (size_t)len, f) != (size_t)len)
   {
      fclose(f);
      free(text);
      return NULL;
   }
   fclose(f);
   text[len] = '\0';
   bank = strstr(text, "spirv_bank[] =");
   call = strstr(text, "this->weave = device.request_shader(spirv_bank + ");
   if (!bank || !call || sscanf(call + strlen("this->weave = device.request_shader(spirv_bank + "), "%lu, %lu", &offset, &bytes) != 2)
   {
      free(text);
      return NULL;
   }
   words = (unsigned long *)malloc((bytes / 4) * sizeof(*words));
   if (!words)
   {
      free(text);
      return NULL;
   }
   for (p = bank; (p = strstr(p, "0x")) != NULL && n < offset + bytes / 4; p += 2)
   {
      const unsigned long w = strtoul(p, NULL, 16);
      if (n >= offset)
         words[n - offset] = w;
      n++;
   }
   free(text);
   if (n < offset + bytes / 4)
   {
      free(words);
      return NULL;
   }
   *count = bytes / 4;
   return words;
}

static int check_blob(const char *path)
{
   size_t count = 0, i;
   unsigned long *w = load_weave(path, &count);
   int fmin = 0, fmax = 0, smooth = 0;
   if (!w)
   {
      printf("  cannot read the weave shader from %s\n", path);
      return 1;
   }
   if (count < 5 || w[0] != 0x07230203UL)
   {
      printf("  the weave shader is not SPIR-V\n");
      free(w);
      return 1;
   }
   for (i = 5; i < count;)
   {
      const unsigned long op = w[i] & 0xffffUL;
      const unsigned long wc = w[i] >> 16;
      if (wc == 0)
         break;
      if (op == 12 && wc >= 5) /* OpExtInst: type, id, set, instruction */
      {
         if (w[i + 4] == 37)
            fmin++;
         else if (w[i + 4] == 40)
            fmax++;
         else if (w[i + 4] == 49)
            smooth++;
      }
      i += wc;
   }
   free(w);
   if (fmin != 0 || fmax != 2 || smooth != 1)
   {
      printf("  the weave shader has %d FMin, %d FMax, %d SmoothStep; wanted 0, 2, 1\n", fmin, fmax, smooth);
      return 1;
   }
   return 0;
}

int main(int argc, char **argv)
{
   int fail = check_rule();
   if (argc > 1)
      fail += check_blob(argv[1]);
   printf("fastmad rule: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
