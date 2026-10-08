/* The chunked reader hands back exactly the bytes the file holds.
 *
 * ThreadedFileReader sits under every compressed CDVD format and serves
 * sectors out of a two-buffer chunk cache: a sector read decompresses
 * the chunk containing it, and every later sector inside that chunk
 * comes from the cache. The cache is where the interesting failures
 * live -- a stale buffer, a sector served from the wrong chunk, a
 * boundary crossing that takes half from each -- and none of them
 * crash. They hand back plausible-looking wrong data, which a game sees
 * as a corrupt disc.
 *
 * tests/cdvd/make_cso.py exists because this path had no coverage, but
 * it needs a real image and a real run to compare against. This needs
 * neither.
 *
 * A flat image is mapped and served directly and never reaches the
 * cache, so the CSO pass below is the one that matters: breaking the
 * cache-hit test and running only the flat pass leaves it green.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <libretro.h>

#include "CDVD/ThreadedFileReader.h"
#include "CDVD/CsoFileReader.h"
#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>
#include "CDVD/IsoFileFormats.h"
#include "Config.h"
#include "Host.h"
#include "HostFS.h"

/* The readers log their errors; the damaged-image case reaches them. */
static void quiet_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level; (void)fmt;
}
retro_log_printf_t log_cb = quiet_log;
std::string libretro_content;

/* Pcsx2Config.cpp supplies EmuConfig and EmuFolders. Only the directory
 * walk is left, which the CHD reader uses to look for a parent image --
 * a path neither image here takes. */
namespace FileSystem {
   bool FindFiles(const char*, const char*, unsigned, std::vector<FILESYSTEM_FIND_DATA>*) { return false; }
}

#define SECTOR   2048
#define SECTORS  512

/* Deterministic, and compressible in parts and not in others, so the CSO
 * writer both compresses and stores blocks. */
static void fill(uint8_t *p, unsigned lsn)
{
   unsigned i;
   for (i = 0; i < SECTOR; i++)
   {
      if ((lsn & 3) == 0)
         p[i] = (uint8_t)(lsn & 0xFF);
      else
         p[i] = (uint8_t)((lsn * 2654435761u + i * 40503u) >> 13);
   }
}

static int check_pass(InputIsoFile &in, const uint8_t *image, const char *what)
{
   /* ReadSync writes at dst + the block offset the detector chose -- 24
    * for a 2048-byte ISO, where the volume descriptor sits at byte 25 of
    * the raw sector -- so the buffer is a raw sector and the comparison
    * starts there. */
   uint8_t got[2456];
   unsigned ofs = (unsigned)in.GetBlockOffset();
   unsigned lsn;

   for (lsn = 0; lsn < SECTORS; lsn++)
      if (in.ReadSync(got, lsn) < 0 || memcmp(got + ofs, image + (size_t)lsn * SECTOR, SECTOR))
      {
         printf("  FAIL: %s, sequential read wrong at sector %u\n", what, lsn);
         return 0;
      }
   /* Backwards, so every read misses the buffer the previous one filled. */
   for (lsn = SECTORS; lsn-- > 0; )
      if (in.ReadSync(got, lsn) < 0 || memcmp(got + ofs, image + (size_t)lsn * SECTOR, SECTOR))
      {
         printf("  FAIL: %s, reverse read wrong at sector %u\n", what, lsn);
         return 0;
      }
   /* Two sectors far apart, alternating: each evicts the other's chunk,
    * so a buffer reused without its offset being checked hands back the
    * wrong one. */
   for (lsn = 0; lsn < 64; lsn++)
   {
      unsigned a = lsn, b = SECTORS - 1 - lsn;
      if (in.ReadSync(got, a) < 0 || memcmp(got + ofs, image + (size_t)a * SECTOR, SECTOR) ||
          in.ReadSync(got, b) < 0 || memcmp(got + ofs, image + (size_t)b * SECTOR, SECTOR))
      {
         printf("  FAIL: %s, alternating read wrong around sector %u\n", what, a);
         return 0;
      }
   }
   printf("  ok: %s -- %d sectors forwards, backwards and alternating\n", what, SECTORS);
   return 1;
}

