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

#include <libretro.h>

#include "DEV9/ATA/ATA.h"

void ata_post_cmd_no_data(ata_state_t* ata)
{
	ata->regStatus &= ~ATA_STAT_BUSY;

	if (ata->regControlEnableIRQ)
		_DEV9irq(ATA_INTR_INTRQ, 1);
}

void ata_cmd_no_data_abort(ata_state_t* ata)
{
	ata_pre_cmd(ata);

	ata->regError |= ATA_ERR_ABORT;
	ata->regStatus |= ATA_STAT_ERR;
	ata_post_cmd_no_data(ata);
}

/* GENRAL FEATURE SET */

void ata_hdd_flush_cache(ata_state_t* ata) /* Can't when DRQ set */
{
	if (!ata_pre_cmd(ata))
		return;
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_FlushCache\n");

	ata->awaitFlush = true;
	ata_async(ata, (uint32_t)-1);
}

void ata_hdd_init_dev_parameters(ata_state_t* ata)
{
	ata_pre_cmd(ata); /* Ignore DRDY bit */
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_InitDevParameters\n");

	ata->curSectors = ata->regNsector;
	ata->curHeads = (uint8_t)((ata->regSelect & 0x7) + 1);
	ata_post_cmd_no_data(ata);
}

void ata_hdd_read_verify_sectors(ata_state_t* ata, bool isLBA48)
{
	if (!ata_pre_cmd(ata))
		return;
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_ReadVerifySectors\n");

	ata_ide_cmd_lba48_transform(ata, isLBA48);

	ata_hdd_can_assess_or_set_error(ata);

	ata_post_cmd_no_data(ata);
}

void ata_hdd_seek_cmd(ata_state_t* ata)
{
	if (!ata_pre_cmd(ata))
		return;
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_SeekCmd\n");

	ata->regStatus &= ~ATA_STAT_SEEK;

	if (ata_hdd_can_seek(ata))
	{
		ata->regStatus |= ATA_STAT_ERR;
		ata->regError |= ATA_ERR_ID;
	}
	else
		ata->regStatus |= ATA_STAT_SEEK;

	ata_post_cmd_no_data(ata);
}

void ata_hdd_set_features(ata_state_t* ata)
{
	if (!ata_pre_cmd(ata))
		return;
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_SetFeatures\n");

	switch (ata->regFeature)
	{
		case 0x02:
			ata->fetWriteCacheEnabled = true;
			break;
		case 0x82:
			ata->fetWriteCacheEnabled = false;
			ata->awaitFlush = true; /* Flush Cache */
			return;
		case 0x03: /* Set transfer mode */
		{
			const uint16_t xferMode = (uint16_t)ata->regNsector; /* Set Transfer mode */

			const int mode = xferMode & 0x07;
			switch ((xferMode) >> 3)
			{
				case 0x00: /* pio default */
					/* if mode = 1, disable IORDY */
					pcsx2_log(RETRO_LOG_DEBUG, "DEV9: PIO Default\n");
					ata->pioMode = 4;
					ata->sdmaMode = -1;
					ata->mdmaMode = -1;
					ata->udmaMode = -1;
					break;
				case 0x01: /* pio mode (3,4) */
					pcsx2_log(RETRO_LOG_DEBUG, "DEV9: PIO Mode %i\n", mode);
					ata->pioMode = mode;
					ata->sdmaMode = -1;
					ata->mdmaMode = -1;
					ata->udmaMode = -1;
					break;
				case 0x02: /* Single word dma mode (0,1,2) */
					pcsx2_log(RETRO_LOG_DEBUG, "DEV9: SDMA Mode %i\n", mode);
					/* pioMode = -1; */
					ata->sdmaMode = mode;
					ata->mdmaMode = -1;
					ata->udmaMode = -1;
					break;
				case 0x04: /* Multi word dma mode (0,1,2) */
					pcsx2_log(RETRO_LOG_DEBUG, "DEV9: MDMA Mode %i\n", mode);
					/* pioMode = -1; */
					ata->sdmaMode = -1;
					ata->mdmaMode = mode;
					ata->udmaMode = -1;
					break;
				case 0x08: /* Ulta dma mode (0,1,2,3,4,5,6) */
					pcsx2_log(RETRO_LOG_DEBUG, "DEV9: UDMA Mode %i\n", mode);
					/* pioMode = -1; */
					ata->sdmaMode = -1;
					ata->mdmaMode = -1;
					ata->udmaMode = mode;
					break;
				default:
					pcsx2_log(RETRO_LOG_ERROR, "DEV9: ATA: Unknown transfer mode\n");
					ata_cmd_no_data_abort(ata);
					break;
			}
		}
		break;
		default:
			pcsx2_log(RETRO_LOG_ERROR, "DEV9: ATA: Unknown feature mode\n");
			break;
	}
	ata_post_cmd_no_data(ata);
}

void ata_hdd_set_multiple_mode(ata_state_t* ata)
{
	if (!ata_pre_cmd(ata))
		return;
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_SetMultipleMode\n");

	ata->curMultipleSectorsSetting = ata->regNsector;

	ata_post_cmd_no_data(ata);
}

void ata_hdd_nop(ata_state_t* ata)
{
	if (!ata_pre_cmd(ata))
		return;
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_Nop\n");

	if (ata->regFeature == 0)
	{
		/* This would abort queues if the
		 * PS2 HDD supported them. */
	}
	/* Always ends in error */
	ata->regError |= ATA_ERR_ABORT;
	ata->regStatus |= ATA_STAT_ERR;
	ata_post_cmd_no_data(ata);
}

/* Other Feature Sets */

void ata_hdd_idle(ata_state_t* ata)
{
	long idleTime; /* in seconds */

	if (!ata_pre_cmd(ata))
		return;
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_Idle\n");

	idleTime = 0;
	if (ata->regNsector >= 1 && ata->regNsector <= 240)
		idleTime = 5 * ata->regNsector;
	else if (ata->regNsector >= 241 && ata->regNsector <= 251)
		idleTime = 30 * (ata->regNsector - 240) * 60;
	else
	{
		switch (ata->regNsector)
		{
			case 0:
				idleTime = 0;
				break;
			case 252:
				idleTime = 21 * 60;
				break;
			case 253: /* bettween 8 and 12 hrs */
				idleTime = 10 * 60 * 60;
				break;
			case 254: /* reserved */
				idleTime = -1;
				break;
			case 255:
				idleTime = 21 * 60 + 15;
				break;
			default:
				idleTime = 0;
				break;
		}
	}

	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_Idle for %lis\n", idleTime);
	ata_post_cmd_no_data(ata);
}

void ata_hdd_idle_immediate(ata_state_t* ata)
{
	if (!ata_pre_cmd(ata))
		return;
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_IdleImmediate\n");
	ata_post_cmd_no_data(ata);
}
