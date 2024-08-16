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
#include <sys/stat.h>

#include <fcntl.h>

#include <file/file_path.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "Common.h"
#include "R5900.h" /* for g_GameStarted */
#include "IopBios.h"
#include "IopMem.h"
#include "iR3000A.h"
#include "BiosTools.h"
#include "DebugTools/SymbolMap.h"

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
	unsigned int mode;
	unsigned int attr;
	unsigned int size;
	unsigned char ctime[8];
	unsigned char atime[8];
	unsigned char mtime[8];
	unsigned int hisize;
} fio_stat_t;
typedef struct
{
	fio_stat_t _fioStat;
	/** Number of subs (main) / subpart number (sub) */
	unsigned int private_0;
	unsigned int private_1;
	unsigned int private_2;
	unsigned int private_3;
	unsigned int private_4;
	/** Sector start.  */
	unsigned int private_5;
} fxio_stat_t;

typedef struct
{
	fio_stat_t stat;
	char name[256];
	unsigned int unknown;
} fio_dirent_t;

typedef struct
{
	fxio_stat_t stat;
	char name[256];
	unsigned int unknown;
} fxio_dirent_t;

static std::string hostRoot;

void Hle_SetElfPath(const char* elfFileName)
{
	hostRoot = Path::ToNativePath(Path::GetDirectory(elfFileName));
	Console.WriteLn("HLE Host: Set 'host:' root path to: %s\n", hostRoot.c_str());
}

