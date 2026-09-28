/* Canonical form of a SPIR-V disassembly, for comparing two builds of a
 * shader: ids renumbered in the order the function bodies first use
 * them, then the ids only the declarations mention in the order those
 * declarations sort once numbered, the declarations before the first
 * function sorted, names and source strings dropped, whitespace
 * trimmed. Two modules that differ only in id numbers and declaration
 * order come out identical; two that differ in an instruction do not.
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

/* Ids numbered so far, in the order the bodies met them; the others
 * wait for the declarations to be sorted. */
static int *id_number;
static int num_numbered;

static int number_id(const char *s, size_t n)
{
   int idx = id_index(s, n, 1);
   if (idx >= 0 && id_number[idx] < 0)
      id_number[idx] = num_numbered++;
   return idx;
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
         number_id(p + 1, (size_t)(e - p - 1));
      p = e;
   }
}

/* Rewrite a line with its numbered ids as %iN and the rest as %?;
 * returns how many of the rest it holds. */
static int rewrite(const char *l, char *out)
{
   const char *p = l;
   char *o = out;
   int pending = 0;
   while (*p)
   {
      if (*p == '%')
      {
         const char *e = p + 1;
         int idx;
         while (is_id_char((unsigned char)*e))
            e++;
         idx = id_index(p + 1, (size_t)(e - p - 1), 0);
         if (idx >= 0 && id_number[idx] >= 0)
            o += sprintf(o, "%%i%d", id_number[idx]);
         else
         {
            o += sprintf(o, "%%?");
            pending++;
         }
         p = e;
      }
      else
         *o++ = *p++;
   }
   *o = 0;
   return pending;
}

struct decl
{
   const char *src;
   char *text;
   int pending;
};

static int cmp_decl(const void *a, const void *b)
{
   return strcmp(((const struct decl *)a)->text, ((const struct decl *)b)->text);
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
   int i, progress;
   struct decl *header;
   int num_header = 0;

   lines = (char **)malloc(MAX_LINES * sizeof(*lines));
   id_names = (char **)malloc(MAX_IDS * sizeof(*id_names));
   id_number = (int *)malloc(MAX_IDS * sizeof(*id_number));
   hash_slots = (int *)malloc(HASH_SIZE * sizeof(*hash_slots));
   if (!lines || !id_names || !id_number || !hash_slots)
      return 1;
   for (i = 0; i < HASH_SIZE; i++)
      hash_slots[i] = -1;
   for (i = 0; i < MAX_IDS; i++)
      id_number[i] = -1;

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

   /* Ids in the order the bodies use them. */
   for (i = first_function; i < num_lines; i++)
      number_ids(lines[i]);

   header = (struct decl *)malloc((size_t)(first_function + 1) * sizeof(*header));
   if (!header)
      return 1;
   for (i = 0; i < first_function; i++)
   {
      header[num_header].src = lines[i];
      header[num_header].text = (char *)malloc(strlen(lines[i]) * 2 + 16);
      num_header++;
   }

   /* Then the ids only the declarations mention: sort the declarations
    * with those ids blanked, and number, in that order, the ids of the
    * declarations that have just their own left -- a declaration whose
    * operands are all numbered sorts the same in any build. Repeat as
    * numbering one resolves others; whatever is left at the end (ids
    * that never resolve this way) is numbered in sorted order. */
   do
   {
      progress = 0;
      for (i = 0; i < num_header; i++)
         header[i].pending = rewrite(header[i].src, header[i].text);
      qsort(header, (size_t)num_header, sizeof(*header), cmp_decl);
      for (i = 0; i < num_header; i++)
         if (header[i].pending == 1)
         {
            number_ids(header[i].src);
            progress = 1;
         }
   } while (progress);
   for (i = 0; i < num_header; i++)
      if (header[i].pending)
         number_ids(header[i].src);
   for (i = 0; i < num_header; i++)
      rewrite(header[i].src, header[i].text);
   qsort(header, (size_t)num_header, sizeof(*header), cmp_decl);

   for (i = 0; i < num_header; i++)
   {
      puts(header[i].text);
      free(header[i].text);
   }
   free(header);
   for (i = first_function; i < num_lines; i++)
   {
      rewrite(lines[i], buf);
      puts(buf);
   }
   for (i = 0; i < num_lines; i++)
      free(lines[i]);
   for (i = 0; i < num_ids; i++)
      free(id_names[i]);
   free(lines);
   free(id_names);
   free(id_number);
   free(hash_slots);
   return 0;
}