static void put32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A CSO whose index runs backwards at its first frame, with 512 KB of
 * payload after it, read through a VFS that has no mapping - the way a
 * frontend VFS serves it, and the path that stages frames in the read
 * buffer. The frame is refused; nothing past the buffer is written. */
static int check_backwards_cso(const char *path)
{
   static struct retro_vfs_interface iface;
   struct retro_vfs_interface_info info;
   const size_t payload = 512 * 1024;
   const uint32_t frames = 4;
   const size_t index_at = 24, payload_at = 24 + (frames + 1) * 4;
   uint8_t *file = (uint8_t*)calloc(1, payload_at + payload);
   uint8_t sector[SECTOR];
   int ok = 1, got;
   FILE *f;

   if (!file)
      return 0;
   memcpy(file, "CISO", 4);
   put32(file + 4, 24);
   put32(file + 8, frames * SECTOR);
   put32(file + 12, 0);
   put32(file + 16, SECTOR);
   file[20] = 1;
   put32(file + index_at, (uint32_t)payload_at);
   put32(file + index_at + 4, 1);
   put32(file + index_at + 8, (uint32_t)(payload_at + payload));
   put32(file + index_at + 12, (uint32_t)(payload_at + payload));
   put32(file + index_at + 16, (uint32_t)(payload_at + payload));
   f = fopen(path, "wb");
   if (!f || fwrite(file, 1, payload_at + payload, f) != payload_at + payload)
   {
      printf("  FAIL: could not write %s\n", path);
      free(file);
      return 0;
   }
   fclose(f);
   free(file);

   iface.get_path = (retro_vfs_get_path_t)retro_vfs_file_get_path_impl;
   iface.open     = (retro_vfs_open_t)retro_vfs_file_open_impl;
   iface.close    = (retro_vfs_close_t)retro_vfs_file_close_impl;
   iface.size     = (retro_vfs_size_t)retro_vfs_file_size_impl;
   iface.tell     = (retro_vfs_tell_t)retro_vfs_file_tell_impl;
   iface.seek     = (retro_vfs_seek_t)retro_vfs_file_seek_impl;
   iface.read     = (retro_vfs_read_t)retro_vfs_file_read_impl;
   info.required_interface_version = FILESTREAM_REQUIRED_VFS_VERSION;
   info.iface = &iface;
   filestream_vfs_init(&info);
   {
      CsoFileReader cr;
      if (!cr.Open(path))
      {
         printf("  FAIL: could not open the damaged CSO\n");
         ok = 0;
      }
      else
      {
         got = cr.ReadSync(sector, 0, 1);
         if (got > 0)
         {
            printf("  FAIL: a frame whose index runs backwards was read (%d)\n", got);
            ok = 0;
         }
         cr.Close();
      }
   }
   info.iface = NULL;
   filestream_vfs_init(&info);
   remove(path);
   if (ok)
      printf("  ok: a CSO frame whose index runs backwards is refused\n");
   return ok;
}

