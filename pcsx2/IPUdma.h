/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

#pragma once

#include "IPU.h"

struct IPUDMAStatus
{
	bool InProgress;
	bool DMAFinished;
};

struct IPUStatus
{
	bool DataRequested;
	bool WaitingOnIPUFrom;
	bool WaitingOnIPUTo;
};

extern IPUDMAStatus IPU1Status;
extern IPUStatus IPUCoreStatus;

extern void ipu0Interrupt(void);
extern void ipu1Interrupt(void);

extern void dmaIPU0(void);
extern void dmaIPU1(void);
extern void IPU0dma(void);
extern void IPU1dma(void);

extern void ipuDmaReset(void);
