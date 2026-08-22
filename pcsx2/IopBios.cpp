/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <ctype.h>
#include <string.h>
#include <compat/strl.h>
#include <retro_miscellaneous.h>
#include <file/file_path.h>
#include <compat/strl.h>
#include <sys/stat.h>

#include <fcntl.h>

#include <file/file_path.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "../common/Console.h"
#include "../common/FileSystem.h"
#include "../common/Path.h"
#include "../common/StringUtil.h"

#include "Common.h"
#include "R5900.h" // for g_GameStarted
#include <streams/file_stream.h>
#include "IopBios.h"
#include "IopMem.h"
#include "iR3000A.h"
#include "ps2/BiosTools.h"

#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(m) (((m)&S_IFMT) == S_IFREG)
#endif
#if !defined(S_ISDIR) && defined(S_IFMT) && defined(S_IFDIR)
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

typedef struct
{
	uint32_t mode;
	uint32_t attr;
	uint32_t size;
	uint8_t ctime[8];
	uint8_t atime[8];
	uint8_t mtime[8];
	uint32_t hisize;
} fio_stat_t;
typedef struct
{
	fio_stat_t _fioStat;
	/** Number of subs (main) / subpart number (sub) */
	uint32_t private_0;
	uint32_t private_1;
	uint32_t private_2;
	uint32_t private_3;
	uint32_t private_4;
	/** Sector start.  */
	uint32_t private_5;
} fxio_stat_t;

typedef struct
{
	fio_stat_t stat;
	char name[256];
	uint32_t unknown;
} fio_dirent_t;

typedef struct
{
	fxio_stat_t stat;
	char name[256];
	uint32_t unknown;
} fxio_dirent_t;

static char hostRoot[PATH_MAX_LENGTH];

/* Lexical canonicalise: a port of Path::Canonicalize, checked against it
 * in tests/path/canon_test.c over that table and 200,000 generated paths. */
static void iop_canonicalize(char *s)
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



void Hle_SetElfPath(const char* elfFileName)
{
	strlcpy(hostRoot, elfFileName, sizeof(hostRoot));
	path_basedir(hostRoot);
	/* path_basedir leaves a trailing separator; the sandbox comparison
	 * below expects the root without one. */
	{
		const size_t n = strlen(hostRoot);
		if (n > 1 && hostRoot[n - 1] == FS_OSPATH_SEPARATOR_CHARACTER)
			hostRoot[n - 1] = '\0';
	}
	Console.WriteLn("HLE Host: Set 'host:' root path to: %s\n", hostRoot);
}

void Hle_ClearElfPath()
{
	hostRoot[0] = '\0';
}

namespace R3000A
{

#define v0 (psxRegs.GPR.n.v0)
#define a0 (psxRegs.GPR.n.a0)
#define a1 (psxRegs.GPR.n.a1)
#define a2 (psxRegs.GPR.n.a2)
#define a3 (psxRegs.GPR.n.a3)
#define sp (psxRegs.GPR.n.sp)
#define ra (psxRegs.GPR.n.ra)
#define pc (psxRegs.pc)

/* Guest strings into caller buffers. iopMemReadString returns a
 * std::string and every consumer here read it back as characters; the
 * buffer form already existed in IopMem and simply was not used. */
#define Ra0_BUF(buf) iopMemReadStringBuf((buf), (int)sizeof(buf), a0, 65536)
#define Ra1_BUF(buf) iopMemReadStringBuf((buf), (int)sizeof(buf), a1, 65536)
#define Ra2 (iopMemReadString(a2))
#define Ra3 (iopMemReadString(a3))

	// Stat values differ between iomanX and ioman
	// These values have been taken from the PS2SDK
	// Specifically iox_stat.h
	struct fio_stat_flags
	{
		// Access flags
		// Execute
		int IXOTH;
		// Write
		int IWOTH;
		// Read
		int IROTH;

		// File mode flags
		// Symlink
		int IFLNK;
		// Regular file
		int IFREG;
		// Directory
		int IFDIR;
	};

