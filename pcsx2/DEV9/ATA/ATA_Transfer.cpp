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
#include "../../../common/Threading.h"

#include "ATA.h"
#include "DEV9/DEV9.h"

#include <streams/file_stream.h>

void ATA::IO_Thread()
{
	Threading::ScopedLock ioWaitHandle(ioMutex);
	ioThreadIdle_bool = false;
	ioWaitHandle.Unlock();

	while (true)
	{
		ioWaitHandle.Lock();
		ioThreadIdle_bool = true;
		ioThreadIdle_cv.Broadcast();

		while (!(ioRead | ioWrite))
			ioReady.Wait(ioMutex);
		ioThreadIdle_bool = false;

		int ioType = -1;
		if (ioRead)
			ioType = 0;
		else if (ioWrite)
			ioType = 1;

		ioWaitHandle.Unlock();

		//Read or Write
		if (ioType == 0)
			IO_Read();
		else if (ioType == 1)
		{
			if (!IO_Write())
			{
				if (retro_atomic_load_acquire_int(&ioClose))
				{
					retro_atomic_store_release_int(&ioClose, 0);
					ioWaitHandle.Lock();
					ioThreadIdle_bool = true;
					ioWaitHandle.Unlock();
					return;
				}
			}
		}
	}
}

void ATA::IO_Read()
{
	const s64 lba = HDD_GetLBA();
	u64 pos;

	if (lba == -1)
	{
		Console.Error("DEV9: ATA: Invalid LBA");
		abort();
	}

	pos = (u64)lba * 512;
	if (filestream_seek(hddImage, (int64_t)pos, RETRO_VFS_SEEK_POSITION_START) < 0 ||
		filestream_read(hddImage, readBuffer, (int64_t)nsector * 512) != (int64_t)nsector * 512)
	{
		Console.Error("DEV9: ATA: File read error");
		abort();
	}
	{
		Threading::ScopedLock ioSignallock(ioMutex);
		ioRead = false;
	}
}

bool ATA::IO_Write()
{
	WriteQueueEntry entry;
	u64 imagePos;

	if (!writeQueue.Dequeue(&entry))
	{
		Threading::ScopedLock ioSignallock(ioMutex);
		ioWrite = false;
		return false;
	}

	imagePos = entry.sector * 512;
	if (filestream_seek(hddImage, (int64_t)imagePos, RETRO_VFS_SEEK_POSITION_START) < 0)
	{
		Console.Error("DEV9: ATA: File seek error");
		abort();
	}
	if (hddSparse)
	{
		u32 written = 0;
		while (written != entry.length)
		{
			u32 writeSize;
			bool sparseWrite;

			IO_SparseCacheUpdateLocation(imagePos + written);
			// Align to sparse block size.
			writeSize = (u32)(hddSparseBlockSize - ((imagePos + written) % hddSparseBlockSize));
			// Limit to size of write.
			writeSize = std::min(writeSize, entry.length - written);

			sparseWrite = IsAllZero(&entry.data[written], writeSize);

			if (sparseWrite)
			{
				if (!IO_SparseZero(imagePos + written, writeSize))
				{
					Console.Error("DEV9: ATA: File sparse write error");

					hddSparse = false;
					hddSparseBlock = nullptr;
					hddSparseBlockValid = false;

					// Fallthough into other if statment.
					sparseWrite = false;
				}
			}

			// Also handles sparse write failures.
			if (!sparseWrite)
			{
				// Update cache.
				if (hddSparseBlockValid)
					memcpy(&hddSparseBlock[(imagePos + written) - HddSparseStart], &entry.data[written], writeSize);

				if (filestream_write(hddImage, &entry.data[written], (int64_t)writeSize) != (int64_t)writeSize ||
					filestream_flush(hddImage) != 0)
				{
					Console.Error("DEV9: ATA: File write error");
					abort();
				}
			}
			written += writeSize;
		}
	}
	else
	{
		if (filestream_write(hddImage, entry.data, (int64_t)entry.length) != (int64_t)entry.length ||
			filestream_flush(hddImage) != 0)
		{
			Console.Error("DEV9: ATA: File write error");
			abort();
		}
	}
	delete[] entry.data;
	return true;
}

void ATA::IO_SparseCacheLoad()
{
	// Reads are bounds checked, but for the sectors read only.
	// Need to bounds check for sparse block, to handle an edge case of a user providing a file with a size that dosn't align with the sparse block size.
	// Normally that won't happen as we generate files of exact Gib size.
	u64 readSize = hddSparseBlockSize;
	const u64 posEnd = HddSparseStart + hddSparseBlockSize;
	s64 orgPos;

	if (posEnd > hddImageSize)
	{
		readSize = hddSparseBlockSize - (posEnd - hddImageSize);
		// Zero cache for data beyond end of file.
		memset(&hddSparseBlock[readSize], 0, hddSparseBlockSize - readSize);
	}

	// Store file pointer.
	orgPos = filestream_tell(hddImage);

	/* The native-handle code asked the OS whether this block was still
	 * a hole (FSCTL_QUERY_ALLOCATED_RANGES, SEEK_HOLE) so it could skip
	 * the read. The VFS has no such query, so the block is always read;
	 * a hole reads back as zeroes straight out of the page cache, so
	 * this costs a copy rather than I/O and the cache contents come out
	 * identical. The flush keeps the read coherent with our own
	 * buffered writes on whatever backend the VFS resolved to. */
	filestream_flush(hddImage);

	// Load into cache.
	if (orgPos < 0 ||
		filestream_seek(hddImage, (int64_t)HddSparseStart, RETRO_VFS_SEEK_POSITION_START) < 0 ||
		filestream_read(hddImage, hddSparseBlock.get(), (int64_t)readSize) != (int64_t)readSize ||
		filestream_seek(hddImage, orgPos, RETRO_VFS_SEEK_POSITION_START) < 0) // Restore file pointer.
	{
		Console.Error("DEV9: ATA: File read error");
		abort();
	}

	hddSparseBlockValid = true;
}

