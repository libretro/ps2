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

static void ata_sce_identify_drive(ata_state_t* ata);

void ata_hdd_sce(ata_state_t* ata)
{
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_SCE SONY-SPECIFIC SECURITY CONTROL COMMAND %x\n", ata->regFeature);

	switch (ata->regFeature)
	{
		case 0xEC:
			ata_sce_identify_drive(ata);
			break;
		default:
			pcsx2_log(RETRO_LOG_ERROR, "DEV9: ATA: Unknown SCE command %x\n", ata->regFeature);
			ata_cmd_no_data_abort(ata);
			return;
	}
}
/* Has
 * ATA_SCE_IDENTIFY_DRIVE @ 0xEC
 *
 * ATA_SCE_SECURITY_ERASE_PREPARE @ 0xF1
 * ATA_SCE_SECURITY_ERASE_UNIT
 * ATA_SCE_SECURITY_FREEZE_LOCK
 * ATA_SCE_SECURITY_SET_PASSWORD
 * ATA_SCE_SECURITY_UNLOCK
 *
 * ATA_SCE_SECURITY_WRITE_ID @ 0x20
 * ATA_SCE_SECURITY_READ_ID @ 0x30 */

static void ata_sce_identify_drive(ata_state_t* ata)
{
	ata_pre_cmd(ata);

	/* HDD_IdentifyDevice(); */ /* Maybe? */

	ata->pioDRQEndTransferFunc = NULL;
	ata_drq_cmd_pio_data_to_host(ata, ata->sceSec, 256 * 2, 0, 256 * 2, true);
}