	fio_stat_flags ioman_stat{
		0x01,
		0x02,
		0x04,
		0x08,
		0x10,
		0x20,
	};

	fio_stat_flags iomanx_stat{
		0x01,
		0x02,
		0x04,
		0x4000,
		0x2000,
		0x1000,
	};

	/* Resolve a guest path under hostRoot, or yield an empty string if it
	 * escapes. Writes into the caller's buffer; no allocation. */
	static void host_path(char* out, size_t out_size, const char* path, int allow_open_host_root)
	{
		char   native[PATH_MAX_LENGTH];
		char   canonical[PATH_MAX_LENGTH];
		size_t root_len;

		out[0] = '\0';
		strlcpy(native, path, sizeof(native));
		iop_canonicalize(native);

		root_len = strlen(hostRoot);
		if (root_len == 0)
			return;

		if (!strncmp(native, hostRoot, root_len))
			strlcpy(out, native, out_size);
		else
		{
			/* Join as Path::Combine does: no trailing separator on the
			 * base, no leading one on the next component, exactly one
			 * between them. A plain concat produced "root//path" whenever
			 * the guest path was absolute, and "root/" for an empty one. */
			size_t n = root_len;
			const char* tail = native;

			while (n > 0 && hostRoot[n - 1] == FS_OSPATH_SEPARATOR_CHARACTER)
				n--;
			while (*tail == FS_OSPATH_SEPARATOR_CHARACTER)
				tail++;

			if (n >= out_size)
				n = out_size - 1;
			memcpy(out, hostRoot, n);
			out[n] = '\0';
			if (*tail)
			{
				strlcat(out, "/", out_size);
				strlcat(out, tail, out_size);
			}
		}

		/* Double-check that it falls within the directory of the elf.
		 * Not a real sandbox, but emulators shouldn't be treated as such.
		 * Don't run untrusted code! */
		strlcpy(canonical, out, sizeof(canonical));
		iop_canonicalize(canonical);

		/* Opening the root of host (`host:.` or `host:`) is allowed as a
		 * directory open but not as a file open. */
		if (!allow_open_host_root || strcmp(canonical, hostRoot))
		{
			if (   strlen(canonical) <= root_len
			    || strncmp(canonical, hostRoot, root_len)
			    || canonical[root_len] != FS_OSPATH_SEPARATOR_CHARACTER)
			{
				Console.Error(
					"IopHLE: Denying access to path outside of ELF directory. Requested path: '{%s}', Resolved path: '{%s}', ELF directory: '{%s}'",
					path, out, hostRoot);
				out[0] = '\0';
			}
		}
	}

	// This is a workaround for GHS on *NIX platforms
	// Whenever a program splits directories with a backslash (ulaunchelf)
	// the directory is considered non-existant
	static __fi void clean_path(char* out, size_t out_size, const char* path)
	{
		char* p;

		strlcpy(out, path, out_size);
		for (p = out; *p; p++)
		{
			if (*p == '\\')
				*p = '/';
		}
	}

