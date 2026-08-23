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

static void ata_smart_enable_ops(ata_state_t* ata, bool enable);
static void ata_smart_set_auto_save_attribute(ata_state_t* ata);
static void ata_smart_execute_offline_immediate(ata_state_t* ata);
static void ata_smart_return_status(ata_state_t* ata);

void ata_hdd_smart(ata_state_t* ata)
{
	pcsx2_log(RETRO_LOG_DEBUG, "DEV9: HDD_Smart\n");

	if ((ata->regStatus & ATA_STAT_READY) == 0)
		return;

	if (ata->regHcyl != 0xC2 || ata->regLcyl != 0x4F)
	{
		ata_cmd_no_data_abort(ata);
		return;
	}

	if (!ata->fetSmartEnabled && ata->regFeature != 0xD8)
	{
		ata_cmd_no_data_abort(ata);
		return;
	}

	switch (ata->regFeature)
	{
		case 0xD9: /* SMART_DISABLE */
			ata_smart_enable_ops(ata, false);
			return;
		case 0xD8: /* SMART_ENABLE */
			ata_smart_enable_ops(ata, true);
			return;
		case 0xD2: /* SMART_ATTR_AUTOSAVE */
			ata_smart_set_auto_save_attribute(ata);
			return;
		case 0xD3: /* SMART_ATTR_SAVE */
			return;
		case 0xDA: /* SMART_STATUS (is fault in disk?) */
			ata_smart_return_status(ata);
			return;
		case 0xD1: /* SMART_READ_THRESH */
			pcsx2_log(RETRO_LOG_ERROR, "DEV9: ATA: SMART_READ_THRESH Not Implemented\n");
			ata_cmd_no_data_abort(ata);
			return;
		case 0xD0: /* SMART_READ_DATA */
			pcsx2_log(RETRO_LOG_ERROR, "DEV9: ATA: SMART_READ_DATA Not Implemented\n");
			ata_cmd_no_data_abort(ata);
			return;
		case 0xD5: /* SMART_READ_LOG */
			pcsx2_log(RETRO_LOG_ERROR, "DEV9: ATA: SMART_READ_LOG Not Implemented\n");
			ata_cmd_no_data_abort(ata);
			return;
		case 0xD4: /* SMART_EXECUTE_OFFLINE */
			ata_smart_execute_offline_immediate(ata);
			return;
		default:
			pcsx2_log(RETRO_LOG_ERROR, "DEV9: ATA: Unknown SMART command %x\n", ata->regFeature);
			ata_cmd_no_data_abort(ata);
			return;
	}
}

static void ata_smart_set_auto_save_attribute(ata_state_t* ata)
{
	ata_pre_cmd(ata);
	switch (ata->regSector)
	{
		case 0x00:
			ata->smartAutosave = false;
			break;
		case 0xF1:
			ata->smartAutosave = true;
			break;
		default:
			/* The C++ version formatted this integer register with %s
			 * and would have crashed had the path ever been hit. */
			pcsx2_log(RETRO_LOG_ERROR, "DEV9: ATA: Unknown SMART_ATTR_AUTOSAVE command %x\n", ata->regSector);
			ata_cmd_no_data_abort(ata);
			return;
	}
	ata_post_cmd_no_data(ata);
}

static void ata_smart_execute_offline_immediate(ata_state_t* ata)
{
	int n = 0;

	ata_pre_cmd(ata);
	switch (ata->regSector)
	{
		case 0: /* off-line routine */
		case 1: /* short self test */
		case 2: /* extended self test */
			ata->smartSelfTestCount++;
			if (ata->smartSelfTestCount > 21)
				ata->smartSelfTestCount = 1;

			n = 2 + (ata->smartSelfTestCount - 1) * 24;
			/* s->smart_selftest_data[n] = s->sector; */
			/* s->smart_selftest_data[n + 1] = 0x00; */ /* OK and finished */
			/* s->smart_selftest_data[n + 2] = 0x34; */ /* hour count lsb */
			/* s->smart_selftest_data[n + 3] = 0x12; */ /* hour count msb */
			break;
		case 127: /* abort off-line routine */
			break;
		case 129: /* short self test, which holds BSY until complete */
		case 130: /* extended self test, which holds BSY until complete */
			ata->smartSelfTestCount++;
			if (ata->smartSelfTestCount > 21)
			{
				ata->smartSelfTestCount = 1;
			}
			n = 2 + (ata->smartSelfTestCount - 1) * 24;

			ata_smart_return_status(ata);
			return;
		default:
			ata_cmd_no_data_abort(ata);
			return;
	}
	(void)n; /* consumed by the commented-out selftest log above */
	ata_post_cmd_no_data(ata);
}

static void ata_smart_enable_ops(ata_state_t* ata, bool enable)
{
	ata_pre_cmd(ata);
	ata->fetSmartEnabled = enable;
	ata_post_cmd_no_data(ata);
}

static void ata_smart_return_status(ata_state_t* ata)
{
	ata_pre_cmd(ata);
	if (!ata->smartErrors)
	{
		ata->regHcyl = 0xC2;
		ata->regLcyl = 0x4F;
	}
	else
	{
		ata->regHcyl = 0x2C;
		ata->regLcyl = 0xF4;
	}
	ata_post_cmd_no_data(ata);
}
