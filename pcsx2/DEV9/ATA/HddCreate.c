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

#include <string.h>

#include <libretro.h>
#include <retro_miscellaneous.h>
#include <features/features_cpu.h>
#include <file/file_path.h>
#include <streams/file_stream.h>

#include "HddCreate.h"
#include "DEV9/ATA/ATA.h" /* pcsx2_log */

#define HDD_CREATE_BUFF_SIZE (4 * 1024)
#define HDD_CREATE_ZERO_BYTES 1024

/* One write buffer's worth of zeroes. Static rather than a local so
 * the frame stays inside the 4 KiB stack budget. */
static const uint8_t hdd_create_zero_buff[HDD_CREATE_BUFF_SIZE];

static void hdd_create_fail(RFILE* newImage, const char* path)
{
	/* Set filesize to zero to avoid potential freeze on close. */
	filestream_flush(newImage);
	filestream_truncate(newImage, 0);
	filestream_close(newImage);
	filestream_delete(path);
	pcsx2_log(RETRO_LOG_INFO, "Failed to create HDD file\n");
}

int hdd_create(const char* path, uint64_t size_bytes)
{
	RFILE* newImage;
	retro_time_t lastUpdate;
	bool sparseSupported;
	int32_t reqMiB;
	int32_t zeroMiB;
	int32_t iMiB;

	if (path_is_valid(path))
	{
		pcsx2_log(RETRO_LOG_INFO, "Failed to create HDD file\n");
		return -1;
	}

	newImage = filestream_open(path,
			RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!newImage)
	{
		pcsx2_log(RETRO_LOG_INFO, "Failed to create HDD file\n");
		return -1;
	}

	/* Set filesize. */
	if (filestream_truncate(newImage, (int64_t)size_bytes) != 0 ||
		filestream_seek(newImage, 0, RETRO_VFS_SEEK_POSITION_START) < 0)
	{
		pcsx2_log(RETRO_LOG_ERROR, "DEV9: HddCreate: Failed to set size\n");
		hdd_create_fail(newImage, path);
		return -1;
	}

	/* Whether the file can actually be sparse is answered by trying:
	 * punching the whole range deallocates whatever the truncate
	 * allocated (nothing, on filesystems where extension is already a
	 * hole) and on Windows arms the sparse attribute as a side effect
	 * of the punch, which is why no explicit FSCTL_SET_SPARSE step
	 * exists here. -1 means the backend or filesystem cannot punch,
	 * and the image is written out in full below. */
	sparseSupported = (filestream_punch_hole(newImage, 0, (int64_t)size_bytes) == 0);

	lastUpdate = cpu_features_get_time_usec();

	/* Round up. */
	reqMiB = (int32_t)((size_bytes + ((1024 * 1024) - 1)) / (1024 * 1024));
	zeroMiB = (int32_t)((HDD_CREATE_ZERO_BYTES + ((1024 * 1024) - 1)) / (1024 * 1024));

	iMiB = 0;
	if (sparseSupported)
		iMiB = reqMiB - zeroMiB;

	for (; iMiB < reqMiB; iMiB++)
	{
		/* Round down. */
		const int32_t req4Kib = MIN(1024, (int32_t)((size_bytes / 1024) - (uint64_t)iMiB * 1024)) / 4;
		int32_t i4kb;

		for (i4kb = 0; i4kb < req4Kib; i4kb++)
		{
			if (filestream_write(newImage, hdd_create_zero_buff, HDD_CREATE_BUFF_SIZE) != HDD_CREATE_BUFF_SIZE)
			{
				hdd_create_fail(newImage, path);
				return -1;
			}
		}

		if (req4Kib != 256)
		{
			const int32_t remainingBytes = (int32_t)(size_bytes - (((uint64_t)iMiB) * (1024 * 1024) + (uint64_t)req4Kib * 4096));
			if (filestream_write(newImage, hdd_create_zero_buff, remainingBytes) != remainingBytes)
			{
				hdd_create_fail(newImage, path);
				return -1;
			}
		}

		{
			const retro_time_t now = cpu_features_get_time_usec();
			if ((now - lastUpdate) / 1000 >= 100 || (iMiB + 1) == reqMiB)
			{
				lastUpdate = now;
				pcsx2_log(RETRO_LOG_INFO, "%llu / %llu Bytes\n",
						(unsigned long long)filestream_tell(newImage),
						(unsigned long long)size_bytes);
			}
		}
	}

	if (filestream_flush(newImage) != 0)
	{
		pcsx2_log(RETRO_LOG_ERROR, "DEV9: HddCreate: Failed to flush\n");
		hdd_create_fail(newImage, path);
		return -1;
	}
	filestream_close(newImage);
	return 0;
}
