/* Canonical form of a SPIR-V disassembly, for comparing two builds of a
 * shader: ids renumbered in the order the function bodies first use
 * them, the declarations before the first function sorted, names and
 * source strings dropped, whitespace trimmed. Two modules that differ
 * only in id numbers and declaration order come out identical; two that
 * differ in an instruction do not.
 *
 *   spirv-dis --no-header a.spv | pgs_spv_canon > a.txt
 *
 * Build, from tests/pgs:
 *   cc -O2 -std=c89 -pedantic -Wall pgs_spv_canon.c -o pgs_spv_canon
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINES 400000
#define MAX_IDS 200000
#define LINE_LEN 4096

static char **lines;
static int num_lines;
static char **id_names;
static int num_ids;

static int is_id_char(int c)
{
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

/* Ids are looked up by hash so a module with tens of thousands of them
 * does not take quadratic time. */
#define HASH_SIZE 524288
static int *hash_slots;

static unsigned hash_name(const char *s, size_t n)
{
   unsigned h = 2166136261u;
   size_t i;
   for (i = 0; i < n; i++)
      h = (h ^ (unsigned char)s[i]) * 16777619u;
   return h & (HASH_SIZE - 1);
}

static int id_index(const char *s, size_t n, int add)
{
   unsigned h = hash_name(s, n);
   for (;;)
   {
      int slot = hash_slots[h];
      if (slot < 0)
      {
         if (!add || num_ids == MAX_IDS)
            return -1;
         id_names[num_ids] = (char *)malloc(n + 1);
         memcpy(id_names[num_ids], s, n);
         id_names[num_ids][n] = 0;
         hash_slots[h] = num_ids;
         return num_ids++;
      }
      if (strlen(id_names[slot]) == n && !memcmp(id_names[slot], s, n))
         return slot;
      h = (h + 1) & (HASH_SIZE - 1);
   }
}

static void number_ids(const char *l)
{
   const char *p = l;
   while ((p = strchr(p, '%')) != NULL)
   {
      const char *e = p + 1;
      while (is_id_char((unsigned char)*e))
         e++;
      if (e > p + 1)
         id_index(p + 1, (size_t)(e - p - 1), 1);
      p = e;
   }
}

static void rewrite(const char *l, char *out)
{
   const char *p = l;
   char *o = out;
   while (*p)
   {
      if (*p == '%')
      {
         const char *e = p + 1;
         int idx;
         while (is_id_char((unsigned char)*e))
            e++;
         idx = id_index(p + 1, (size_t)(e - p - 1), 0);
         o += sprintf(o, "%%i%d", idx);
         p = e;
      }
      else
         *o++ = *p++;
   }
   *o = 0;
}

static int cmp_str(const void *a, const void *b)
{
   return strcmp(*(char *const *)a, *(char *const *)b);
}

static int dropped(const char *l)
{
   while (*l == ' ' || *l == '\t')
      l++;
   return !strncmp(l, "OpName", 6) || !strncmp(l, "OpMemberName", 12) ||
          !strncmp(l, "OpSource", 8) || !strncmp(l, "OpString", 8) ||
          !strncmp(l, "OpModuleProcessed", 17);
}

int main(void)
{
   static char buf[LINE_LEN];
   int first_function = -1;
   int i;
   char **header;
   int num_header = 0;

   lines = (char **)malloc(MAX_LINES * sizeof(*lines));
   id_names = (char **)malloc(MAX_IDS * sizeof(*id_names));
   hash_slots = (int *)malloc(HASH_SIZE * sizeof(*hash_slots));
   if (!lines || !id_names || !hash_slots)
      return 1;
   for (i = 0; i < HASH_SIZE; i++)
      hash_slots[i] = -1;

   while (fgets(buf, sizeof(buf), stdin))
   {
      size_t n = strlen(buf);
      char *start = buf, *end;
      if (dropped(buf))
         continue;
      while (*start == ' ' || *start == '\t')
         start++;
      end = start + strlen(start);
      while (end > start && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
         end--;
      *end = 0;
      if (end == start)
         continue;
      if (num_lines == MAX_LINES || n >= sizeof(buf) - 1)
      {
         fprintf(stderr, "disassembly too large for this tool\n");
         return 1;
      }
      lines[num_lines] = (char *)malloc((size_t)(end - start) + 1);
      strcpy(lines[num_lines], start);
      if (first_function < 0 && strstr(start, "= OpFunction "))
         first_function = num_lines;
      num_lines++;
   }
   if (first_function < 0)
      first_function = num_lines;

   /* Ids in the order the bodies use them, then whatever the header
    * alone mentions. */
   for (i = first_function; i < num_lines; i++)
      number_ids(lines[i]);
   for (i = 0; i < first_function; i++)
      number_ids(lines[i]);

   header = (char **)malloc((size_t)(first_function + 1) * sizeof(*header));
   if (!header)
      return 1;
   for (i = 0; i < first_function; i++)
   {
      header[num_header] = (char *)malloc(strlen(lines[i]) * 2 + 16);
      rewrite(lines[i], header[num_header]);
      num_header++;
   }
   qsort(header, (size_t)num_header, sizeof(*header), cmp_str);
   for (i = 0; i < num_header; i++)
      puts(header[i]);
   for (i = first_function; i < num_lines; i++)
   {
      rewrite(lines[i], buf);
      puts(buf);
   }
   return 0;
}