	static int host_stat(const char* path, fio_stat_t* host_stats, fio_stat_flags& stat = ioman_stat)
	{
		struct stat file_stats;
		char file_path[PATH_MAX_LENGTH];

		host_path(file_path, sizeof(file_path), path, 1);

		if (!FileSystem::StatFile(file_path, &file_stats))
			return -IOP_ENOENT;

		host_stats->size = (uint32_t)file_stats.st_size;
		host_stats->hisize = 0;

		// Convert the mode.
		host_stats->mode = (file_stats.st_mode & (stat.IROTH | stat.IWOTH | stat.IXOTH));
#ifndef _WIN32
		if (S_ISLNK(file_stats.st_mode))
			host_stats->mode |= stat.IFLNK;
#endif
		if (S_ISREG(file_stats.st_mode))
			host_stats->mode |= stat.IFREG;
		if (S_ISDIR(file_stats.st_mode))
			host_stats->mode |= stat.IFDIR;

		// Convert the creation time.
		struct tm* loctime;
		loctime = localtime(&(file_stats.st_ctime));
		host_stats->ctime[6] = (unsigned char)loctime->tm_year;
		host_stats->ctime[5] = (unsigned char)loctime->tm_mon + 1;
		host_stats->ctime[4] = (unsigned char)loctime->tm_mday;
		host_stats->ctime[3] = (unsigned char)loctime->tm_hour;
		host_stats->ctime[2] = (unsigned char)loctime->tm_min;
		host_stats->ctime[1] = (unsigned char)loctime->tm_sec;

		// Convert the access time.
		loctime = localtime(&(file_stats.st_atime));
		host_stats->atime[6] = (unsigned char)loctime->tm_year;
		host_stats->atime[5] = (unsigned char)loctime->tm_mon + 1;
		host_stats->atime[4] = (unsigned char)loctime->tm_mday;
		host_stats->atime[3] = (unsigned char)loctime->tm_hour;
		host_stats->atime[2] = (unsigned char)loctime->tm_min;
		host_stats->atime[1] = (unsigned char)loctime->tm_sec;

		// Convert the last modified time.
		loctime = localtime(&(file_stats.st_mtime));
		host_stats->mtime[6] = (unsigned char)loctime->tm_year;
		host_stats->mtime[5] = (unsigned char)loctime->tm_mon + 1;
		host_stats->mtime[4] = (unsigned char)loctime->tm_mday;
		host_stats->mtime[3] = (unsigned char)loctime->tm_hour;
		host_stats->mtime[2] = (unsigned char)loctime->tm_min;
		host_stats->mtime[1] = (unsigned char)loctime->tm_sec;

		return 0;
	}

	static int host_stat(const char* path, fxio_stat_t* host_stats)
	{
		return host_stat(path, &host_stats->_fioStat, iomanx_stat);
	}

	// TODO: sandbox option, other permissions
	class HostFile
	{
	public:
		RFILE* fp;

		HostFile(RFILE* hostfp)
		{
			fp = hostfp;
		}

		~HostFile() = default;



		/* host: I/O goes through the libretro VFS rather than open(2).
		 * The frontend can then serve these paths itself -- on Android a
		 * content:// URI is not something open(2) can take at all -- and
		 * this was the last raw POSIX file path left in the core. */
		static int open(HostFile** file, const char* full_path, s32 flags, u16 mode)
		{
			const char* colon = strchr(full_path, ':');
			char file_path[PATH_MAX_LENGTH];

			host_path(file_path, sizeof(file_path), colon ? colon + 1 : full_path, 0);
			unsigned vfs_mode;
			RFILE* fp;

			switch (flags & IOP_O_RDWR)
			{
				case IOP_O_WRONLY:
					vfs_mode = RETRO_VFS_FILE_ACCESS_WRITE;
					break;
				case IOP_O_RDWR:
					vfs_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE;
					break;
				case IOP_O_RDONLY:
				default:
					vfs_mode = RETRO_VFS_FILE_ACCESS_READ;
					break;
			}

			/* O_TRUNC is the VFS default for a writable open; without
			 * O_TRUNC the existing contents have to be kept, and O_APPEND
			 * needs them kept as well before the seek to the end below. */
			if ((vfs_mode & RETRO_VFS_FILE_ACCESS_WRITE) &&
			    !(flags & IOP_O_TRUNC))
				vfs_mode |= RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;

			/* IOP_O_CREAT is implied by a writable VFS open; a read-only
			 * open of a missing file fails below either way. */

			fp = filestream_open(file_path, vfs_mode,
					RETRO_VFS_FILE_ACCESS_HINT_NONE);
			if (!fp)
				return -IOP_ENOENT;

			if (flags & IOP_O_APPEND)
				filestream_seek(fp, 0, RETRO_VFS_SEEK_POSITION_END);

			*file = new HostFile(fp);
			if (!*file)
			{
				filestream_close(fp);
				return -IOP_ENOMEM;
			}

			return 0;
		}

		void close()
		{
			filestream_close(fp);
			delete this;
		}

