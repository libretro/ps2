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

#include "DEV9/ATA/ATA.h"

static void ata_hdd_read_pio(ata_state_t* ata, bool isLBA48);
static void ata_hdd_read_pio_s2(ata_state_t* ata);
static void ata_hdd_read_pio_end_block(ata_state_t* ata);

void ata_drq_cmd_pio_data_to_host(ata_state_t* ata, uint8_t* buff, int buffLen,
		int buffIndex, int size, bool sendIRQ)
{
	/* Data in PIO ready to be sent */
	ata->pioPtr = 0;
	ata->pioEnd = size >> 1;

	memcpy(ata->pioBuffer, &buff[buffIndex],
			(size_t)(size < (buffLen - buffIndex) ? size : (buffLen - buffIndex)));

	ata->regStatus &= ~ATA_STAT_BUSY;
	ata->regStatus |= ATA_STAT_DRQ;

	if (ata->regControlEnableIRQ && sendIRQ)
		_DEV9irq(ATA_INTR_INTRQ, 1); /* 0x6c cycles before */
}

static void ata_post_cmd_pio_data_to_host(ata_state_t* ata)
{
	ata->pioPtr = 0;
	ata->pioEnd = 0;
	/* AnyMoreData? */
	if (ata->pioDRQEndTransferFunc)
	{
		ata->regStatus |= ATA_STAT_BUSY;
		ata->regStatus &= ~ATA_STAT_DRQ;
		/* Call cmd to retrive more data */
		ata->pioDRQEndTransferFunc(ata);
	}
	else
		ata->regStatus &= ~ATA_STAT_DRQ;
}

/* FromHost */
uint16_t ata_read_pio(ata_state_t* ata)
{
	/* pcsx2_log(RETRO_LOG_DEBUG, "DEV9: *ATA_R_DATA 16bit read, pio_count %i,  pio_size %i\n", ata->pioPtr, ata->pioEnd); */
	if (ata->pioPtr < ata->pioEnd)
	{
		uint16_t ret;
		memcpy(&ret, &ata->pioBuffer[ata->pioPtr * 2], sizeof(ret));
		/* pcsx2_log(RETRO_LOG_DEBUG, "DEV9: *ATA_R_DATA returned value is  %x\n", ret); */
		ata->pioPtr++;
		if (ata->pioPtr >= ata->pioEnd) /* Fnished transfer (Changed from MegaDev9) */
			ata_post_cmd_pio_data_to_host(ata);

		return ret;
	}
	return 0xFF;
}
/* ATAwritePIO */

void ata_hdd_identify_device(ata_state_t* ata)
{
	if (!ata_pre_cmd(ata))
		return;
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HddidentifyDevice\n");

	/* IDE transfer start */
	ata_create_hdd_info(ata, ata->hddSizeSectors);

	ata->pioDRQEndTransferFunc = NULL;
	ata_drq_cmd_pio_data_to_host(ata, ata->identifyData, 256 * 2, 0, 256 * 2, true);
}

/* Read Buffer */

void ata_hdd_read_multiple(ata_state_t* ata, bool isLBA48)
{
	ata->sectorsPerInterrupt = ata->curMultipleSectorsSetting;
	ata_hdd_read_pio(ata, isLBA48);
}

void ata_hdd_read_sectors(ata_state_t* ata, bool isLBA48)
{
	ata->sectorsPerInterrupt = 1;
	ata_hdd_read_pio(ata, isLBA48);
}

static void ata_hdd_read_pio(ata_state_t* ata, bool isLBA48)
{
	if (!ata_pre_cmd(ata))
		return;

	if (ata->sectorsPerInterrupt == 0)
	{
		ata_cmd_no_data_abort(ata);
		return;
	}

	ata_ide_cmd_lba48_transform(ata, isLBA48);

	if (!ata_hdd_can_seek(ata))
	{
		ata->regStatus |= ATA_STAT_ERR;
		ata->regError |= ATA_ERR_ID;
		ata_post_cmd_no_data(ata);
		return;
	}

	ata_hdd_read_sync(ata, ata_hdd_read_pio_s2);
}

static void ata_hdd_read_pio_s2(ata_state_t* ata)
{
	ata->pioDRQEndTransferFunc = ata_hdd_read_pio_end_block;
	ata_drq_cmd_pio_data_to_host(ata, ata->readBuffer, ata->readBufferLen, 0, 256 * 2, true);
}

static void ata_hdd_read_pio_end_block(ata_state_t* ata)
{
	ata->rdTransferred += 512;
	if (ata->rdTransferred >= ata->nsector * 512)
	{
		ata_hdd_set_error_at_transfer_end(ata);
		ata->regStatus &= ~ATA_STAT_BUSY;
		ata->pioDRQEndTransferFunc = NULL;
		ata->rdTransferred = 0;
	}
	else
	{
		if ((ata->rdTransferred / 512) % ata->sectorsPerInterrupt == 0)
			ata_drq_cmd_pio_data_to_host(ata, ata->readBuffer, ata->readBufferLen, ata->rdTransferred, 256 * 2, true);
		else
			ata_drq_cmd_pio_data_to_host(ata, ata->readBuffer, ata->readBufferLen, ata->rdTransferred, 256 * 2, false);
	}
}

/* Write Buffer */

/* Write Multiple */

/* Write Sectors */

/* Download Microcode (Used for FW updates) */
