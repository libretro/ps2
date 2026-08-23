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

static void ata_hdd_unk(ata_state_t* ata);

void ata_ide_exec_cmd(ata_state_t* ata, uint16_t value)
{
	switch (value)
	{
		case 0x00:
			ata_hdd_nop(ata);
			break;
		case 0x20:
			ata_hdd_read_sectors(ata, false);
			break;
			/* 0x21 */
		case 0x40:
			ata_hdd_read_verify_sectors(ata, false);
			break;
			/* 0x41 */
		case 0x70:
			ata_hdd_seek_cmd(ata);
			break;
		case 0x90:
			ata_hdd_execute_device_diag(ata);
			break;
		case 0x91:
			ata_hdd_init_dev_parameters(ata);
			break;
		case 0xB0:
			ata_hdd_smart(ata);
			break;
		case 0xC4:
			ata_hdd_read_multiple(ata, false);
			break;
		case 0xC8:
			ata_hdd_read_dma(ata, false);
			break;
			/* 0xC9 */
		case 0xCA:
			ata_hdd_write_dma(ata, false);
			break;
			/* 0xCB */
			/* 0x25 = HDDreadDMA48; */
			/* 0x35 = HDDwriteDMA48; */
		case 0xE1:
			ata_hdd_idle_immediate(ata);
			break;
		case 0xE3:
			ata_hdd_idle(ata);
			break;
		case 0xE7:
			ata_hdd_flush_cache(ata);
			break;
			/* 0xEA = HDDflushCache48 */
		case 0xEC:
			ata_hdd_identify_device(ata);
			break;
			/* 0xA1 = HDDidentifyPktDevice */
		case 0xEF:
			ata_hdd_set_features(ata);
			break;

			/* 0xF1 = HDDsecSetPassword
			 * 0xF2 = HDDsecUnlock
			 * 0xF3 = HDDsecErasePrepare;
			 * 0xF4 = HDDsecEraseUnit; */

			/* This command is Sony-specific and isn't part of the IDE standard */
			/* The Sony HDD has a modified firmware that supports this command */
			/* Sending this command to a standard HDD will give an error */
			/* We roughly emulate it to make programs think the HDD is a Sony one */
			/* However, we only send null, if anyting checks the returned data */
			/* it will fail */
		case 0x8E:
			ata_hdd_sce(ata);
			break;

		default:
			ata_hdd_unk(ata);
			break;
	}
}

static void ata_hdd_unk(ata_state_t* ata)
{
	pcsx2_log(RETRO_LOG_ERROR, "DEV9: ATA: Unknown cmd %x\n", ata->regCommand);

	ata_pre_cmd(ata);

	ata->regError |= ATA_ERR_ABORT;
	ata->regStatus |= ATA_STAT_ERR;
	ata_post_cmd_no_data(ata);
}

bool ata_pre_cmd(ata_state_t* ata)
{
	if ((ata->regStatus & ATA_STAT_READY) == 0)
	{
		/* Ignore CMD write except for EXECUTE DEVICE DIAG and INITIALIZE DEVICE PARAMETERS */
		return false;
	}
	ata->regStatus |= ATA_STAT_BUSY;

	ata->regStatus &= ~ATA_STAT_WRERR;
	ata->regStatus &= ~ATA_STAT_DRQ;
	ata->regStatus &= ~ATA_STAT_ERR;

	ata->regStatus &= ~ATA_STAT_SEEK;

	ata->regError = 0;

	return true;
}

void ata_ide_cmd_lba48_transform(ata_state_t* ata, bool islba48)
{
	ata->lba48 = islba48;
	/* TODO */
	/* handle the 'magic' 0 nsector count conversion here. to avoid
             * fiddling with the rest of the read logic, we just store the
             * full sector count in ->nsector
             */
	if (!ata->lba48)
	{
		if (ata->regNsector == 0)
			ata->nsector = 256;
		else
			ata->nsector = ata->regNsector;
	}
	else
	{
		if (ata->regNsector == 0 && ata->regNsectorHOB == 0)
			ata->nsector = 65536;
		else
		{
			const int lo = ata->regNsector;
			const int hi = ata->regNsectorHOB;

			ata->nsector = (hi << 8) | lo;
		}
	}
}

/* OTHER FEATURE SETS BELOW (TODO?)
 *
 * CFA ERASE SECTORS
 * WRITE MULTIPLE
 * SET MULTIPLE
 *
 * CFA WRITE MULTIPLE WITHOUT ERASE
 * GET MEDIA STATUS
 * MEDIA LOCK
 * MEDIA UNLOCK
 * STANDBY IMMEDIAYTE
 * STANBY
 *
 * CHECK POWER MODE
 * SLEEP
 *
 * MEDIA EJECT
 *
 * SECURITY SET PASSWORD
 * SECURITY UNLOCK
 * SECUTIRY ERASE PREPARE
 * SECURITY ERASE UNIT
 * SECURITY FREEZE LOCK
 * SECURITY DIABLE PASSWORD
 * READ NATIVE MAX ADDRESS
 * SET MAX ADDRESS */