		int lseek(s32 offset, s32 whence)
		{
			int seek_position;

			switch (whence)
			{
				case IOP_SEEK_SET:
					seek_position = RETRO_VFS_SEEK_POSITION_START;
					break;
				case IOP_SEEK_CUR:
					seek_position = RETRO_VFS_SEEK_POSITION_CURRENT;
					break;
				case IOP_SEEK_END:
					seek_position = RETRO_VFS_SEEK_POSITION_END;
					break;
				default:
					return -IOP_EIO;
			}

			return (int)filestream_seek(fp, offset, seek_position);
		}

		int read(void* buf, u32 count) /* Flawfinder: ignore */
		{
			const int64_t r = filestream_read(fp, buf, count);
			return (r < 0) ? -IOP_EIO : (int)r;
		}

		int write(void* buf, u32 count)
		{
			const int64_t w = filestream_write(fp, buf, count);
			return (w < 0) ? -IOP_EIO : (int)w;
		}
	};

	class HostDir
	{
	public:
		FileSystem::FindResultsArray results;
		FileSystem::FindResultsArray::iterator dir;
		char basedir[PATH_MAX_LENGTH];

		HostDir(FileSystem::FindResultsArray results_, const char* basedir_)
			: results(std::move(results_))
		{
			strlcpy(basedir, basedir_, sizeof(basedir));
			dir = results.begin();
		}

		~HostDir() = default;

		static int open(HostDir** dir, const char* full_path)
		{
			const char* colon = strchr(full_path, ':');
			char path[PATH_MAX_LENGTH];

			host_path(path, sizeof(path), colon ? colon + 1 : full_path, 1);

			if (!path_is_directory(path))
				return -IOP_ENOENT; // Should return ENOTDIR if path is a file?
			
			FileSystem::FindResultsArray results;
			FileSystem::FindFiles(path, "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_HIDDEN_FILES, &results);

			*dir = new HostDir(std::move(results), path);
			if (!*dir)
				return -IOP_ENOMEM;

			return 0;
		}

		int read(void* buf, bool iomanX) /* Flawfinder: ignore */
		{
			if (dir == results.end())
				return 0;

			if (iomanX)
			{
				fxio_dirent_t* hostcontent = (fxio_dirent_t*)buf;
				strlcpy(hostcontent->name, dir->FileName.c_str(), sizeof(hostcontent->name));
				{
					char joined[PATH_MAX_LENGTH];
					snprintf(joined, sizeof(joined), "%s/%s", basedir, dir->FileName.c_str());
					host_stat(joined, &hostcontent->stat);
				}
			}
			else
			{
				fio_dirent_t* hostcontent = (fio_dirent_t*)buf;
				strlcpy(hostcontent->name, dir->FileName.c_str(), sizeof(hostcontent->name));
				{
					char joined[PATH_MAX_LENGTH];
					snprintf(joined, sizeof(joined), "%s/%s", basedir, dir->FileName.c_str());
					host_stat(joined, &hostcontent->stat);
				}
			}

			dir = std::next(dir);
			return 1;
		}

		void close()
		{
			delete this;
		}
	};

	namespace ioman
	{
		const int firstfd = 0x100;
		const int maxfds = 0x100;
		int openfds = 0;

		int freefdcount()
		{
			return maxfds - openfds;
		}

		struct filedesc
		{
			enum
			{
				FILE_FREE,
				FILE_FILE,
				FILE_DIR,
			} type;
			union
			{
				HostFile* file;
				HostDir* dir;
			};

			constexpr filedesc()
				: type(FILE_FREE)
				, file(nullptr)
			{
			}
			operator bool() const { return type != FILE_FREE; }
			operator HostFile*() const { return type == FILE_FILE ? file : NULL; }
			operator HostDir*() const { return type == FILE_DIR ? dir : NULL; }
			void operator=(HostFile* f)
			{
				type = FILE_FILE;
				file = f;
				openfds++;
			}
			void operator=(HostDir* d)
			{
				type = FILE_DIR;
				dir = d;
				openfds++;
			}

			void close()
			{
				switch (type)
				{
					case FILE_FILE:
						file->close();
						file = NULL;
						break;
					case FILE_DIR:
						dir->close();
						dir = NULL;
						break;
					case FILE_FREE:
						return;
				}

				type = FILE_FREE;
				openfds--;
			}
		};

