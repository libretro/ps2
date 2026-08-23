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

#include "DEV9/ATA/ATA.h"

void ata_pre_cmd_execute_device_diag(ata_state_t* ata)
{
	ata->regStatus |= ATA_STAT_BUSY;
	ata->regStatus &= ~ATA_STAT_READY;
	dev9_irq_cause_clear(ATA_INTR_INTRQ);
	/* dev9.spd.regIntStat &= unchecked((UInt16)~DEV9Header.ATA_INTR_DMA_RDY); */ /* Is this correct? */
}

static void ata_post_cmd_execute_device_diag(ata_state_t* ata)
{
	ata->regStatus &= ~ATA_STAT_BUSY;
	ata->regStatus |= ATA_STAT_READY;

	ata_set_selected_device(ata, 0);

	if (ata->regControlEnableIRQ)
		_DEV9irq(ATA_INTR_INTRQ, 1);
}

/* GENRAL FEATURE SET */

void ata_hdd_execute_device_diag(ata_state_t* ata)
{
	ata_pre_cmd_execute_device_diag(ata);
	/* Perform Self Diag
	 * Would check both drives, but the PS2 would only have 1 */
	ata->regError &= ~ATA_ERR_ICRC;
	/* Passed self-Diag */
	ata->regError = (0x01 | (ata->regError & ATA_ERR_ICRC));

	ata->regNsector = 1;
	ata->regSector = 1;
	ata->regLcyl = 0;
	ata->regHcyl = 0;

	ata->regStatus &= ~ATA_STAT_DRQ;
	ata->regStatus &= ~ATA_STAT_ECC;
	ata->regStatus &= ~ATA_STAT_ERR;

	ata_post_cmd_execute_device_diag(ata);
}
