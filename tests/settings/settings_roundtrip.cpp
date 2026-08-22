/* Settings round trip: write values, read them back, compare.
 *
 * The config strings are the one part of this tree the existing gates
 * cannot see. Frame hashes come from a fixed harness configuration, and the
 * savestate round trip covers the savestate header, not the ini. A BIOS
 * path silently truncated at some buffer's length would pass every gate
 * there is and fail on somebody's deep directory.
 *
 * So before Pcsx2Config's std::string fields become fixed buffers, this
 * establishes what the current implementation does with awkward input:
 * long paths, UTF-8, embedded separators, empty values. Run it before and
 * after the conversion; the two must agree.
 *
 *   cc -o settings_roundtrip settings_roundtrip.cpp \
 *      ../../common/MemorySettingsInterface.cpp ... -I...
 */

#include <cstdio>
#include <cstring>
#include <string>

#include "common/MemorySettingsInterface.h"

struct tcase
{
   const char *name;
   const char *value;
};

/* Deliberately awkward: the point is to find where a fixed buffer would
 * start losing data, not to check that "abc" survives. */
static std::string long_path(size_t n)
{
   std::string s = "/home/user";
   while (s.size() < n)
      s += "/averyverylongdirectorycomponentname";
   s.resize(n);
   return s;
}

int main(void)
{
   MemorySettingsInterface si;
   int fails = 0;
   int total = 0;

   const std::string p64   = long_path(64);
   const std::string p128  = long_path(128);
   const std::string p255  = long_path(255);
   const std::string p256  = long_path(256);
   const std::string p511  = long_path(511);
   const std::string p512  = long_path(512);
   const std::string p1024 = long_path(1024);

   const tcase cases[] = {
      { "empty",        ""                                  },
      { "short",        "scph39001.bin"                     },
      { "spaces",       "/home/a b/c d/scph39001.bin"       },
      { "utf8",         "/home/\xc3\xa9\xc3\xa8/bios.bin"   },
      { "backslash",    "C:\\PS2\\bios\\scph39001.bin"      },
      { "trailing_sep", "/home/user/bios/"                  },
      { "len64",        p64.c_str()                         },
      { "len128",       p128.c_str()                        },
      { "len255",       p255.c_str()                        },
      { "len256",       p256.c_str()                        },
      { "len511",       p511.c_str()                        },
      { "len512",       p512.c_str()                        },
      { "len1024",      p1024.c_str()                       },
   };

   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      std::string got;

      si.SetStringValue("Filenames", cases[i].name, cases[i].value);
      total++;

      if (!si.GetStringValue("Filenames", cases[i].name, &got))
      {
         printf("FAIL  %-12s: key missing after set\n", cases[i].name);
         fails++;
         continue;
      }

      if (got != cases[i].value)
      {
         printf("FAIL  %-12s: wrote %zu bytes, read %zu\n",
               cases[i].name, strlen(cases[i].value), got.size());
         fails++;
      }
   }

   /* A key that was never set must stay unset rather than yielding an
    * empty string, because the caller distinguishes the two. */
   {
      std::string got;
      total++;
      if (si.GetStringValue("Filenames", "never_set", &got))
      {
         printf("FAIL  never_set: reported present\n");
         fails++;
      }
   }

   printf("%d cases, %d failing\n", total, fails);
   return fails != 0;
}