int main(int argc, char **argv)
{
   const char *iso = "cdvdread.iso";
   const char *cso = "cdvdread.cso";
   const char *dir = (argc > 1) ? argv[1] : ".";
   uint8_t *image  = (uint8_t*)malloc(SECTOR * SECTORS);
   uint8_t got[2456];
   unsigned lsn;
   int ok = 1, have_cso = 0;
   FILE *f;

   setvbuf(stdout, NULL, _IONBF, 0);
   printf("cdvd read\n");

   for (lsn = 0; lsn < SECTORS; lsn++)
      fill(image + (size_t)lsn * SECTOR, lsn);
   /* Sector 16 is the primary volume descriptor, which the block size is
    * detected from: "CD001" at byte 1 and the logical block size at 128.
    * Without it the image is probed as 2352- and 2448-byte sectors and
    * read with the wrong geometry. */
   {
      uint8_t *pvd = image + (size_t)16 * SECTOR;
      memset(pvd, 0, SECTOR);
      pvd[0] = 1;
      memcpy(pvd + 1, "CD001", 5);
      pvd[6] = 1;
      pvd[128] = (uint8_t)(SECTOR & 0xFF);
      pvd[129] = (uint8_t)((SECTOR >> 8) & 0xFF);
      pvd[130] = (uint8_t)((SECTOR >> 8) & 0xFF);
      pvd[131] = (uint8_t)(SECTOR & 0xFF);
   }
   f = fopen(iso, "wb");
   if (!f || fwrite(image, SECTOR, SECTORS, f) != SECTORS)
   {
      printf("  FAIL: could not write %s\n", iso);
      return 1;
   }
   fclose(f);

   {
      char cmd[1024];
      snprintf(cmd, sizeof(cmd),
               "python3 '%s/../cdvd/make_cso.py' '%s' '%s' > /dev/null 2>&1", dir, iso, cso);
      have_cso = (system(cmd) == 0);
      if (!have_cso)
         printf("  note: no python3, so the chunk cache is not covered here\n");
   }

   {
      InputIsoFile in;
      if (!in.Open(iso))
      {
         printf("  FAIL: Open\n");
         return 1;
      }
      printf("  opened %u sectors, block offset %d\n",
             (unsigned)in.GetBlockCount(), in.GetBlockOffset());
      ok &= check_pass(in, image, "flat, served directly");

      /* The split path: BeginRead2 reads, FinishRead3 converts to the
       * requested mode and writes the data bytes at dst + 0. */
      {
         int good = 1;
         for (lsn = 0; lsn < SECTORS && good; lsn += 7)
         {
            in.BeginRead2(lsn);
            if (in.FinishRead3(got, CDVD_MODE_2048) < 0 ||
                memcmp(got, image + (size_t)lsn * SECTOR, SECTOR))
            {
               printf("  FAIL: BeginRead2/FinishRead3 wrong at sector %u\n", lsn);
               good = 0;
               break;
            }
            /* Again: BeginRead2 short-circuits on the same sector, so
             * this checks the held result is still the right one. */
            in.BeginRead2(lsn);
            if (in.FinishRead3(got, CDVD_MODE_2048) < 0 ||
                memcmp(got, image + (size_t)lsn * SECTOR, SECTOR))
            {
               printf("  FAIL: repeated read of sector %u differs\n", lsn);
               good = 0;
            }
         }
         if (good)
            printf("  ok: the split read path, including a repeated sector\n");
         ok &= good;
      }

      /* The 2368-byte mode is the raw 2352: on a 2048-byte image the 24
       * bytes before the data are zero and the data follows. A mode the
       * reader does not know is an error, not a zero-length copy. */
      {
         int good = 1, i;
         memset(got, 0xee, sizeof(got));
         in.BeginRead2(20);
         if (in.FinishRead3(got, CDVD_MODE_2368) < 0)
            good = 0;
         for (i = 0; i < 24 && good; i++)
            if (got[i] != 0)
               good = 0;
         if (good && memcmp(got + 24, image + (size_t)20 * SECTOR, SECTOR))
            good = 0;
         in.BeginRead2(20);
         if (in.FinishRead3(got, 99) != -1)
            good = 0;
         if (good)
            printf("  ok: the 2368-byte mode reads the raw sector, an unknown mode fails\n");
         else
            printf("  FAIL: the 2368-byte mode or an unknown mode\n");
         ok &= good;
      }

      /* Past the end: several games do this and must not be told the
       * disc is broken. */
      in.BeginRead2(SECTORS + 16);
      if (in.FinishRead3(got, CDVD_MODE_2048) != 0)
      {
         printf("  FAIL: a read past the end should report 0, not an error\n");
         ok = 0;
      }
      else
         printf("  ok: a read past the end reports 0\n");
      in.Close();
   }

   /* And through the CSO, where every read goes through the chunk cache. */
   if (have_cso)
   {
      InputIsoFile in;
      if (!in.Open(cso))
      {
         printf("  FAIL: could not open the CSO\n");
         ok = 0;
      }
      else
      {
         ok &= check_pass(in, image, "CSO, through the chunk cache");
         in.Close();
      }
      remove(cso);
   }

   ok &= check_backwards_cso("cdvdread_backwards.cso");

   free(image);
   remove(iso);
   printf(ok ? "cdvd read: ok\n" : "cdvd read: FAILED\n");
   return ok ? 0 : 1;
}