		filedesc fds[maxfds];

		template <typename T>
		T* getfd(int fd)
		{
			fd -= firstfd;

			if (fd < 0 || fd >= maxfds)
				return NULL;

			return fds[fd];
		}

		template <typename T>
		int allocfd(T* obj)
		{
			for (int i = 0; i < maxfds; i++)
			{
				if (!fds[i])
				{
					fds[i] = obj;
					return firstfd + i;
				}
			}

			obj->close();
			return -IOP_EMFILE;
		}

		void freefd(int fd)
		{
			fd -= firstfd;

			if (fd < 0 || fd >= maxfds)
				return;

			fds[fd].close();
		}

		void reset()
		{
			for (int i = 0; i < maxfds; i++)
			{
				if (fds[i])
					fds[i].close();
			}
		}

		/* "host:", or "host0:" through "host9...:" -- the digits between
		 * the name and the colon are a unit number and are ignored. */
		static int is_host(const char* path)
		{
			size_t i;

			if (strncmp(path, "host", 4))
				return 0;

			for (i = 4; path[i] >= '0' && path[i] <= '9'; i++)
				;
			if (path[i] != ':')
				return 0;

			return (!g_GameStarted || EmuConfig.HostFs);
		}

		int open_HLE()
		{
			HostFile* file = NULL;
			char path_raw[PATH_MAX_LENGTH];
			char path[PATH_MAX_LENGTH];
			Ra0_BUF(path_raw);
			clean_path(path, sizeof(path), path_raw);
			s32 flags = a1;
			u16 mode = a2;

			if (is_host(path))
			{
				if (!freefdcount())
				{
					v0 = -IOP_EMFILE;
					pc = ra;
					return 1;
				}

				int err = HostFile::open(&file, path, flags, mode);

				if (err != 0 || !file)
				{
					if (err == 0) // ???
						err = -IOP_EIO;
					if (file) // ??????
						file->close();
					v0 = err;
				}
				else
				{
					v0 = allocfd(file);
					if ((s32)v0 < 0)
						file->close();
				}

				pc = ra;
				return 1;
			}

			return 0;
		}

		int close_HLE()
		{
			s32 fd = a0;

			if (getfd<HostFile>(fd))
			{
				freefd(fd);
				v0 = 0;
				pc = ra;
				return 1;
			}

			return 0;
		}

		int dopen_HLE()
		{
			HostDir* dir = NULL;
			char path_raw[PATH_MAX_LENGTH];
			char path[PATH_MAX_LENGTH];
			Ra0_BUF(path_raw);
			clean_path(path, sizeof(path), path_raw);

			if (is_host(path))
			{
				int err = HostDir::open(&dir, path);

				if (err != 0 || !dir)
				{
					if (err == 0)
						err = -IOP_EIO;
					if (dir)
						dir->close();
					v0 = err;
				}
				else
				{
					v0 = allocfd(dir);
					if ((s32)v0 < 0)
						dir->close();
				}

				pc = ra;
				return 1;
			}

			return 0;
		}

		int dclose_HLE()
		{
			s32 dir = a0;

			if (getfd<HostDir>(dir))
			{
				freefd(dir);
				v0 = 0;
				pc = ra;
				return 1;
			}

			return 0;
		}

		int _dread_HLE(bool iomanX)
		{
			s32 fh = a0;
			u32 data = a1;
			if (iomanX)
			{
				if (HostDir* dir = getfd<HostDir>(fh))
				{
					char buf[sizeof(fxio_dirent_t)];
					v0 = dir->read(&buf, iomanX); /* Flawfinder: ignore */

					for (s32 i = 0; i < (s32)sizeof(fxio_dirent_t); i++)
						iopMemWrite8(data + i, buf[i]);

					pc = ra;
					return 1;
				}
			}
			else
			{
				if (HostDir* dir = getfd<HostDir>(fh))
				{
					char buf[sizeof(fio_dirent_t)];
					v0 = dir->read(&buf, false); /* Flawfinder: ignore */

					for (s32 i = 0; i < (s32)sizeof(fio_dirent_t); i++)
						iopMemWrite8(data + i, buf[i]);

					pc = ra;
					return 1;
				}
			}
			return 0;
		}

