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

#include <stdlib.h>
#include <string.h>

#include <libretro.h>

#include "DEV9/ATA/ATA.h"

static void ata_drq_cmd_dma_data_to_host(ata_state_t* ata)
{
	/* Ready to Start DMA */
	ata->regStatus &= ~ATA_STAT_BUSY;
	ata->regStatus |= ATA_STAT_DRQ;
	ata->dmaReady = true;
	_DEV9irq(SPD_INTR_ATA_FIFO_DATA, 1);
	/* PCSX2 will Start DMA */
}

static void ata_post_cmd_dma_data_to_host(ata_state_t* ata)
{
	ata->nsectorLeft = 0;

	ata->regStatus &= ~ATA_STAT_DRQ;
	ata->regStatus &= ~ATA_STAT_BUSY;
	ata->dmaReady = false;

	dev9_irq_cause_clear(SPD_INTR_ATA_FIFO_DATA);
	if (ata->regControlEnableIRQ)
		_DEV9irq(ATA_INTR_INTRQ, 1);
	/* PCSX2 Will Start DMA */
}

static void ata_drq_cmd_dma_data_from_host(ata_state_t* ata)
{
	/* Ready to Start DMA */
	if (!ata_hdd_can_assess_or_set_error(ata))
		return;

	ata->nsectorLeft = ata->nsector;
	ata->currentWrite = (uint8_t*)malloc((size_t)ata->nsector * 512);
	if (!ata->currentWrite)
		abort();
	ata->currentWriteLength = (uint32_t)(ata->nsector * 512);
	ata->currentWriteSectors = (uint64_t)ata_hdd_get_lba(ata);

	ata->regStatus &= ~ATA_STAT_BUSY;
	ata->regStatus |= ATA_STAT_DRQ;
	ata->dmaReady = true;
	_DEV9irq(SPD_INTR_ATA_FIFO_DATA, 1);
	/* PCSX2 will Start DMA */
}

static void ata_post_cmd_dma_data_from_host(ata_state_t* ata)
{
	ata_write_entry_t entry;
	entry.data = ata->currentWrite;
	entry.sector = ata->currentWriteSectors;
	entry.length = ata->currentWriteLength;
	ata_write_queue_enqueue(&ata->writeQueue, &entry);
	ata->currentWrite = NULL;
	ata->currentWriteLength = 0;
	ata->currentWriteSectors = 0;
	ata->nsectorLeft = 0;

	ata->regStatus &= ~ATA_STAT_DRQ;
	ata->dmaReady = false;

	dev9_irq_cause_clear(SPD_INTR_ATA_FIFO_DATA);

	if (ata->fetWriteCacheEnabled)
	{
		ata->regStatus &= ~ATA_STAT_BUSY;
		if (ata->regControlEnableIRQ)
			_DEV9irq(ATA_INTR_INTRQ, 1); /* 0x6C */
	}
	else
		ata->awaitFlush = true;

	ata_async(ata, (uint32_t)-1);
}

void ata_read_dma8_mem(ata_state_t* ata, uint8_t* pMem, int size)
{
	if ((ata->udmaMode >= 0) && dev9_ata_dma_enabled())
	{
		if (size == 0)
			return;
		pcsx2_log(RETRO_LOG_DEBUG, "DEV9: DMA read, size %i, transferred %i, total size %i\n",
				size, ata->rdTransferred, ata->nsector * 512);

		/* read */
		memcpy(pMem, &ata->readBuffer[ata->rdTransferred], (size_t)size);

		ata->rdTransferred += size;

		if (ata->rdTransferred >= ata->nsector * 512)
		{
			ata_hdd_set_error_at_transfer_end(ata);

			ata->nsector = 0;
			ata->rdTransferred = 0;
			ata_post_cmd_dma_data_to_host(ata);
		}
	}
}

void ata_write_dma8_mem(ata_state_t* ata, uint8_t* pMem, int size)
{
	if ((ata->udmaMode >= 0) && dev9_ata_dma_enabled())
	{
		pcsx2_log(RETRO_LOG_DEBUG, "DEV9: DMA write, size %i, transferred %i, total size %i\n",
				size, ata->wrTransferred, ata->nsector * 512);

		/* write */
		memcpy(&ata->currentWrite[ata->wrTransferred], pMem, (size_t)size);

		ata->wrTransferred += size;

		if (ata->wrTransferred >= ata->nsector * 512)
		{
			ata_hdd_set_error_at_transfer_end(ata);

			ata->nsector = 0;
			ata->wrTransferred = 0;
			ata_post_cmd_dma_data_from_host(ata);
		}
	}
}

/* GENRAL FEATURE SET */

void ata_hdd_read_dma(ata_state_t* ata, bool isLBA48)
{
	if (!ata_pre_cmd(ata))
		return;
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_ReadDMA\n");

	ata_ide_cmd_lba48_transform(ata, isLBA48);

	if (!ata_hdd_can_seek(ata))
	{
		ata->regStatus |= ATA_STAT_ERR;
		ata->regError |= ATA_ERR_ID;
		ata_post_cmd_no_data(ata);
		return;
	}

	/* Do Sync Read */
	ata_hdd_read_sync(ata, ata_drq_cmd_dma_data_to_host);
}

void ata_hdd_write_dma(ata_state_t* ata, bool isLBA48)
{
	if (!ata_pre_cmd(ata))
		return;
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_WriteDMA\n");

	ata_ide_cmd_lba48_transform(ata, isLBA48);

	if (!ata_hdd_can_seek(ata))
	{
		ata->regStatus |= ATA_STAT_ERR;
		ata->regError |= ATA_ERR_ID;
		ata_post_cmd_no_data(ata);
		return;
	}

	/* Do Async write */
	ata_drq_cmd_dma_data_from_host(ata);
}