void ATA::IO_SparseCacheUpdateLocation(u64 byteOffset)
{
	const u64 currentBlockStart = (byteOffset / hddSparseBlockSize) * hddSparseBlockSize;
	if (currentBlockStart != HddSparseStart)
	{
		HddSparseStart = currentBlockStart;
		hddSparseBlockValid = false;
		// Only update cache when we perform a sparse write.
	}
}

// Also sets hddImage write ptr.
bool ATA::IO_SparseZero(u64 byteOffset, u64 byteSize)
{
	if (hddSparseBlockValid == false)
		IO_SparseCacheLoad();

	//Write to cache
	memset(&hddSparseBlock[byteOffset - HddSparseStart], 0, byteSize);

	//Is block non-zero?
	if (!IsAllZero(hddSparseBlock.get(), hddSparseBlockSize))
	{
		//No, do normal write
		if (filestream_write(hddImage, &hddSparseBlock[byteOffset - HddSparseStart], (int64_t)byteSize) != (int64_t)byteSize ||
			filestream_flush(hddImage) != 0)
		{
			Console.Error("DEV9: ATA: File write error");
			abort();
		}
		return true;
	}

	/* Yes, try sparse write. filestream_punch_hole carries the whole
	 * platform matrix the native-handle code open-coded here --
	 * FSCTL_SET_ZERO_DATA (arming FSCTL_SET_SPARSE itself first),
	 * fallocate(FALLOC_FL_PUNCH_HOLE), fcntl(F_PUNCHHOLE) -- and
	 * returns -1 where the backend or filesystem cannot punch at all,
	 * upon which the caller falls back to plain writes for good. */
	if (filestream_punch_hole(hddImage, (int64_t)HddSparseStart, (int64_t)hddSparseBlockSize) != 0)
		return false;

	if (filestream_seek(hddImage, (int64_t)(byteOffset + byteSize), RETRO_VFS_SEEK_POSITION_START) < 0)
	{
		Console.Error("DEV9: ATA: File seek error");
		abort();
	}
	return true;
}

bool ATA::IsAllZero(const void* data, size_t len)
{
	intmax_t* pbi = (intmax_t*)data;
	intmax_t* pbiUpper = ((intmax_t*)(((char*)data) + len)) - 1;
	for (; pbi <= pbiUpper; pbi++)
		if (*pbi)
			return false; // Check with the biggest int available most of the array, but without aligning it.
	for (char* p = (char*)pbi; p < ((char*)data) + len; p++)
		if (*p)
			return false; // Check end of non aligned array.
	return true;
}

void ATA::HDD_ReadAsync(void (ATA::*drqCMD)())
{
	nsectorLeft = 0;

	if (!HDD_CanAssessOrSetError())
		return;

	nsectorLeft = nsector;
	if (readBufferLen < nsector * 512)
	{
		delete[] readBuffer;
		readBuffer = new u8[nsector * 512];
		readBufferLen = nsector * 512;
	}
	waitingCmd = drqCMD;

	{
		Threading::ScopedLock ioSignallock(ioMutex);
		ioRead = true;
	}
	ioReady.Broadcast();
}

//Note, we don't expect both Async & Sync Reads
//Do one of the other
void ATA::HDD_ReadSync(void (ATA::*drqCMD)())
{
	//unique_lock instead of lock_guard as also used for cv
	Threading::ScopedLock ioWaitHandle(ioMutex);
	//Set ioWrite false to prevent reading & writing at the same time
	const bool ioWritePaused = ioWrite;
	ioWrite = false;

	//wait until thread waiting
	while (!ioThreadIdle_bool)
		ioThreadIdle_cv.Wait(ioMutex);
	ioWaitHandle.Unlock();

	nsectorLeft = 0;

	if (!HDD_CanAssessOrSetError())
	{
		if (ioWritePaused)
		{
			ioWaitHandle.Lock();
			ioWrite = true;
			ioWaitHandle.Unlock();
			ioReady.Broadcast();
		}
		return;
	}

	nsectorLeft = nsector;
	if (readBufferLen < nsector * 512)
	{
		delete[] readBuffer;
		readBuffer = new u8[nsector * 512];
		readBufferLen = nsector * 512;
	}

	IO_Read();

	if (ioWritePaused)
	{
		ioWaitHandle.Lock();
		ioWrite = true;
		ioWaitHandle.Unlock();
		ioReady.Broadcast();
	}

	(this->*drqCMD)();
}

bool ATA::HDD_CanAssessOrSetError()
{
	if (!HDD_CanAccess(&nsector))
	{
		//Read what we can
		regStatus |= (u8)ATA_STAT_ERR;
		regError |= (u8)ATA_ERR_ID;
		if (nsector == -1)
		{
			PostCmdNoData();
			return false;
		}
	}
	return true;
}
void ATA::HDD_SetErrorAtTransferEnd()
{
	u64 currSect = HDD_GetLBA();
	currSect += nsector;
	if ((regStatus & ATA_STAT_ERR) != 0)
	{
		//Error condition
		//Write errored sector to LBA
		currSect++;
		HDD_SetLBA(currSect);
	}
}
