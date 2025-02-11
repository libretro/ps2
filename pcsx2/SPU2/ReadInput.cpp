/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

#include "Global.h"
#include "Dma.h"
#include "../IopDma.h"
#include "../IopHw.h"

#include "spu2.h"

// Core 0 Input is "SPDIF mode" - Source audio is AC3 compressed.

// Core 1 Input is "CDDA mode" - Source audio data is 32 bits.
// PS2 note:  Very! few PS2 games use this mode.  Some PSX games used it, however no
// *known* PS2 game does since it was likely only available if the game was recorded to CD
// media (ie, not available in DVD mode, which almost all PS2 games use).  Plus PS2 games
// generally prefer to use ADPCM streaming audio since they need as much storage space as
// possible for FMVs and high-def textures.
//
StereoOut32 V_Core::ReadInput_HiFi()
{
	StereoOut32 retval;
	const u16 ReadIndex = (OutPos * 2) & 0x1FF;

	retval.Left  = (s32&)(*GetMemPtr(0x2000 + (Index << 10) + ReadIndex));
	retval.Right = (s32&)(*GetMemPtr(0x2200 + (Index << 10) + ReadIndex));

	if (Index == 1)
	{
		retval.Left >>= 16;
		retval.Right >>= 16;
	}

	// Simulate MADR increase, GTA VC tracks the MADR address for calculating a certain point in the buffer
	if (InputDataTransferred)
	{
		u32 amount = std::min(InputDataTransferred, (u32)0x180);

		InputDataTransferred -= amount;
		MADR += amount;
		// Because some games watch the MADR to see when it reaches the end we need to end the DMA here
		// Tom & Jerry War of the Whiskers is one such game, the music will skip
		if (!InputDataTransferred && !InputDataLeft)
		{
			if (Index == 0)
			{
				if (Cores[0].DmaMode)
					Cores[0].Regs.STATX |= 0x80;
				Cores[0].Regs.STATX &= ~0x400;
				Cores[0].TSA = Cores[0].ActiveTSA;
				if (HW_DMA4_CHCR & 0x01000000)
				{
					HW_DMA4_CHCR &= ~0x01000000;
					psxDmaInterrupt(4);
				}
			}
			else
			{
				if (Cores[1].DmaMode)
					Cores[1].Regs.STATX |= 0x80;
				Cores[1].Regs.STATX &= ~0x400;
				Cores[1].TSA = Cores[1].ActiveTSA;
				if (HW_DMA7_CHCR & 0x01000000)
				{
					HW_DMA7_CHCR &= ~0x01000000;
					psxDmaInterrupt2(0);
				}
			}
		}
	}

	if (ReadIndex == 0x100 || ReadIndex == 0x0 || ReadIndex == 0x80 || ReadIndex == 0x180)
	{
		if (ReadIndex == 0x100)
			InputPosWrite = 0;
		else if (ReadIndex == 0)
			InputPosWrite = 0x100;

		if (InputDataLeft >= 0x100)
		{
			AutoDMAReadBuffer(0);
			AdmaInProgress = 1;
			if (InputDataLeft < 0x100)
				InputDataLeft = 0;
		}
		else if ((AutoDMACtrl & (Index + 1)))
			AutoDMACtrl |= ~3;
	}
	return retval;
}

StereoOut32 V_Core::ReadInput()
{
	StereoOut32 retval;
	u16 ReadIndex = OutPos;

	for (int i = 0; i < 2; i++)
		if (Cores[i].IRQEnable && (0x2000 + (Index << 10) + ReadIndex) == (Cores[i].IRQA & 0xfffffdff))
			has_to_call_irq[i] = true;

	// PlayMode & 2 is Bypass Mode, so it doesn't go through the SPU
	if ((Index == 1) || !(Index == 0 && (PlayMode & 2) != 0))
	{
		retval.Left  = (s32)(*GetMemPtr(0x2000 + (Index << 10) + ReadIndex));
		retval.Right = (s32)(*GetMemPtr(0x2200 + (Index << 10) + ReadIndex));
	}

	// Simulate MADR increase, GTA VC tracks the MADR address for calculating a certain point in the buffer
	if (InputDataTransferred)
	{
		u32 amount = std::min(InputDataTransferred, (u32)0x180);

		InputDataTransferred -= amount;
		MADR += amount;

		// Because some games watch the MADR to see when it reaches the end we need to end the DMA here
		// Tom & Jerry War of the Whiskers is one such game, the music will skip
		if (!InputDataTransferred && !InputDataLeft)
		{
			if (Index == 0)
			{
				if (Cores[0].DmaMode)
					Cores[0].Regs.STATX |= 0x80;
				Cores[0].Regs.STATX &= ~0x400;
				Cores[0].TSA = Cores[0].ActiveTSA;
				if (HW_DMA4_CHCR & 0x01000000)
				{
					HW_DMA4_CHCR &= ~0x01000000;
					psxDmaInterrupt(4);
				}
			}
			else
			{
				if (Cores[1].DmaMode)
					Cores[1].Regs.STATX |= 0x80;
				Cores[1].Regs.STATX &= ~0x400;
				Cores[1].TSA = Cores[1].ActiveTSA;
				if (HW_DMA7_CHCR & 0x01000000)
				{
					HW_DMA7_CHCR &= ~0x01000000;
					psxDmaInterrupt2(0);
				}
			}
		}
	}

	if (PlayMode == 2 && Index == 0) //Bitstream bypass refills twice as quickly (GTA VC)
		ReadIndex = (ReadIndex * 2) & 0x1FF;

	if (ReadIndex == 0x100 || ReadIndex == 0x0 || ReadIndex == 0x80 || ReadIndex == 0x180)
	{
		if (ReadIndex == 0x100)
			InputPosWrite = 0;
		else if (ReadIndex == 0)
			InputPosWrite = 0x100;

		if (InputDataLeft >= 0x100)
		{
			AutoDMAReadBuffer(0);
			AdmaInProgress = 1;
			if (InputDataLeft < 0x100)
				InputDataLeft = 0;
		}
		else if ((AutoDMACtrl & (Index + 1)))
			AutoDMACtrl |= ~3;
	}
	return retval;
}