		int dread_HLE()
		{
			return _dread_HLE(false);
		}

		int dreadx_HLE()
		{
			return _dread_HLE(true);
		}

		int _getStat_HLE(bool iomanx)
		{
			char path_raw[PATH_MAX_LENGTH];
			char path[PATH_MAX_LENGTH];
			Ra0_BUF(path_raw);
			clean_path(path, sizeof(path), path_raw);
			u32 data = a1;

			if (is_host(path))
			{
				const char* colon = strchr(path, ':');
				char full_path[PATH_MAX_LENGTH];

				host_path(full_path, sizeof(full_path), colon ? colon + 1 : path, 1);
				if (iomanx)
				{
					char buf[sizeof(fxio_stat_t)];
					v0 = host_stat(full_path, (fxio_stat_t*)&buf);

					for (size_t i = 0; i < sizeof(fxio_stat_t); i++)
						iopMemWrite8((uint32_t)(data + i), buf[i]);
				}
				else
				{
					char buf[sizeof(fio_stat_t)];
					v0 = host_stat(full_path, (fio_stat_t*)&buf);

					for (size_t i = 0; i < sizeof(fio_stat_t); i++)
						iopMemWrite8((uint32_t)(data + i), buf[i]);
				}
				pc = ra;
				return 1;
			}

			return 0;
		}

		int getStat_HLE()
		{
			return _getStat_HLE(false);
		}

		int getStatx_HLE()
		{
			return _getStat_HLE(true);
		}

		int lseek_HLE()
		{
			s32 fd = a0;
			s32 offset = a1;
			s32 whence = a2;

			if (HostFile* file = getfd<HostFile>(fd))
			{
				v0 = file->lseek(offset, whence);
				pc = ra;
				return 1;
			}

			return 0;
		}

		int remove_HLE()
		{
			char full_path_raw[PATH_MAX_LENGTH];
			char full_path[PATH_MAX_LENGTH];
			Ra0_BUF(full_path_raw);
			clean_path(full_path, sizeof(full_path), full_path_raw);

			if (is_host(full_path))
			{
				const char* colon = strchr(full_path, ':');
				char file_path[PATH_MAX_LENGTH];

				host_path(file_path, sizeof(file_path), colon ? colon + 1 : full_path, 0);
				const bool succeeded = FileSystem::DeleteFilePath(file_path);
				if (!succeeded)
					Console.Warning("IOPHLE remove_HLE failed for '%s'", file_path);
				v0 = succeeded ? 0 : -IOP_EIO;
				pc = ra;
			}
			return 0;
		}

		int mkdir_HLE()
		{
			char full_path_raw[PATH_MAX_LENGTH];
			char full_path[PATH_MAX_LENGTH];
			Ra0_BUF(full_path_raw);
			clean_path(full_path, sizeof(full_path), full_path_raw);

			if (is_host(full_path))
			{
				const char* colon = strchr(full_path, ':');
				char folder_path[PATH_MAX_LENGTH];

				host_path(folder_path, sizeof(folder_path), colon ? colon + 1 : full_path, 0); // NOTE: Don't allow creating the ELF directory.
				const bool succeeded = path_mkdir(folder_path);
				if (!succeeded)
					Console.Warning("IOPHLE mkdir_HLE failed for '%s'", folder_path);
				v0 = succeeded ? 0 : -IOP_EIO;
				pc = ra;
				return 1;
			}

			return 0;
		}

		int read_HLE()
		{
			s32 fd = a0;
			u32 data = a1;
			u32 count = a2;

			if (HostFile* file = getfd<HostFile>(fd))
			{
				auto buf = std::make_unique<char[]>(count);

				v0 = file->read(buf.get(), count);

				for (s32 i = 0; i < (s32)v0; i++)
					iopMemWrite8(data + i, buf[i]);

				pc = ra;
				return 1;
			}

			return 0;
		}