void Hle_ClearElfPath()
{
	hostRoot = {};
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

#define Ra0 (iopMemReadString(a0))
#define Ra1 (iopMemReadString(a1))
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

	static std::string host_path(const std::string& path, bool allow_open_host_root)
	{
		/* We are NOT allowing to use the root of the host unit.
		 * For now it just supports relative folders from the location of the ELF */
		std::string native_path(Path::Canonicalize(path));
		std::string new_path;
		if (!hostRoot.empty() && StringUtil::StartsWith(native_path, hostRoot))
			new_path = std::move(native_path);
		else if (!hostRoot.empty()) /* relative paths */
			new_path = Path::Combine(hostRoot, native_path);

		/* Double-check that it falls within the directory of the elf.
		 * Not a real sandbox, but emulators shouldn't be treated as such. Don't run untrusted code! */
		std::string canonicalized_path(Path::Canonicalize(new_path));

		/* Are we opening the root of host? (i.e. `host:.` or `host:`)
		 * We want to allow this as a directory open, but not as a file open. */
		if (!allow_open_host_root || canonicalized_path != hostRoot)
		{
			/* Only allow descendants of the hostfs directory. */
			if (canonicalized_path.length() <= hostRoot.length() || // Length has to be equal or longer,
				!StringUtil::StartsWith(canonicalized_path, hostRoot) || // and start with the host root,
				canonicalized_path[hostRoot.length()] != FS_OSPATH_SEPARATOR_CHARACTER) // and we can't access a sibling.
			{
				Console.Error(
					"IopHLE: Denying access to path outside of ELF directory. Requested path: '{%s}', Resolved path: '{%s}', ELF directory: '{%s}'",
					path.c_str(), new_path.c_str(), hostRoot.c_str());
				new_path.clear();
			}
		}

		return new_path;
	}

	// This is a workaround for GHS on *NIX platforms
	// Whenever a program splits directories with a backslash (ulaunchelf)
	// the directory is considered non-existant
	static __fi std::string clean_path(const std::string path)
	{
		std::string ret = path;
		std::replace(ret.begin(), ret.end(), '\\', '/');
		return ret;
	}

	static int host_stat(const std::string path, fio_stat_t* host_stats, fio_stat_flags& stat = ioman_stat)
	{
		struct stat file_stats;
		const std::string file_path(host_path(path, true));

		if (!FileSystem::StatFile(file_path.c_str(), &file_stats))
			return -IOP_ENOENT;

		host_stats->size = file_stats.st_size;
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

	static int host_stat(const std::string path, fxio_stat_t* host_stats)
	{
		return host_stat(path, &host_stats->_fioStat, iomanx_stat);
	}

	// TODO: sandbox option, other permissions
	class HostFile : public IOManFile
	{
	public:
		int fd;

		HostFile(int hostfd)
		{
			fd = hostfd;
		}

		virtual ~HostFile() = default;

		static __fi int translate_error(int err)
		{
			if (err >= 0)
				return err;

			switch (err)
			{
				case -ENOENT:
					return -IOP_ENOENT;
				case -EACCES:
					return -IOP_EACCES;
				case -EISDIR:
					return -IOP_EISDIR;
				case -EIO:
				default:
					return -IOP_EIO;
			}
		}

		static int open(IOManFile** file, const std::string& full_path, s32 flags, u16 mode)
		{
			const std::string path(full_path.substr(full_path.find(':') + 1));
			const std::string file_path(host_path(path, false));
			int native_flags = O_BINARY; /* necessary in Windows. */

			switch (flags & IOP_O_RDWR)
			{
				case IOP_O_RDONLY:
					native_flags |= O_RDONLY;
					break;
				case IOP_O_WRONLY:
					native_flags |= O_WRONLY;
					break;
				case IOP_O_RDWR:
					native_flags |= O_RDWR;
					break;
			}

			if (flags & IOP_O_APPEND)
				native_flags |= O_APPEND;
			if (flags & IOP_O_CREAT)
				native_flags |= O_CREAT;
			if (flags & IOP_O_TRUNC)
				native_flags |= O_TRUNC;

#ifdef _WIN32
			const int native_mode = _S_IREAD | _S_IWRITE;
#else
			const int native_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
#endif

			const int hostfd = FileSystem::OpenFDFile(file_path.c_str(), native_flags, native_mode);
			if (hostfd < 0)
				return translate_error(hostfd);

			*file = new HostFile(hostfd);
			if (!*file)
				return -IOP_ENOMEM;

			return 0;
		}

		virtual void close()
		{
			::close(fd);
			delete this;
		}

		virtual int lseek(s32 offset, s32 whence)
		{
			int err;

			switch (whence)
			{
				case IOP_SEEK_SET:
					err = ::lseek(fd, offset, SEEK_SET);
					break;
				case IOP_SEEK_CUR:
					err = ::lseek(fd, offset, SEEK_CUR);
					break;
				case IOP_SEEK_END:
					err = ::lseek(fd, offset, SEEK_END);
					break;
				default:
					return -IOP_EIO;
			}

			return translate_error(err);
		}

		virtual int read(void* buf, u32 count) /* Flawfinder: ignore */
		{
			return translate_error(::read(fd, buf, count));
		}

		virtual int write(void* buf, u32 count)
		{
			return translate_error(::write(fd, buf, count));
		}
	};

	class HostDir : public IOManDir
	{
	public:
		FileSystem::FindResultsArray results;
		FileSystem::FindResultsArray::iterator dir;
		std::string basedir;

		HostDir(FileSystem::FindResultsArray results_, std::string basedir_)
			: results(std::move(results_))
			, basedir(std::move(basedir_))
		{
			dir = results.begin();
		}

		virtual ~HostDir() = default;

		static int open(IOManDir** dir, const std::string& full_path)
		{
			std::string relativePath = full_path.substr(full_path.find(':') + 1);
			std::string path = host_path(relativePath, true);

			if (!path_is_directory(path.c_str()))
				return -IOP_ENOENT; /* Should return ENOTDIR if path is a file? */
			
			FileSystem::FindResultsArray results;
			FileSystem::FindFiles(path.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_HIDDEN_FILES, &results);

			*dir = new HostDir(std::move(results), std::move(path));
			if (!*dir)
				return -IOP_ENOMEM;

			return 0;
		}

		virtual int read(void* buf, bool iomanX) /* Flawfinder: ignore */
		{
			if (dir == results.end())
				return 0;

			if (iomanX)
			{
				fxio_dirent_t* hostcontent = (fxio_dirent_t*)buf;
				strcpy(hostcontent->name, dir->FileName.c_str());
				host_stat(host_path(Path::Combine(basedir, dir->FileName), true), &hostcontent->stat);
			}
			else
			{
				fio_dirent_t* hostcontent = (fio_dirent_t*)buf;
				strcpy(hostcontent->name, dir->FileName.c_str());
				host_stat(host_path(Path::Combine(basedir, dir->FileName), true), &hostcontent->stat);
			}

			dir = std::next(dir);
			return 1;
		}

		virtual void close()
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
				IOManFile* file;
				IOManDir* dir;
			};

			constexpr filedesc()
				: type(FILE_FREE)
				, file(nullptr)
			{
			}
			operator bool() const { return type != FILE_FREE; }
			operator IOManFile*() const { return type == FILE_FILE ? file : NULL; }
			operator IOManDir*() const { return type == FILE_DIR ? dir : NULL; }
			void operator=(IOManFile* f)
			{
				type = FILE_FILE;
				file = f;
				openfds++;
			}
			void operator=(IOManDir* d)
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

		bool is_host(const std::string path)
		{
			auto not_number_pos = path.find_first_not_of("0123456789", 4);
			if (not_number_pos == std::string::npos)
				return false;

			return ((!g_GameStarted || EmuConfig.HostFs) && 0 == path.compare(0, 4, "host") && path[not_number_pos] == ':');
		}

		int open_HLE()
		{
			IOManFile* file = NULL;
			const std::string path = clean_path(Ra0);
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
					if (err == 0) /* ??? */
						err = -IOP_EIO;
					if (file) /* ?????? */
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

			if (getfd<IOManFile>(fd))
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
			IOManDir* dir = NULL;
			const std::string path = clean_path(Ra0);

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

			if (getfd<IOManDir>(dir))
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
				if (IOManDir* dir = getfd<IOManDir>(fh))
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
				if (IOManDir* dir = getfd<IOManDir>(fh))
				{
					char buf[sizeof(fio_dirent_t)];
					v0 = dir->read(&buf); /* Flawfinder: ignore */

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
			const std::string path = clean_path(Ra0);
			u32 data = a1;

			if (is_host(path))
			{
				const std::string full_path = host_path(path.substr(path.find(':') + 1), true);
				if (iomanx)
				{
					char buf[sizeof(fxio_stat_t)];
					v0 = host_stat(full_path, (fxio_stat_t*)&buf);

					for (size_t i = 0; i < sizeof(fxio_stat_t); i++)
						iopMemWrite8(data + i, buf[i]);
				}
				else
				{
					char buf[sizeof(fio_stat_t)];
					v0 = host_stat(full_path, (fio_stat_t*)&buf);

					for (size_t i = 0; i < sizeof(fio_stat_t); i++)
						iopMemWrite8(data + i, buf[i]);
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

			if (IOManFile* file = getfd<IOManFile>(fd))
			{
				v0 = file->lseek(offset, whence);
				pc = ra;
				return 1;
			}

			return 0;
		}

		int remove_HLE()
		{
			const std::string full_path = clean_path(Ra0);

			if (is_host(full_path))
			{
				const std::string path = full_path.substr(full_path.find(':') + 1);
				const std::string file_path(host_path(path, false));
				const bool succeeded = FileSystem::DeleteFilePath(file_path.c_str());
				if (!succeeded)
					Console.Warning("IOPHLE remove_HLE failed for '%s'", file_path.c_str());
				v0 = succeeded ? 0 : -IOP_EIO;
				pc = ra;
			}
			return 0;
		}

		int mkdir_HLE()
		{
			const std::string full_path = clean_path(Ra0);

			if (is_host(full_path))
			{
				const std::string path = full_path.substr(full_path.find(':') + 1);
				const std::string folder_path(host_path(path, false)); /* NOTE: Don't allow creating the ELF directory. */
				const bool succeeded = path_mkdir(folder_path.c_str());
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

			if (IOManFile* file = getfd<IOManFile>(fd))
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
			const std::string full_path = clean_path(Ra0);

			if (is_host(full_path))
			{
				const std::string path = full_path.substr(full_path.find(':') + 1);
				const std::string folder_path(host_path(path, false)); /* NOTE: Don't allow removing the elf directory itself. */
				const bool succeeded = FileSystem::DeleteDirectory(folder_path.c_str());
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

			if (fd == 1) /* stdout */
			{
				const std::string s = Ra1;
				pc = ra;
				v0 = a2;
				return 1;
			}
			else if (IOManFile* file = getfd<IOManFile>(fd))
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
	}

	namespace sysmem
	{
		int Kprintf_HLE()
		{
			/* Emulate the expected Kprintf functionality: */
			iopMemWrite32(sp, a0);
			iopMemWrite32(sp + 4, a1);
			iopMemWrite32(sp + 8, a2);
			iopMemWrite32(sp + 12, a3);
			pc = ra;

			return 1;
		}
	}

	namespace loadcore
	{

		int RegisterLibraryEntries_HLE()
		{
			return 0;
		}

		void RegisterLibraryEntries_DEBUG()
		{
		}
	}

	namespace intrman
	{
		void RegisterIntrHandler_DEBUG()
		{
		}
	}

	namespace sifcmd
	{
		void sceSifRegisterRpc_DEBUG()
		{
		}
	}

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
#define MODULE(n) if (#n == libname) switch (index) {
#define END_MODULE }
#define EXPORT(i, n) case (i): return #n;

// machine generated
MODULE(cdvdman)
	EXPORT(  4, sceCdInit)
	EXPORT(  5, sceCdStandby)
	EXPORT(  6, sceCdRead)
	EXPORT(  7, sceCdSeek)
	EXPORT(  8, sceCdGetError)
	EXPORT(  9, sceCdGetToc)
	EXPORT( 10, sceCdSearchFile)
	EXPORT( 11, sceCdSync)
	EXPORT( 12, sceCdGetDiskType)
	EXPORT( 13, sceCdDiskReady)
	EXPORT( 14, sceCdTrayReq)
	EXPORT( 15, sceCdStop)
	EXPORT( 16, sceCdPosToInt)
	EXPORT( 17, sceCdIntToPos)
	EXPORT( 21, sceCdCheckCmd)
	EXPORT( 22, _sceCdRI)
	EXPORT( 24, sceCdReadClock)
	EXPORT( 28, sceCdStatus)
	EXPORT( 29, sceCdApplySCmd)
	EXPORT( 37, sceCdCallback)
	EXPORT( 38, sceCdPause)
	EXPORT( 39, sceCdBreak)
	EXPORT( 40, sceCdReadCDDA)
	EXPORT( 44, sceCdGetReadPos)
	EXPORT( 45, sceCdCtrlADout)
	EXPORT( 46, sceCdNop)
	EXPORT( 47, _sceGetFsvRbuf)
	EXPORT( 48, _sceCdstm0Cb)
	EXPORT( 49, _sceCdstm1Cb)
	EXPORT( 50, _sceCdSC)
	EXPORT( 51, _sceCdRC)
	EXPORT( 54, sceCdApplyNCmd)
	EXPORT( 56, sceCdStInit)
	EXPORT( 57, sceCdStRead)
	EXPORT( 58, sceCdStSeek)
	EXPORT( 59, sceCdStStart)
	EXPORT( 60, sceCdStStat)
	EXPORT( 61, sceCdStStop)
	EXPORT( 62, sceCdRead0)
	EXPORT( 63, _sceCdRV)
	EXPORT( 64, _sceCdRM)
	EXPORT( 66, sceCdReadChain)
	EXPORT( 67, sceCdStPause)
	EXPORT( 68, sceCdStResume)
	EXPORT( 74, sceCdPowerOff)
	EXPORT( 75, sceCdMmode)
	EXPORT( 77, sceCdStSeekF)
	EXPORT( 78, sceCdPOffCallback)
	EXPORT( 81, _sceCdSetTimeout)
	EXPORT( 83, sceCdReadDvdDualInfo)
	EXPORT( 84, sceCdLayerSearchFile)
	EXPORT(112, sceCdApplySCmd2)
	EXPORT(114, _sceCdRE)
END_MODULE
MODULE(deci2api)
	EXPORT(  4, sceDeci2Open)
	EXPORT(  5, sceDeci2Close)
	EXPORT(  6, sceDeci2ExRecv)
	EXPORT(  7, sceDeci2ExSend)
	EXPORT(  8, sceDeci2ReqSend)
	EXPORT(  9, sceDeci2ExReqSend)
	EXPORT( 10, sceDeci2ExLock)
	EXPORT( 11, sceDeci2ExUnLock)
	EXPORT( 12, sceDeci2ExPanic)
	EXPORT( 13, sceDeci2Poll)
	EXPORT( 14, sceDeci2ExPoll)
	EXPORT( 15, sceDeci2ExRecvSuspend)
	EXPORT( 16, sceDeci2ExRecvUnSuspend)
	EXPORT( 17, sceDeci2ExWakeupThread)
	EXPORT( 18, sceDeci2ExSignalSema)
	EXPORT( 19, sceDeci2ExSetEventFlag)
END_MODULE
MODULE(eenetctl)
	EXPORT(  4, sceEENetCtlSetConfiguration)
	EXPORT(  5, sceEENetCtlRegisterDialCnf)
	EXPORT(  6, sceEENetCtlUnRegisterDialCnf)
	EXPORT(  7, sceEENetCtlSetDialingData)
	EXPORT(  8, sceEENetCtlClearDialingData)
END_MODULE
MODULE(ent_devm)
	EXPORT(  4, sceEENetDevAttach)
	EXPORT(  5, sceEENetDevReady)
	EXPORT(  6, sceEENetDevDetach)
	EXPORT(  7, sceEENetSifAddCmdHandler)
	EXPORT(  8, sceEENetSifRemoveCmdHandler)
	EXPORT(  9, sceEENetSifSendCmd)
	EXPORT( 10, sceEENetSifBindRpc)
	EXPORT( 11, sceEENetSifCallRpc)
	EXPORT( 12, sceEENetCheckWaitingDriverList)
	EXPORT( 13, sceEENetCheckTerminatedDriverList)
END_MODULE
MODULE(excepman)
	EXPORT(  4, RegisterExceptionHandler)
	EXPORT(  5, RegisterPriorityExceptionHandler)
	EXPORT(  6, RegisterDefaultExceptionHandler)
	EXPORT(  7, ReleaseExceptionHandler)
	EXPORT(  8, ReleaseDefaultExceptionHandler)
END_MODULE
MODULE(heaplib)
	EXPORT(  4, CreateHeap)
	EXPORT(  5, DeleteHeap)
	EXPORT(  6, AllocHeapMemory)
	EXPORT(  7, FreeHeapMemory)
	EXPORT(  8, HeapTotalFreeSize)
END_MODULE
MODULE(ilink)
	EXPORT(  0, sce1394SetupModule)
	EXPORT(  2, sce1394ReleaseModule)
	EXPORT(  4, sce1394Initialize)
	EXPORT(  5, sce1394Destroy)
	EXPORT(  6, sce1394Debug)
	EXPORT(  7, sce1394ConfGet)
	EXPORT(  8, sce1394ConfSet)
	EXPORT(  9, sce1394ChangeThreadPriority)
	EXPORT( 12, sce1394UnitAdd)
	EXPORT( 13, sce1394UnitDelete)
	EXPORT( 17, sce1394GenerateCrc32)
	EXPORT( 18, sce1394GenerateCrc16)
	EXPORT( 19, sce1394ValidateCrc16)
	EXPORT( 23, sce1394SbControl)
	EXPORT( 24, sce1394SbEnable)
	EXPORT( 25, sce1394SbDisable)
	EXPORT( 26, sce1394SbReset)
	EXPORT( 27, sce1394SbEui64)
	EXPORT( 28, sce1394SbNodeId)
	EXPORT( 29, sce1394SbNodeCount)
	EXPORT( 30, sce1394SbSelfId)
	EXPORT( 31, sce1394SbGenNumber)
	EXPORT( 32, sce1394SbPhyPacket)
	EXPORT( 33, sce1394SbCycleTime)
	EXPORT( 36, sce1394EvAlloc)
	EXPORT( 37, sce1394EvFree)
	EXPORT( 38, sce1394EvWait)
	EXPORT( 39, sce1394EvPoll)
	EXPORT( 43, sce1394PbAlloc)
	EXPORT( 44, sce1394PbFree)
	EXPORT( 45, sce1394PbGet)
	EXPORT( 46, sce1394PbSet)
	EXPORT( 50, sce1394TrDataInd)
	EXPORT( 51, sce1394TrDataUnInd)
	EXPORT( 55, sce1394TrAlloc)
	EXPORT( 56, sce1394TrFree)
	EXPORT( 57, sce1394TrGet)
	EXPORT( 58, sce1394TrSet)
	EXPORT( 59, sce1394TrWrite)
	EXPORT( 60, sce1394TrWriteV)
	EXPORT( 61, sce1394TrRead)
	EXPORT( 62, sce1394TrReadV)
	EXPORT( 63, sce1394TrLock)
	EXPORT( 67, sce1394CrEui64)
	EXPORT( 68, sce1394CrGenNumber)
	EXPORT( 69, sce1394CrMaxRec)
	EXPORT( 70, sce1394CrMaxSpeed)
	EXPORT( 71, sce1394CrRead)
	EXPORT( 72, sce1394CrCapability)
	EXPORT( 73, sce1394CrFindNode)
	EXPORT( 74, sce1394CrFindUnit)
	EXPORT( 75, sce1394CrInvalidate)
END_MODULE
MODULE(ilsocket)
	EXPORT(  0, sceILsockModuleInit)
	EXPORT(  2, sceILsockModuleReset)
	EXPORT(  4, sceILsockInit)
	EXPORT(  5, sceILsockReset)
	EXPORT(  8, sceILsockOpen)
	EXPORT(  9, sceILsockClose)
	EXPORT( 10, sceILsockBind)
	EXPORT( 11, sceILsockConnect)
	EXPORT( 12, sceILsockSend)
	EXPORT( 13, sceILsockSendTo)
	EXPORT( 14, sceILsockRecv)
	EXPORT( 15, sceILsockRecvFrom)
	EXPORT( 18, sceILsockHtoNl)
	EXPORT( 19, sceILsockHtoNs)
	EXPORT( 20, sceILsockNtoHl)
	EXPORT( 21, sceILsockNtoHs)
	EXPORT( 22, sce1394GetCycleTimeV)
END_MODULE
MODULE(inet)
	EXPORT(  4, sceInetName2Address)
	EXPORT(  5, sceInetAddress2String)
	EXPORT(  6, sceInetCreate)
	EXPORT(  7, sceInetOpen)
	EXPORT(  8, sceInetClose)
	EXPORT(  9, sceInetRecv)
	EXPORT( 10, sceInetSend)
	EXPORT( 11, sceInetAbort)
	EXPORT( 12, sceInetRecvFrom)
	EXPORT( 13, sceInetSendTo)
	EXPORT( 14, sceInetAddress2Name)
	EXPORT( 15, sceInetControl)
	EXPORT( 16, sceInetPoll)
	EXPORT( 17, sceInetNtohs)
	EXPORT( 18, sceInetHtons)
	EXPORT( 19, sceInetNtohl)
	EXPORT( 20, sceInetHtonl)
	EXPORT( 21, sceInetGet4u)
	EXPORT( 22, sceInetPut4u)
	EXPORT( 24, sceInetGetInterfaceList)
	EXPORT( 25, sceInetInterfaceControl)
	EXPORT( 27, sceInetGetRoutingTable)
	EXPORT( 28, sceInetAddRouting)
	EXPORT( 29, sceInetDelRouting)
	EXPORT( 30, sceInetGetNameServers)
	EXPORT( 31, sceInetAddNameServer)
	EXPORT( 32, sceInetDelNameServer)
	EXPORT( 36, sceInetChangeThreadPriority)
	EXPORT( 38, sceInetGetLog)
	EXPORT( 39, sceInetWaitInterfaceEvent)
	EXPORT( 40, sceInetSignalInterfaceEvent)
	EXPORT( 41, sceInetAbortLog)
END_MODULE
MODULE(inetctl)
	EXPORT(  4, sceInetCtlSetConfiguration)
	EXPORT(  5, sceInetCtlUpInterface)
	EXPORT(  6, sceInetCtlDownInterface)
	EXPORT(  7, sceInetCtlSetAutoMode)
	EXPORT(  8, sceInetCtlRegisterEventHandler)
	EXPORT(  9, sceInetCtlUnregisterEventHandler)
	EXPORT( 10, sceInetCtlGetState)
	EXPORT( 11, sceInetCtlGetConfiguration)
	EXPORT( 12, sceInetCtlSetDialingData)
	EXPORT( 13, sceInetCtlClearDialingData)
END_MODULE
MODULE(intrman)
	EXPORT(  4, RegisterIntrHandler)
	EXPORT(  5, ReleaseIntrHandler)
	EXPORT(  6, EnableIntr)
	EXPORT(  7, DisableIntr)
	EXPORT(  8, CpuDisableIntr)
	EXPORT(  9, CpuEnableIntr)
	EXPORT( 17, CpuSuspendIntr)
	EXPORT( 18, CpuResumeIntr)
	EXPORT( 23, QueryIntrContext)
	EXPORT( 24, QueryIntrStack)
	EXPORT( 25, iCatchMultiIntr)
END_MODULE
MODULE(ioman)
	EXPORT(  4, open)
	EXPORT(  5, close)
	EXPORT(  6, read)
	EXPORT(  7, write)
	EXPORT(  8, lseek)
	EXPORT(  9, ioctl)
	EXPORT( 10, remove)
	EXPORT( 11, mkdir)
	EXPORT( 12, rmdir)
	EXPORT( 13, dopen)
	EXPORT( 14, dclose)
	EXPORT( 15, dread)
	EXPORT( 16, getstat)
	EXPORT( 17, chstat)
	EXPORT( 18, format)
	EXPORT( 20, AddDrv)
	EXPORT( 21, DelDrv)
	EXPORT( 23, StdioInit)
	EXPORT( 25, rename)
	EXPORT( 26, chdir)
	EXPORT( 27, sync)
	EXPORT( 28, mount)
	EXPORT( 29, umount)
	EXPORT( 30, lseek64)
	EXPORT( 31, devctl)
	EXPORT( 32, symlink)
	EXPORT( 33, readlink)
	EXPORT( 34, ioctl2)
END_MODULE
MODULE(libsd)
	EXPORT(  2, sceSdQuit)
	EXPORT(  4, sceSdInit)
	EXPORT(  5, sceSdSetParam)
	EXPORT(  6, sceSdGetParam)
	EXPORT(  7, sceSdSetSwitch)
	EXPORT(  8, sceSdGetSwitch)
	EXPORT(  9, sceSdSetAddr)
	EXPORT( 10, sceSdGetAddr)
	EXPORT( 11, sceSdSetCoreAttr)
	EXPORT( 12, sceSdGetCoreAttr)
	EXPORT( 13, sceSdNote2Pitch)
	EXPORT( 14, sceSdPitch2Note)
	EXPORT( 15, sceSdProcBatch)
	EXPORT( 16, sceSdProcBatchEx)
	EXPORT( 17, sceSdVoiceTrans)
	EXPORT( 18, sceSdBlockTrans)
	EXPORT( 19, sceSdVoiceTransStatus)
	EXPORT( 20, sceSdBlockTransStatus)
	EXPORT( 21, sceSdSetTransCallback)
	EXPORT( 22, sceSdSetIRQCallback)
	EXPORT( 23, sceSdSetEffectAttr)
	EXPORT( 24, sceSdGetEffectAttr)
	EXPORT( 25, sceSdClearEffectWorkArea)
	EXPORT( 26, sceSdSetTransIntrHandler)
	EXPORT( 27, sceSdSetSpu2IntrHandler)
	EXPORT( 28, sceSdGetTransIntrHandlerArgument)
	EXPORT( 29, sceSdGetSpu2IntrHandlerArgument)
	EXPORT( 30, sceSdStopTrans)
	EXPORT( 31, sceSdCleanEffectWorkArea)
	EXPORT( 32, sceSdSetEffectMode)
	EXPORT( 33, sceSdSetEffectModeParams)
END_MODULE
MODULE(loadcore)
	EXPORT(  4, FlushIcache)
	EXPORT(  5, FlushDcache)
	EXPORT(  6, RegisterLibraryEntries)
	EXPORT(  7, ReleaseLibraryEntries)
	EXPORT( 10, RegisterNonAutoLinkEntries)
	EXPORT( 11, QueryLibraryEntryTable)
	EXPORT( 12, QueryBootMode)
	EXPORT( 13, RegisterBootMode)
	EXPORT( 27, SetRebootTimeLibraryHandlingMode)
END_MODULE
MODULE(moddelay)
	EXPORT(  4, sceMidiDelay_Init)
	EXPORT(  5, sceMidiDelay_ATick)
	EXPORT(  6, sceMidiDelay_Flush)
END_MODULE
MODULE(modem)
	EXPORT(  4, sceModemRegisterDevice)
	EXPORT(  5, sceModemUnregisterDevice)
END_MODULE
MODULE(modhsyn)
	EXPORT(  4, sceHSyn_Init)
	EXPORT(  5, sceHSyn_ATick)
	EXPORT(  6, sceHSyn_Load)
	EXPORT(  7, sceHSyn_VoiceTrans)
	EXPORT(  8, sceHSyn_SetReservVoice)
	EXPORT(  9, sceHSyn_SetEffectAttr)
	EXPORT( 10, sceHSyn_SetVolume)
	EXPORT( 11, sceHSyn_GetVolume)
	EXPORT( 12, sceHSyn_AllNoteOff)
	EXPORT( 13, sceHSyn_AllSoundOff)
	EXPORT( 14, sceHSyn_ResetAllControler)
	EXPORT( 15, sceHSyn_SetVoiceStatBuffer)
	EXPORT( 16, sceHSyn_SetDebugInfoBuffer)
	EXPORT( 17, sceHSyn_GetChStat)
	EXPORT( 18, sceHSyn_SetOutputMode)
	EXPORT( 19, sceHSyn_SESetMaxVoices)
	EXPORT( 20, sceHSyn_SEAllNoteOff)
	EXPORT( 21, sceHSyn_SEAllSoundOff)
	EXPORT( 22, sceHSyn_SERetrieveVoiceNumberByID)
	EXPORT( 23, sceHSyn_MSGetVoiceStateByID)
	EXPORT( 24, sceHSyn_MSGetVoiceEnvelopeByID)
	EXPORT( 25, sceHSyn_SERetrieveAllSEMsgIDs)
	EXPORT( 26, sceHSyn_GetReservVoice)
	EXPORT( 27, sceHSyn_GetOutputMode)
	EXPORT( 28, sceHSyn_Unload)
END_MODULE
MODULE(modload)
	EXPORT(  4, ReBootStart)
	EXPORT(  5, LoadModuleAddress)
	EXPORT(  6, LoadModule)
	EXPORT(  7, LoadStartModule)
	EXPORT(  8, StartModule)
	EXPORT(  9, LoadModuleBufferAddress)
	EXPORT( 10, LoadModuleBuffer)
	EXPORT( 16, GetModuleIdList)
	EXPORT( 17, ReferModuleStatus)
	EXPORT( 18, GetModuleIdListByName)
	EXPORT( 19, LoadModuleWithOption)
	EXPORT( 20, StopModule)
	EXPORT( 21, UnloadModule)
	EXPORT( 22, SearchModuleByName)
	EXPORT( 23, SearchModuleByAddress)
	EXPORT( 26, SelfStopModule)
	EXPORT( 27, SelfUnloadModule)
	EXPORT( 28, AllocLoadMemory)
	EXPORT( 29, FreeLoadMemory)
	EXPORT( 30, SetModuleFlags)
END_MODULE
MODULE(modmidi)
	EXPORT(  4, sceMidi_Init)
	EXPORT(  5, sceMidi_ATick)
	EXPORT(  6, sceMidi_Load)
	EXPORT(  7, sceMidi_SelectSong)
	EXPORT(  8, sceMidi_SongPlaySwitch)
	EXPORT(  9, sceMidi_SongSetVolume)
	EXPORT( 10, sceMidi_SongVolumeChange)
	EXPORT( 11, sceMidi_SongSetAbsoluteTempo)
	EXPORT( 12, sceMidi_SongSetRelativeTempo)
	EXPORT( 13, sceMidi_SongSetLocation)
	EXPORT( 14, sceMidi_SelectMidi)
	EXPORT( 15, sceMidi_MidiPlaySwitch)
	EXPORT( 16, sceMidi_MidiSetLocation)
	EXPORT( 17, sceMidi_MidiSetVolume)
	EXPORT( 18, sceMidi_MidiVolumeChange)
	EXPORT( 19, sceMidi_MidiSetAbsoluteTempo)
	EXPORT( 20, sceMidi_MidiGetAbsoluteTempo)
	EXPORT( 21, sceMidi_MidiSetRelativeTempo)
	EXPORT( 22, sceMidi_MidiGetRelativeTempo)
	EXPORT( 23, sceMidi_MidiSetUSecTempo)
	EXPORT( 24, sceMidi_MidiGetUSecTempo)
	EXPORT( 25, sceMidi_Unload)
END_MODULE
MODULE(modmono)
	EXPORT(  4, sceMidiMono_Init)
	EXPORT(  5, sceMidiMono_ATick)
	EXPORT(  6, sceMidiMono_SetMono)
END_MODULE
MODULE(modmsin)
	EXPORT(  4, sceMSIn_Init)
	EXPORT(  5, sceMSIn_ATick)
	EXPORT(  6, sceMSIn_Load)
	EXPORT(  7, sceMSIn_PutMsg)
	EXPORT(  8, sceMSIn_PutExcMsg)
	EXPORT(  9, sceMSIn_PutHsMsg)
END_MODULE
MODULE(modsein)
	EXPORT(  4, sceSEIn_Init)
	EXPORT(  5, sceSEIn_ATick)
	EXPORT(  6, sceSEIn_Load)
	EXPORT(  7, sceSEIn_PutMsg)
	EXPORT(  8, sceSEIn_PutSEMsg)
	EXPORT(  9, sceSEIn_MakeNoteOn)
	EXPORT( 10, sceSEIn_MakePitchOn)
	EXPORT( 11, sceSEIn_MakeTimeVolume)
	EXPORT( 12, sceSEIn_MakeTimePanpot)
	EXPORT( 13, sceSEIn_MakeTimePitch)
	EXPORT( 14, sceSEIn_MakePitchLFO)
	EXPORT( 15, sceSEIn_MakeAmpLFO)
	EXPORT( 16, sceSEIn_MakeAllNoteOff)
	EXPORT( 17, sceSEIn_MakeAllNoteOffMask)
	EXPORT( 18, sceSEIn_MakeNoteOnZero)
	EXPORT( 19, sceSEIn_MakePitchOnZero)
END_MODULE
MODULE(modsesq)
	EXPORT(  4, sceSESq_Init)
	EXPORT(  5, sceSESq_ATick)
	EXPORT(  6, sceSESq_Load)
	EXPORT(  7, sceSESq_SelectSeq)
	EXPORT(  8, sceSESq_UnselectSeq)
	EXPORT(  9, sceSESq_SeqPlaySwitch)
	EXPORT( 10, sceSESq_SeqGetStatus)
	EXPORT( 11, sceSESq_SeqIsInPlay)
	EXPORT( 12, sceSESq_SeqIsDataEnd)
	EXPORT( 13, sceSESq_SeqSetSEMsgID)
	EXPORT( 14, sceSESq_SeqTerminateVoice)
END_MODULE
MODULE(modssyn)
	EXPORT(  4, sceSSyn_Init)
	EXPORT(  5, sceSSyn_ATick)
	EXPORT(  6, sceSSyn_Load)
END_MODULE
MODULE(msifrpc)
	EXPORT(  4, sceSifMInitRpc)
	EXPORT( 16, sceSifMTermRpc)
	EXPORT( 17, sceSifMEntryLoop)
END_MODULE
MODULE(netcnf)
	EXPORT(  4, sceNetCnfGetCount)
	EXPORT(  5, sceNetCnfGetList)
	EXPORT(  6, sceNetCnfLoadEntry)
	EXPORT(  7, sceNetCnfAddEntry)
	EXPORT(  8, sceNetCnfDeleteEntry)
	EXPORT(  9, sceNetCnfSetLatestEntry)
	EXPORT( 10, sceNetCnfAllocMem)
	EXPORT( 11, sceNetCnfInitIFC)
	EXPORT( 12, sceNetCnfLoadConf)
	EXPORT( 13, sceNetCnfLoadDial)
	EXPORT( 14, sceNetCnfMergeConf)
	EXPORT( 15, sceNetCnfName2Address)
	EXPORT( 16, sceNetCnfAddress2String)
	EXPORT( 17, sceNetCnfEditEntry)
	EXPORT( 18, sceNetCnfDeleteAll)
	EXPORT( 19, sceNetCnfCheckCapacity)
	EXPORT( 20, sceNetCnfConvA2S)
	EXPORT( 21, sceNetCnfConvS2A)
	EXPORT( 22, sceNetCnfCheckSpecialProvider)
	EXPORT( 23, sceNetCnfSetCallback)
END_MODULE
MODULE(netdev)
	EXPORT(  4, sceInetRegisterNetDevice)
	EXPORT(  5, sceInetUnregisterNetDevice)
	EXPORT(  6, sceInetAllocMem)
	EXPORT(  7, sceInetFreeMem)
	EXPORT(  8, sceInetPktEnQ)
	EXPORT(  9, sceInetPktDeQ)
	EXPORT( 10, sceInetRand)
	EXPORT( 11, sceInetPrintf)
	EXPORT( 12, sceInetAllocPkt)
	EXPORT( 13, sceInetFreePkt)
	EXPORT( 14, sceInetRegisterPPPoE)
	EXPORT( 15, sceInetUnregisterPPPoE)
END_MODULE
MODULE(scrtpad)
	EXPORT(  4, AllocScratchPad)
	EXPORT(  5, FreeScratchPad)
END_MODULE
MODULE(sdhd)
	EXPORT(  4, sceSdHdGetMaxProgramNumber)
	EXPORT(  5, sceSdHdGetMaxSampleSetNumber)
	EXPORT(  6, sceSdHdGetMaxSampleNumber)
	EXPORT(  7, sceSdHdGetMaxVAGInfoNumber)
	EXPORT(  8, sceSdHdGetProgramParamAddr)
	EXPORT(  9, sceSdHdGetProgramParam)
	EXPORT( 10, sceSdHdGetSplitBlockAddr)
	EXPORT( 11, sceSdHdGetSplitBlock)
	EXPORT( 12, sceSdHdGetSampleSetParamAddr)
	EXPORT( 13, sceSdHdGetSampleSetParam)
	EXPORT( 14, sceSdHdGetSampleParamAddr)
	EXPORT( 15, sceSdHdGetSampleParam)
	EXPORT( 16, sceSdHdGetVAGInfoParamAddr)
	EXPORT( 17, sceSdHdGetVAGInfoParam)
	EXPORT( 18, sceSdHdCheckProgramNumber)
	EXPORT( 19, sceSdHdGetSplitBlockCountByNote)
	EXPORT( 20, sceSdHdGetSplitBlockAddrByNote)
	EXPORT( 21, sceSdHdGetSplitBlockByNote)
	EXPORT( 22, sceSdHdGetSampleSetParamCountByNote)
	EXPORT( 23, sceSdHdGetSampleSetParamAddrByNote)
	EXPORT( 24, sceSdHdGetSampleSetParamByNote)
	EXPORT( 25, sceSdHdGetSampleParamCountByNoteVelocity)
	EXPORT( 26, sceSdHdGetSampleParamAddrByNoteVelocity)
	EXPORT( 27, sceSdHdGetSampleParamByNoteVelocity)
	EXPORT( 28, sceSdHdGetVAGInfoParamCountByNoteVelocity)
	EXPORT( 29, sceSdHdGetVAGInfoParamAddrByNoteVelocity)
	EXPORT( 30, sceSdHdGetVAGInfoParamByNoteVelocity)
	EXPORT( 31, sceSdHdGetSampleParamCountByVelocity)
	EXPORT( 32, sceSdHdGetSampleParamAddrByVelocity)
	EXPORT( 33, sceSdHdGetSampleParamByVelocity)
	EXPORT( 34, sceSdHdGetVAGInfoParamCountByVelocity)
	EXPORT( 35, sceSdHdGetVAGInfoParamAddrByVelocity)
	EXPORT( 36, sceSdHdGetVAGInfoParamByVelocity)
	EXPORT( 37, sceSdHdGetVAGInfoParamAddrBySampleNumber)
	EXPORT( 38, sceSdHdGetVAGInfoParamBySampleNumber)
	EXPORT( 39, sceSdHdGetSplitBlockNumberBySplitNumber)
	EXPORT( 40, sceSdHdGetVAGSize)
	EXPORT( 41, sceSdHdGetSplitBlockCount)
	EXPORT( 42, sceSdHdGetMaxSplitBlockCount)
	EXPORT( 43, sceSdHdGetMaxSampleSetParamCount)
	EXPORT( 44, sceSdHdGetMaxSampleParamCount)
	EXPORT( 45, sceSdHdGetMaxVAGInfoParamCount)
	EXPORT( 46, sceSdHdModifyVelocity)
	EXPORT( 47, sceSdHdModifyVelocityLFO)
	EXPORT( 48, sceSdHdGetValidProgramNumberCount)
	EXPORT( 49, sceSdHdGetValidProgramNumber)
	EXPORT( 50, sceSdHdGetSampleNumberBySampleIndex)
END_MODULE
MODULE(sdrdrv)
	EXPORT(  4, sceSdrChangeThreadPriority)
	EXPORT(  5, sceSdrSetUserCommandFunction)
END_MODULE
MODULE(sdsq)
	EXPORT(  4, sceSdSqGetMaxMidiNumber)
	EXPORT(  5, sceSdSqGetMaxSongNumber)
	EXPORT(  6, sceSdSqInitMidiData)
	EXPORT(  7, sceSdSqReadMidiData)
	EXPORT(  8, sceSdSqInitSongData)
	EXPORT(  9, sceSdSqReadSongData)
	EXPORT( 10, sceSdSqGetMaxCompTableIndex)
	EXPORT( 11, sceSdSqGetCompTableOffset)
	EXPORT( 12, sceSdSqGetCompTableDataByIndex)
	EXPORT( 13, sceSdSqGetNoteOnEventByPolyKeyPress)
	EXPORT( 14, sceSdSqCopyMidiData)
	EXPORT( 15, sceSdSqCopySongData)
END_MODULE
MODULE(sifcmd)
	EXPORT(  4, sceSifInitCmd)
	EXPORT(  5, sceSifExitCmd)
	EXPORT(  6, sceSifGetSreg)
	EXPORT(  7, sceSifSetSreg)
	EXPORT(  8, sceSifSetCmdBuffer)
	EXPORT( 10, sceSifAddCmdHandler)
	EXPORT( 11, sceSifRemoveCmdHandler)
	EXPORT( 12, sceSifSendCmd)
	EXPORT( 13, isceSifSendCmd)
	EXPORT( 14, sceSifInitRpc)
	EXPORT( 15, sceSifBindRpc)
	EXPORT( 16, sceSifCallRpc)
	EXPORT( 17, sceSifRegisterRpc)
	EXPORT( 18, sceSifCheckStatRpc)
	EXPORT( 19, sceSifSetRpcQueue)
	EXPORT( 20, sceSifGetNextRequest)
	EXPORT( 21, sceSifExecRequest)
	EXPORT( 22, sceSifRpcLoop)
	EXPORT( 23, sceSifGetOtherData)
	EXPORT( 24, sceSifRemoveRpc)
	EXPORT( 25, sceSifRemoveRpcQueue)
	EXPORT( 28, sceSifSendCmdIntr)
	EXPORT( 29, isceSifSendCmdIntr)
END_MODULE
MODULE(sifman)
	EXPORT(  5, sceSifInit)
	EXPORT(  6, sceSifSetDChain)
	EXPORT(  7, sceSifSetDma)
	EXPORT(  8, sceSifDmaStat)
	EXPORT( 29, sceSifCheckInit)
	EXPORT( 32, sceSifSetDmaIntr)
END_MODULE
MODULE(spucodec)
	EXPORT(  4, sceSpuCodecEncode)
END_MODULE
MODULE(stdio)
	EXPORT(  4, printf)
	EXPORT(  5, getchar)
	EXPORT(  6, putchar)
	EXPORT(  7, puts)
	EXPORT(  8, gets)
	EXPORT(  9, fdprintf)
	EXPORT( 10, fdgetc)
	EXPORT( 11, fdputc)
	EXPORT( 12, fdputs)
	EXPORT( 13, fdgets)
	EXPORT( 14, vfdprintf)
END_MODULE
MODULE(sysclib)
	EXPORT(  4, setjmp)
	EXPORT(  5, longjmp)
	EXPORT(  6, toupper)
	EXPORT(  7, tolower)
	EXPORT(  8, look_ctype_table)
	EXPORT(  9, get_ctype_table)
	EXPORT( 10, memchr)
	EXPORT( 11, memcmp)
	EXPORT( 12, memcpy)
	EXPORT( 13, memmove)
	EXPORT( 14, memset)
	EXPORT( 15, bcmp)
	EXPORT( 16, bcopy)
	EXPORT( 17, bzero)
	EXPORT( 18, prnt)
	EXPORT( 19, sprintf)
	EXPORT( 20, strcat)
	EXPORT( 21, strchr)
	EXPORT( 22, strcmp)
	EXPORT( 23, strcpy)
	EXPORT( 24, strcspn)
	EXPORT( 25, index)
	EXPORT( 26, rindex)
	EXPORT( 27, strlen)
	EXPORT( 28, strncat)
	EXPORT( 29, strncmp)
	EXPORT( 30, strncpy)
	EXPORT( 31, strpbrk)
	EXPORT( 32, strrchr)
	EXPORT( 33, strspn)
	EXPORT( 34, strstr)
	EXPORT( 35, strtok)
	EXPORT( 36, strtol)
	EXPORT( 37, atob)
	EXPORT( 38, strtoul)
	EXPORT( 40, wmemcopy)
	EXPORT( 41, wmemset)
	EXPORT( 42, vsprintf)
	EXPORT( 43, strtok_r)
END_MODULE
MODULE(sysmem)
	EXPORT(  4, AllocSysMemory)
	EXPORT(  5, FreeSysMemory)
	EXPORT(  6, QueryMemSize)
	EXPORT(  7, QueryMaxFreeMemSize)
	EXPORT(  8, QueryTotalFreeMemSize)
	EXPORT(  9, QueryBlockTopAddress)
	EXPORT( 10, QueryBlockSize)
	EXPORT( 14, Kprintf)
END_MODULE
MODULE(thbase)
	EXPORT(  4, CreateThread)
	EXPORT(  5, DeleteThread)
	EXPORT(  6, StartThread)
	EXPORT(  7, StartThreadArgs)
	EXPORT(  8, ExitThread)
	EXPORT(  9, ExitDeleteThread)
	EXPORT( 10, TerminateThread)
	EXPORT( 11, iTerminateThread)
	EXPORT( 12, DisableDispatchThread)
	EXPORT( 13, EnableDispatchThread)
	EXPORT( 14, ChangeThreadPriority)
	EXPORT( 15, iChangeThreadPriority)
	EXPORT( 16, RotateThreadReadyQueue)
	EXPORT( 17, iRotateThreadReadyQueue)
	EXPORT( 18, ReleaseWaitThread)
	EXPORT( 19, iReleaseWaitThread)
	EXPORT( 20, GetThreadId)
	EXPORT( 21, CheckThreadStack)
	EXPORT( 22, ReferThreadStatus)
	EXPORT( 23, iReferThreadStatus)
	EXPORT( 24, SleepThread)
	EXPORT( 25, WakeupThread)
	EXPORT( 26, iWakeupThread)
	EXPORT( 27, CancelWakeupThread)
	EXPORT( 28, iCancelWakeupThread)
	EXPORT( 29, SuspendThread)
	EXPORT( 30, iSuspendThread)
	EXPORT( 31, ResumeThread)
	EXPORT( 32, iResumeThread)
	EXPORT( 33, DelayThread)
	EXPORT( 34, GetSystemTime)
	EXPORT( 35, SetAlarm)
	EXPORT( 36, iSetAlarm)
	EXPORT( 37, CancelAlarm)
	EXPORT( 38, iCancelAlarm)
	EXPORT( 39, USec2SysClock)
	EXPORT( 40, SysClock2USec)
	EXPORT( 41, GetSystemStatusFlag)
	EXPORT( 42, GetThreadCurrentPriority)
	EXPORT( 43, GetSystemTimeLow)
	EXPORT( 44, ReferSystemStatus)
	EXPORT( 45, ReferThreadRunStatus)
	EXPORT( 46, GetThreadStackFreeSize)
	EXPORT( 47, GetThreadmanIdList)
END_MODULE
MODULE(thevent)
	EXPORT(  4, CreateEventFlag)
	EXPORT(  5, DeleteEventFlag)
	EXPORT(  6, SetEventFlag)
	EXPORT(  7, iSetEventFlag)
	EXPORT(  8, ClearEventFlag)
	EXPORT(  9, iClearEventFlag)
	EXPORT( 10, WaitEventFlag)
	EXPORT( 11, PollEventFlag)
	EXPORT( 13, ReferEventFlagStatus)
	EXPORT( 14, iReferEventFlagStatus)
END_MODULE
MODULE(thfpool)
	EXPORT(  4, CreateFpl)
	EXPORT(  5, DeleteFpl)
	EXPORT(  6, AllocateFpl)
	EXPORT(  7, pAllocateFpl)
	EXPORT(  8, ipAllocateFpl)
	EXPORT(  9, FreeFpl)
	EXPORT( 11, ReferFplStatus)
	EXPORT( 12, iReferFplStatus)
END_MODULE
MODULE(thmsgbx)
	EXPORT(  4, CreateMbx)
	EXPORT(  5, DeleteMbx)
	EXPORT(  6, SendMbx)
	EXPORT(  7, iSendMbx)
	EXPORT(  8, ReceiveMbx)
	EXPORT(  9, PollMbx)
	EXPORT( 11, ReferMbxStatus)
	EXPORT( 12, iReferMbxStatus)
END_MODULE
MODULE(thsemap)
	EXPORT(  4, CreateSema)
	EXPORT(  5, DeleteSema)
	EXPORT(  6, SignalSema)
	EXPORT(  7, iSignalSema)
	EXPORT(  8, WaitSema)
	EXPORT(  9, PollSema)
	EXPORT( 11, ReferSemaStatus)
	EXPORT( 12, iReferSemaStatus)
END_MODULE
MODULE(thvpool)
	EXPORT(  4, CreateVpl)
	EXPORT(  5, DeleteVpl)
	EXPORT(  6, AllocateVpl)
	EXPORT(  7, pAllocateVpl)
	EXPORT(  8, ipAllocateVpl)
	EXPORT(  9, FreeVpl)
	EXPORT( 11, ReferVplStatus)
	EXPORT( 12, iReferVplStatus)
END_MODULE
MODULE(timrman)
	EXPORT(  4, AllocHardTimer)
	EXPORT(  5, ReferHardTimer)
	EXPORT(  6, FreeHardTimer)
	EXPORT(  7, SetTimerMode)
	EXPORT(  8, GetTimerStatus)
	EXPORT(  9, SetTimerCounter)
	EXPORT( 10, GetTimerCounter)
	EXPORT( 11, SetTimerCompare)
	EXPORT( 12, GetTimerCompare)
	EXPORT( 16, GetHardTimerIntrCode)
	EXPORT( 20, SetTimerHandler)
	EXPORT( 21, SetOverflowHandler)
	EXPORT( 22, SetupHardTimer)
	EXPORT( 23, StartHardTimer)
	EXPORT( 24, StopHardTimer)
END_MODULE
MODULE(usbd)
	EXPORT(  4, sceUsbdRegisterLdd)
	EXPORT(  5, sceUsbdUnregisterLdd)
	EXPORT(  6, sceUsbdScanStaticDescriptor)
	EXPORT(  7, sceUsbdSetPrivateData)
	EXPORT(  8, sceUsbdGetPrivateData)
	EXPORT(  9, sceUsbdOpenPipe)
	EXPORT( 10, sceUsbdClosePipe)
	EXPORT( 11, sceUsbdTransferPipe)
	EXPORT( 12, sceUsbdOpenPipeAligned)
	EXPORT( 13, sceUsbdGetDeviceLocation)
	EXPORT( 16, sceUsbdChangeThreadPriority)
	EXPORT( 17, sceUsbdGetReportDescriptor)
	EXPORT( 18, sceUsbdMultiIsochronousTransfer)
END_MODULE
MODULE(usbmload)
	EXPORT(  4, sceUsbmlDisable)
	EXPORT(  5, sceUsbmlEnable)
	EXPORT(  6, sceUsbmlActivateCategory)
	EXPORT(  7, sceUsbmlInactivateCategory)
	EXPORT(  8, sceUsbmlRegisterLoadFunc)
	EXPORT(  9, sceUsbmlUnregisterLoadFunc)
	EXPORT( 10, sceUsbmlLoadConffile)
	EXPORT( 11, sceUsbmlRegisterDevice)
	EXPORT( 12, sceUsbmlChangeThreadPriority)
END_MODULE
MODULE(vblank)
	EXPORT(  4, WaitVblankStart)
	EXPORT(  5, WaitVblankEnd)
	EXPORT(  6, WaitVblank)
	EXPORT(  7, WaitNonVblank)
	EXPORT(  8, RegisterVblankHandler)
	EXPORT(  9, ReleaseVblankHandler)
END_MODULE

// undocumented functions from old list
#if 0
MODULE(sysmem)
	EXPORT(  3, return_addr_of_memsize)
    EXPORT( 15, set_Kprintf)
END_MODULE
MODULE(loadcore)
	EXPORT(  3, return_LibraryEntryTable)
	EXPORT(  8, findFixImports)
	EXPORT(  9, restoreImports)
    EXPORT( 14, setFlag)
	EXPORT( 15, resetFlag)
    EXPORT( 16, linkModule)
	EXPORT( 17, unlinkModule)
	EXPORT( 20, registerFunc)
	EXPORT( 22, read_header)
	EXPORT( 23, load_module)
	EXPORT( 24, findImageInfo)
END_MODULE
MODULE(excepman)
    EXPORT(  3, getcommon)
END_MODULE
MODULE(intrman)
	EXPORT( 10, syscall04)
	EXPORT( 11, syscall08)
	EXPORT( 12, resetICTRL)
	EXPORT( 13, setICTRL)
	EXPORT( 14, syscall0C)
	EXPORT( 19, CpuSuspendIntr)
	EXPORT( 20, CpuResumeIntr)
	EXPORT( 21, syscall10)
	EXPORT( 22, syscall14)
	EXPORT( 28, set_h1)
	EXPORT( 29, reset_h1)
	EXPORT( 30, set_h2)
	EXPORT( 31, reset_h2)
END_MODULE
MODULE(ssbusc)
	EXPORT(  4, setTable1)
	EXPORT(  5, getTable1)
	EXPORT(  6, setTable2)
	EXPORT(  7, getTable2)
	EXPORT(  8, setCOM_DELAY_1st)
	EXPORT(  9, getCOM_DELAY_1st)
	EXPORT( 10, setCOM_DELAY_2nd)
	EXPORT( 11, getCOM_DELAY_2nd)
	EXPORT( 12, setCOM_DELAY_3rd)
	EXPORT( 13, getCOM_DELAY_3rd)
	EXPORT( 14, setCOM_DELAY_4th)
	EXPORT( 15, getCOM_DELAY_4th)
	EXPORT( 16, setCOM_DELAY)
	EXPORT( 16, getCOM_DELAY)
END_MODULE
MODULE(dmacman)
	EXPORT(  4, SetD_MADR)
	EXPORT(  5, GetD_MADR)
	EXPORT(  6, SetD_BCR)
	EXPORT(  7, GetD_BCR)
	EXPORT(  8, SetD_CHCR)
	EXPORT(  9, GetD_CHCR)
	EXPORT( 10, SetD_TADR)
	EXPORT( 11, GetD_TADR)
	EXPORT( 12, Set_4_9_A)
	EXPORT( 13, Get_4_9_A)
	EXPORT( 14, SetDPCR)
	EXPORT( 15, GetDPCR)
	EXPORT( 16, SetDPCR2)
	EXPORT( 17, GetDPCR2)
	EXPORT( 18, SetDPCR3)
	EXPORT( 19, GetDPCR3)
	EXPORT( 20, SetDICR)
	EXPORT( 21, GetDICR)
	EXPORT( 22, SetDICR2)
	EXPORT( 23, GetDICR2)
	EXPORT( 24, SetBF80157C)
	EXPORT( 25, GetBF80157C)
	EXPORT( 26, SetBF801578)
	EXPORT( 27, GetBF801578)
	EXPORT( 28, SetDMA)
	EXPORT( 29, SetDMA_chainedSPU_SIF0)
	EXPORT( 30, SetDMA_SIF0)
	EXPORT( 31, SetDMA_SIF1)
	EXPORT( 32, StartTransfer)
	EXPORT( 33, SetVal)
	EXPORT( 34, EnableDMAch)
	EXPORT( 35, DisableDMAch)
END_MODULE
MODULE(timrman)
	EXPORT( 13, SetHoldMode)
	EXPORT( 14, GetHoldMode)
	EXPORT( 15, GetHoldReg)
END_MODULE
MODULE(sifman)
	EXPORT(  4, sceSif2Init)
	EXPORT(  9, sceSifSend)
	EXPORT( 10, sceSifSendSync)
	EXPORT( 11, sceSifIsSending)
	EXPORT( 12, sceSifSetSIF0DMA)
	EXPORT( 13, sceSifSendSync0)
	EXPORT( 14, sceSifIsSending0)
	EXPORT( 15, sceSifSetSIF1DMA)
	EXPORT( 16, sceSifSendSync1)
	EXPORT( 17, sceSifIsSending1)
	EXPORT( 18, sceSifSetSIF2DMA)
	EXPORT( 19, sceSifSendSync2)
	EXPORT( 20, sceSifIsSending2)
	EXPORT( 21, getEEIOPflags)
	EXPORT( 22, setEEIOPflags)
	EXPORT( 23, getIOPEEflags)
	EXPORT( 24, setIOPEEflags)
	EXPORT( 25, getEErcvaddr)
	EXPORT( 26, getIOPrcvaddr)
	EXPORT( 27, setIOPrcvaddr)
    EXPORT( 30, setSif0CB)
	EXPORT( 31, resetSif0CB)
END_MODULE
MODULE(sifcmd)
    EXPORT(  9, sceSifSetSysCmdBuffer)
	EXPORT( 26, setSif1CB)
	EXPORT( 27, resetSif1CB)
END_MODULE
MODULE(cdvdman)
	EXPORT( 20, sceDvdRead)
	EXPORT( 23, sceCdWriteILinkID)
    EXPORT( 25, sceCdWriteRTC)
	EXPORT( 26, sceCdReadNVM)
	EXPORT( 27, sceCdWriteNVM)
	EXPORT( 30, setHDmode)
	EXPORT( 31, sceCdOpenConfig)
	EXPORT( 32, sceCdCloseConfig)
	EXPORT( 33, sceCdReadConfig)
	EXPORT( 34, sceCdWriteConfig)
	EXPORT( 35, sceCdReadKey)
	EXPORT( 36. sceCdDecSet)
	EXPORT( 41, sceCdReadConsoleID)
	EXPORT( 42, sceCdWriteConsoleID)
	EXPORT( 43, sceCdGetMecaconVersion)
	EXPORT( 52, sceCdForbidDVDP)
	EXPORT( 53, sceCdReadSubQ)
	EXPORT( 55, AutoAdjustCtrl)
END_MODULE
MODULE(sio2man)
	EXPORT(  4, set8268_ctrl)
	EXPORT(  5, get8268_ctrl)
	EXPORT(  6, get826C_recv1)
	EXPORT(  7, call7_send1)
	EXPORT(  8, call8_send1)
	EXPORT(  9, call9_send2)
	EXPORT( 10, call10_send2)
	EXPORT( 11, get8270_recv2)
	EXPORT( 12, call12_set_params)
	EXPORT( 13, call13_get_params)
	EXPORT( 14, get8274_recv3)
	EXPORT( 15, set8278)
	EXPORT( 16, get8278)
	EXPORT( 17, set827C)
	EXPORT( 18, get827C)
	EXPORT( 19, set8260_datain)
	EXPORT( 20, get8264_dataout)
	EXPORT( 21, set8280_intr)
	EXPORT( 22, get8280_intr)
	EXPORT( 23, signalExchange1)
	EXPORT( 24, signalExchange2)
	EXPORT( 25, packetExchange)
END_MODULE
#endif

#undef MODULE
#undef END_MODULE
#undef EXPORT

		switch (index)
		{
			case 0:
				return "start";
			case 2:
				return "shutdown";
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

	void irxImportLog(const std::string& libname, u16 index, const char* funcname) { }
	void irxImportLog_rec(u32 import_table, u16 index, const char* funcname) { }

	int irxImportExec(u32 import_table, u16 index)
	{
		if (!import_table)
			return 0;

		std::string libname = iopMemReadString(import_table + 12, 8);
		const char* funcname = irxImportFuncname(libname, index);
		irxHLE hle = irxImportHLE(libname, index);
		irxDEBUG debug = irxImportDebug(libname, index);

		if (debug)
			debug();

		if (hle)
			return hle();
		return 0;
	}

} // end namespace R3000A
