/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

#include <retro_atomic.h>
#include <algorithm>
#include "common/Console.h"
#include "common/StringUtil.h"

#include <file/file_path.h>
#include <streams/file_stream.h>

#include "HddCreate.h"

#define HDD_CREATE_BUFF_SIZE (4 * 1024)

void HddCreate::Start()
{
	Init();
	WriteImage(filePath, neededSize, 1024);
	Cleanup();
}

void HddCreate::WriteImage(std::string hddPath, u64 fileBytes, u64 zeroSizeBytes)
{
	u8 buff[HDD_CREATE_BUFF_SIZE] = {0}; // 4kb.
	RFILE* newImage;
	bool sparseSupported;
	s32 reqMiB;
	s32 zeroMiB;
	s32 iMiB;

	if (path_is_valid(hddPath.c_str()))
	{
		retro_atomic_store_release_int(&errored, 1);
		SetError();
		return;
	}

	newImage = filestream_open(hddPath.c_str(),
			RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!newImage)
	{
		retro_atomic_store_release_int(&errored, 1);
		SetError();
		return;
	}

	// Set filesize.
	if (filestream_truncate(newImage, (int64_t)fileBytes) != 0 ||
		filestream_seek(newImage, 0, RETRO_VFS_SEEK_POSITION_START) < 0)
	{
		Console.Error("DEV9: HddCreate: Failed to set size");
		filestream_close(newImage);
		filestream_delete(hddPath.c_str());
		retro_atomic_store_release_int(&errored, 1);
		SetError();
		return;
	}

	/* Whether the file can actually be sparse is answered by trying:
	 * punching the whole range deallocates whatever the truncate
	 * allocated (nothing, on filesystems where extension is already a
	 * hole) and on Windows arms the sparse attribute as a side effect
	 * of the punch, which is why no explicit FSCTL_SET_SPARSE step
	 * exists here. -1 means the backend or filesystem cannot punch,
	 * and the image is written out in full below. */
	sparseSupported = (filestream_punch_hole(newImage, 0, (int64_t)fileBytes) == 0);

	lastUpdate = std::chrono::steady_clock::now();

	// Round up.
	reqMiB = (s32)((fileBytes + ((1024 * 1024) - 1)) / (1024 * 1024));
	zeroMiB = (s32)((zeroSizeBytes + ((1024 * 1024) - 1)) / (1024 * 1024));

	iMiB = 0;
	if (sparseSupported)
		iMiB = reqMiB - zeroMiB;

	for (; iMiB < reqMiB; iMiB++)
	{
		// Round down.
		const s32 req4Kib = std::min<s32>(1024, (s32)((fileBytes / 1024) - (u64)iMiB * 1024)) / 4;
		s32 i4kb;

		for (i4kb = 0; i4kb < req4Kib; i4kb++)
		{
			if (filestream_write(newImage, buff, HDD_CREATE_BUFF_SIZE) != HDD_CREATE_BUFF_SIZE)
			{
				filestream_flush(newImage);
				// Set filesize to zero to avoid potential freeze on close.
				filestream_truncate(newImage, 0);
				filestream_close(newImage);
				filestream_delete(hddPath.c_str());
				retro_atomic_store_release_int(&errored, 1);
				SetError();
				return;
			}
		}

		if (req4Kib != 256)
		{
			const s32 remainingBytes = (s32)(fileBytes - (((u64)iMiB) * (1024 * 1024) + (u64)req4Kib * 4096));
			if (filestream_write(newImage, buff, remainingBytes) != remainingBytes)
			{
				filestream_flush(newImage);
				// Set filesize to zero to avoid potential freeze on close.
				filestream_truncate(newImage, 0);
				filestream_close(newImage);
				filestream_delete(hddPath.c_str());
				retro_atomic_store_release_int(&errored, 1);
				SetError();
				return;
			}
		}

		{
			const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count() >= 100 || (iMiB + 1) == reqMiB)
			{
				lastUpdate = now;
				SetFileProgress((u64)filestream_tell(newImage));
			}
		}
		if (retro_atomic_load_acquire_int(&canceled))
		{
			filestream_flush(newImage);
			// Set filesize to zero to avoid potential freeze on close.
			filestream_truncate(newImage, 0);
			filestream_close(newImage);
			filestream_delete(hddPath.c_str());
			retro_atomic_store_release_int(&errored, 1);
			SetError();
			return;
		}
	}

	if (filestream_flush(newImage) != 0)
	{
		Console.Error("DEV9: HddCreate: Failed to flush");
		filestream_truncate(newImage, 0);
		filestream_close(newImage);
		filestream_delete(hddPath.c_str());
		retro_atomic_store_release_int(&errored, 1);
		SetError();
		return;
	}
	filestream_close(newImage);
}

void HddCreate::SetFileProgress(u64 currentSize)
{
	Console.WriteLn(StringUtil::StdStringFromFormat("%llu / %llu Bytes",
		(unsigned long long)currentSize, (unsigned long long)neededSize).c_str());
}

void HddCreate::SetError()
{
	Console.WriteLn("Failed to create HDD file");
}

void HddCreate::SetCanceled()
{
	retro_atomic_store_release_int(&canceled, 1);
}