		int rmdir_HLE()
		{
			char full_path_raw[PATH_MAX_LENGTH];
			char full_path[PATH_MAX_LENGTH];
			Ra0_BUF(full_path_raw);
			clean_path(full_path, sizeof(full_path), full_path_raw);

			if (is_host(full_path))
			{
				const char* colon = strchr(full_path, ':');
				char folder_path[PATH_MAX_LENGTH];

				host_path(folder_path, sizeof(folder_path), colon ? colon + 1 : full_path, 0); // NOTE: Don't allow removing the elf directory itself.
				const bool succeeded = FileSystem::DeleteDirectory(folder_path);
				if (!succeeded)
					Console.Warning("IOPHLE rmdir_HLE failed for '%s'", folder_path);
				v0 = succeeded ? 0 : -IOP_EIO;
				pc = ra;
				return 1;
			}

			return 0;
		}

		int write_HLE()
		{
			s32 fd = a0;
			u32 data = a1;
			u32 count = a2;

			if (fd == 1) // stdout
			{
				char s[PATH_MAX_LENGTH];
				Ra1_BUF(s);
				pc = ra;
				v0 = a2;
				return 1;
			}
			else if (HostFile* file = getfd<HostFile>(fd))
			{
				auto buf = std::make_unique<char[]>(count);

				for (u32 i = 0; i < count; i++)
					buf[i] = iopMemRead8(data + i);

				v0 = file->write(buf.get(), count);

				pc = ra;
				return 1;
			}

			return 0;
		}
	} // namespace ioman

	namespace sysmem
	{
		int Kprintf_HLE()
		{
			// Emulate the expected Kprintf functionality:
			iopMemWrite32(sp, a0);
			iopMemWrite32(sp + 4, a1);
			iopMemWrite32(sp + 8, a2);
			iopMemWrite32(sp + 12, a3);
			pc = ra;

			return 1;
		}
	} // namespace sysmem

	namespace loadcore
	{

		int RegisterLibraryEntries_HLE()
		{
			return 0;
		}

		void RegisterLibraryEntries_DEBUG()
		{
		}
	} // namespace loadcore

	namespace intrman
	{
		void RegisterIntrHandler_DEBUG()
		{
		}
	} // namespace intrman

	namespace sifcmd
	{
		void sceSifRegisterRpc_DEBUG()
		{
		}
	} // namespace sifcmd

	u32 irxImportTableAddr(u32 entrypc)
	{
		u32 i;

		i = entrypc - 0x18;
		while (entrypc - i < 0x2000)
		{
			if (iopMemRead32(i) == 0x41e00000)
				return i;
			i -= 4;
		}

		return 0;
	}

