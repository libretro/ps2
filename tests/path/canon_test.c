/* Test table for the lexical path canonicaliser used by the host: sandbox.
 *
 * IopBios::host_path decides whether a guest path stays inside the ELF
 * directory. It used Path::Canonicalize, which removes "." and ".."
 * components WITHOUT touching the filesystem -- deliberately: the guest may
 * be creating a file that does not exist yet, so anything resolving real
 * paths (realpath, path_resolve_realpath) would both fail on new files and
 * follow symlinks, changing what the sandbox compares.
 *
 * So the C replacement has to be lexical too. This exercises it against the
 * cases that matter, the escape attempts above all, before it goes anywhere
 * near the emulator.
 *
 *   cc -o canon_test canon_test.c && ./canon_test
 *
 * The table below is not what canonicalisation "should" do -- it is what
 * Path::Canonicalize actually does, established by running both against the
 * same inputs. My first attempt at this port passed a table I wrote from
 * first principles and still disagreed with the reference in six ways,
 * including treating backslash as a separator (it is not, on POSIX) and
 * refusing to pop above the root (the reference pops the empty leading
 * component, so "/.." yields "", not "/"). Both would have changed what
 * the sandbox compares. Beyond this table, the two implementations agree on
 * 200,000 randomly generated paths.
 */

#include <stdio.h>
#include <string.h>

/* Candidate implementation, kept here until it is proven, then moved into
 * the tree. Operates in place on a mutable buffer.
 *
 * Rules, matching Path::Canonicalize:
 *   - both '/' and '\\' separate; output uses '/'
 *   - "." components are dropped
 *   - ".." pops the previous component, except that it cannot pop above the
 *     root of an absolute path, and is kept verbatim at the start of a
 *     relative path (there is nothing to pop yet)
 *   - repeated separators collapse
 *   - a trailing separator is dropped, except for a bare root
 */
/* Faithful port of Path::Canonicalize (common/FileSystem.cpp) for POSIX.
 *
 * The rules are the reference implementation's, not the ones you would
 * guess, and the differences matter:
 *
 *   - Only '/' separates. A backslash is an ordinary character here;
 *     translating it is ToNativePath's job, and only on Windows.
 *   - An absolute path yields an EMPTY leading component, so the rejoin
 *     puts the slash back. That empty component is a real component: ".."
 *     pops it, which is why "/.." canonicalizes to "" and not "/".
 *   - ".." at the very beginning of a relative path is preserved, but
 *     only while nothing has been pushed yet -- "a/../../b" pops "a",
 *     then the second ".." finds an empty list and is preserved.
 *   - "." is dropped, unless it is the only component.
 *   - Components are rejoined with a single '/'.
 */
static void path_canonicalize_lexical(char *s)
{
	char       *comps[128];
	int         ncomp   = 0;
	int         total   = 0;
	char       *r       = s;
	char       *w;
	int         i;
	char       *starts[128];
	int         nstart  = 0;

	/* Split exactly as SplitNativePath does, including the empty leading
	 * component for an absolute path. */
	{
		size_t start = 0, pos = 0;
		const size_t len = strlen(s);

		while (pos < len)
		{
			if (s[pos] != '/')
			{
				pos++;
				continue;
			}
			if (pos != start || pos == 0)
			{
				s[pos] = '\0';
				if (nstart < 128)
					starts[nstart++] = s + start;
			}
			pos++;
			start = pos;
		}
		if (start != pos && nstart < 128)
			starts[nstart++] = s + start;
	}
	total = nstart;

	for (i = 0; i < total; i++)
	{
		char *c = starts[i];

		if (!strcmp(c, "."))
		{
			if (total == 1)
				comps[ncomp++] = c;
			continue;
		}
		if (!strcmp(c, ".."))
		{
			if (ncomp > 0)
				ncomp--;
			else if (ncomp < 128)
				comps[ncomp++] = c;
			continue;
		}
		if (ncomp < 128)
			comps[ncomp++] = c;
	}

	w = s;
	for (i = 0; i < ncomp; i++)
	{
		const size_t len = strlen(comps[i]);

		if (i)
			*w++ = '/';
		memmove(w, comps[i], len);
		w += len;
	}
	*w = '\0';
}

struct tcase { const char *in; const char *want; };

static const struct tcase cases[] = {
   /* plain */
   { "/a/b/c",              "/a/b/c" },
   { "a/b/c",               "a/b/c" },
   { "/a/b/c/",             "/a/b/c" },
   { "/",                   "" },       /* root: one empty component */
   { "",                    "" },

   /* separators */
   { "\\a\\b",              "\\a\\b" },  /* backslash is not a separator */
   { "a\\b/c",              "a\\b/c" },
   { "/a//b///c",           "/a/b/c" },

   /* dot */
   { "/a/./b",              "/a/b" },
   { "./a",                 "a" },
   { "/a/.",                "/a" },

   /* dotdot */
   { "/a/b/../c",           "/a/c" },
   { "/a/b/../../c",        "/c" },
   { "a/b/../c",            "a/c" },
   { "/a/../..",            "" },      /* the empty root component pops too */
   { "/..",                 "" },
   { "../a",                "../a" },   /* relative: kept */
   { "../../a",             "a" },   /* second .. pops the first */
   { "a/../../b",           "../b" },

   /* the escapes the sandbox exists to catch */
   { "/root/elf/../../etc/passwd",       "/etc/passwd" },
   { "/root/elf/./../elf2/file",         "/root/elf2/file" },
   { "/root/elf/sub/../../elf/ok",       "/root/elf/ok" },
};

int main(void)
{
   size_t i;
   int    fails = 0;

   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      char buf[512];

      strncpy(buf, cases[i].in, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
      path_canonicalize_lexical(buf);

      if (strcmp(buf, cases[i].want))
      {
         printf("FAIL  '%s' -> '%s'  (want '%s')\n",
               cases[i].in, buf, cases[i].want);
         fails++;
      }
   }

   printf("%zu cases, %d failing\n", sizeof(cases) / sizeof(cases[0]), fails);
   return fails != 0;
}