	const char* irxImportFuncname(const std::string& libname, u16 index)
	{
#include "IopModuleNames.cpp"

		switch (index)
		{
			case 0:
				return "start";
			// case 1: reinit?
			case 2:
				return "shutdown";
				// case 3: ???
		}

		return 0;
	}

// clang-format off
#define MODULE(n)          \
	if (#n == libname)     \
	{                      \
		using namespace n; \
		switch (index)     \
		{
#define END_MODULE \
	}              \
	}
#define EXPORT_D(i, n) \
	case (i):          \
		return n##_DEBUG;
#define EXPORT_H(i, n) \
	case (i):          \
		return n##_HLE;
	// clang-format on

	irxHLE irxImportHLE(const std::string& libname, u16 index)
	{
		// debugging output
		// clang-format off
		MODULE(sysmem)
			EXPORT_H( 14, Kprintf)
		END_MODULE

		// For grabbing the thread list from thbase
		MODULE(loadcore)
			EXPORT_H( 6, RegisterLibraryEntries)
		END_MODULE

		// Special case with ioman and iomanX
		// They are mostly compatible excluding stat structures
		if(libname == "ioman" || libname == "iomanx")
		{
			const bool use_ioman = libname == "ioman";
			using namespace ioman;
				switch(index)
				{
					EXPORT_H(  4, open)
					EXPORT_H(  5, close)
					EXPORT_H(  6, read)
					EXPORT_H(  7, write)
					EXPORT_H(  8, lseek)
					EXPORT_H( 10, remove)
					EXPORT_H( 11, mkdir)
					EXPORT_H( 12, rmdir)
					EXPORT_H( 13, dopen)
					EXPORT_H( 14, dclose)
					case 15: // dread
					if(use_ioman)
						return dread_HLE;
					else
						return dreadx_HLE;
					case 16: // getStat
					if(use_ioman)
						return getStat_HLE;
					else
						return getStatx_HLE;
				}
		}
		// clang-format on
		return 0;
	}

	irxDEBUG irxImportDebug(const std::string& libname, u16 index)
	{
		// clang-format off
		MODULE(loadcore)
			EXPORT_D(  6, RegisterLibraryEntries)
		END_MODULE
		MODULE(intrman)
			EXPORT_D(  4, RegisterIntrHandler)
		END_MODULE
		MODULE(sifcmd)
			EXPORT_D( 17, sceSifRegisterRpc)
		END_MODULE
		// clang-format off

		return 0;
	}

#undef MODULE
#undef END_MODULE
#undef EXPORT_D
#undef EXPORT_H

	void irxImportLog(const std::string& libname, u16 index, const char* funcname)
	{
	}

	void irxImportLog_rec(u32 import_table, u16 index, const char* funcname)
	{
	}

	int irxImportExec(u32 import_table, u16 index)
	{
		if (!import_table)
			return 0;

		std::string libname = iopMemReadString(import_table + 12, 8);
		irxHLE hle          = irxImportHLE(libname, index);
		irxDEBUG debug      = irxImportDebug(libname, index);

		if (debug)
			debug();

		if (hle)
			return hle();
		return 0;
	}

	// C.71: per-stub cache of the import resolution. psxJ runs the module-import
	// stub through irxImportExec on EVERY execution -- a backward scan of up to
	// 0x2000 bytes for the 0x41e00000 table magic, a std::string heap allocation
	// for the library name, and a string-compare chain, millions of times per
	// minute (this is the one J form the arm64 IOP JIT leaves to the interpreter
	// for exactly this hook). The resolution depends only on the table magic,
	// the 8-byte libname and the index, so cache it per stub pc and revalidate
	// those inputs with three RAM reads per hit. A module loading over the stub
	// rewrites name/magic and fails revalidation -> full re-resolve. A stub with
	// NO table (scan miss) is not cached: there is nothing stable to revalidate
	// against.
	namespace
	{
		struct JStubCacheEntry { u32 table, name0, name1; u16 index; irxHLE hle; irxDEBUG debug; };
		std::unordered_map<u32, JStubCacheEntry> s_jstub_cache;
	}

	int irxImportExecCached(u32 stubpc, u16 index)
	{
		auto it = s_jstub_cache.find(stubpc);
		if (it != s_jstub_cache.end())
		{
			const JStubCacheEntry& e = it->second;
			if (e.index == index &&
				iopMemRead32(e.table) == 0x41e00000 &&
				iopMemRead32(e.table + 12) == e.name0 &&
				iopMemRead32(e.table + 16) == e.name1)
			{
				if (e.debug)
					e.debug();
				if (e.hle)
					return e.hle();
				return 0;
			}
			s_jstub_cache.erase(it);
		}

		const u32 table = irxImportTableAddr(stubpc);
		if (!table)
			return 0;

		JStubCacheEntry e;
		e.table = table;
		e.name0 = iopMemRead32(table + 12);
		e.name1 = iopMemRead32(table + 16);
		e.index = index;
		std::string libname = iopMemReadString(table + 12, 8);
		e.hle   = irxImportHLE(libname, index);
		e.debug = irxImportDebug(libname, index);
		s_jstub_cache.emplace(stubpc, e);

		if (e.debug)
			e.debug();
		if (e.hle)
			return e.hle();
		return 0;
	}

} // end namespace R3000A

namespace R3000A
{
	irxHLE irxImportHLECh(const char* libname, u16 index)
	{
		return irxImportHLE(std::string(libname), index);
	}
} // namespace R3000A
