/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include <array>
#include <cstring> /* memset */
#include <deque>
#include <vector>

#include <file/file_path.h>

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "MemoryCardProtocol.h"
#include "MultitapProtocol.h"

#include "Config.h"
#include "Host.h"
#include "R3000A.h"
#include "IopHw.h"
#include "IopDma.h"

#include "Common.h"
#include "Memory.h"
#include "Sio.h"
#include "PAD/PAD.h"

#define MCD_SIZE 131072 /* Legacy PSX card default size = 1024 * 8 * 16 = 131072 */

#define MC2_MBSIZE 1081344 // Size of a single megabyte of card data = 1024 * 528 * 2 = 1081344

#define MC2_ERASE_SIZE 8448 /* 528 * 16 */

static std::deque<u8> fifoIn;
static std::deque<u8> fifoOut;

static _mcd mcds[2][4];
static _mcd *mcd;

static MemoryCardProtocol g_MemoryCardProtocol;
static MultitapProtocol g_MultitapProtocol;

static bool FileMcd_Open = false;

Sio0 sio0;
Sio2 sio2;

void MultitapProtocol::SupportCheck()
{
	fifoOut.push_back(0x5a);
	fifoOut.push_back(0x04);
	fifoOut.push_back(0x00);
	fifoOut.push_back(0x5a);
}

void MultitapProtocol::Select()
{
	const u8 newSlot = fifoIn.front();
	fifoIn.pop_front();
	const bool isInBounds = (newSlot < SIO::SLOTS);

	if (isInBounds)
		sio2.slot = newSlot;

	fifoOut.push_back(0x5a);
	fifoOut.push_back(0x00);
	fifoOut.push_back(0x00);
	fifoOut.push_back(isInBounds ? newSlot : 0xff);
	fifoOut.push_back(isInBounds ? 0x5a : 0x66);
}

MultitapProtocol::MultitapProtocol() = default;
MultitapProtocol::~MultitapProtocol() = default;

void MultitapProtocol::SoftReset()
{
}

void MultitapProtocol::FullReset()
{
	SoftReset();

	sio2.slot = 0;
}

void MultitapProtocol::SendToMultitap()
{
	const u8 commandByte = fifoIn.front();
	fifoIn.pop_front();
	fifoOut.push_back(0x80);

	switch (static_cast<MultitapMode>(commandByte))
	{
		case MultitapMode::PAD_SUPPORT_CHECK:
		case MultitapMode::MEMCARD_SUPPORT_CHECK:
			SupportCheck();
			break;
		case MultitapMode::SELECT_PAD:
		case MultitapMode::SELECT_MEMCARD:
			Select();
			break;
		default:
			break;
	}
}

// A repeated pattern in memcard commands is to pad with zero bytes,
// then end with 0x2b and terminator bytes. This function is a shortcut for that.
#define The2bTerminator(length) \
	while (fifoOut.size() < (length) - 2) \
		fifoOut.push_back(0x00); \
	fifoOut.push_back(0x2b); \
	fifoOut.push_back(mcd->term)

void MemoryCardProtocol::ResetPS1State()
{
	ps1McState.currentByte = 2;
	ps1McState.sectorAddrMSB = 0;
	ps1McState.sectorAddrLSB = 0;
	ps1McState.checksum = 0;
	ps1McState.expectedChecksum = 0;
	memset(ps1McState.buf, 0, sizeof(ps1McState.buf));
}

void MemoryCardProtocol::Probe()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	The2bTerminator(4);
}

void MemoryCardProtocol::UnknownWriteDeleteEnd()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	The2bTerminator(4);
}

void MemoryCardProtocol::SetSector()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	const u8 sectorLSB = fifoIn.front();
	fifoIn.pop_front();
	const u8 sector2nd = fifoIn.front();
	fifoIn.pop_front();
	const u8 sector3rd = fifoIn.front();
	fifoIn.pop_front();
	const u8 sectorMSB = fifoIn.front();
	fifoIn.pop_front();
	const u8 expectedChecksum = fifoIn.front();
	fifoIn.pop_front();

	u8 computedChecksum = sectorLSB ^ sector2nd ^ sector3rd ^ sectorMSB;
	mcd->goodSector = (computedChecksum == expectedChecksum);

	u32 newSector = sectorLSB | (sector2nd << 8) | (sector3rd << 16) | (sectorMSB << 24);
	mcd->sectorAddr = newSector;

	McdSizeInfo info;
	FileMcd_GetSizeInfo(mcd->port, mcd->slot, &info);
	mcd->transferAddr = (info.SectorSize + 16) * mcd->sectorAddr;

	The2bTerminator(9);
}

void MemoryCardProtocol::GetSpecs()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	McdSizeInfo info;
	FileMcd_GetSizeInfo(mcd->port, mcd->slot, &info);
	fifoOut.push_back(0x2b);
	
	const u8 sectorSizeLSB = (info.SectorSize & 0xff);
	//checksum ^= sectorSizeLSB;
	fifoOut.push_back(sectorSizeLSB);

	const u8 sectorSizeMSB = (info.SectorSize >> 8);
	//checksum ^= sectorSizeMSB;
	fifoOut.push_back(sectorSizeMSB);

	const u8 eraseBlockSizeLSB = (info.EraseBlockSizeInSectors & 0xff);
	//checksum ^= eraseBlockSizeLSB;
	fifoOut.push_back(eraseBlockSizeLSB);

	const u8 eraseBlockSizeMSB = (info.EraseBlockSizeInSectors >> 8);
	//checksum ^= eraseBlockSizeMSB;
	fifoOut.push_back(eraseBlockSizeMSB);

	const u8 sectorCountLSB = (info.McdSizeInSectors & 0xff);
	//checksum ^= sectorCountLSB;
	fifoOut.push_back(sectorCountLSB);

	const u8 sectorCount2nd = (info.McdSizeInSectors >> 8);
	//checksum ^= sectorCount2nd;
	fifoOut.push_back(sectorCount2nd);

	const u8 sectorCount3rd = (info.McdSizeInSectors >> 16);
	//checksum ^= sectorCount3rd;
	fifoOut.push_back(sectorCount3rd);

	const u8 sectorCountMSB = (info.McdSizeInSectors >> 24);
	//checksum ^= sectorCountMSB;
	fifoOut.push_back(sectorCountMSB);
	
	fifoOut.push_back(info.Xor);
	fifoOut.push_back(mcd->term);
}

void MemoryCardProtocol::SetTerminator()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	const u8 newTerminator = fifoIn.front();
	fifoIn.pop_front();
	const u8 oldTerminator = mcd->term;
	mcd->term = newTerminator;
	fifoOut.push_back(0x00);
	fifoOut.push_back(0x2b);
	fifoOut.push_back(oldTerminator);
}

void MemoryCardProtocol::GetTerminator()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	fifoOut.push_back(0x2b);
	fifoOut.push_back(mcd->term);
	fifoOut.push_back(static_cast<u8>(Terminator::DEFAULT));
}

void MemoryCardProtocol::WriteData()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	fifoOut.push_back(0x00);
	fifoOut.push_back(0x2b);
	const u8 writeLength = fifoIn.front();
	fifoIn.pop_front();
	u8 checksum = 0x00;
	std::vector<u8> buf;

	for (size_t writeCounter = 0; writeCounter < writeLength; writeCounter++)
	{
		const u8 writeByte = fifoIn.front();
		fifoIn.pop_front();
		checksum ^= writeByte;
		buf.push_back(writeByte);
		fifoOut.push_back(0x00);
	}

	FileMcd_Save(mcd->port, mcd->slot, buf.data(), mcd->transferAddr, buf.size());
	fifoOut.push_back(checksum);
	fifoOut.push_back(mcd->term);

	/* After one read or write, the memcard is almost certainly going to 
	 * be issued a new read or write for the next segment of the same sector. 
	 * Bump the transferAddr to where that segment begins.
	 * If it is the end and a new sector is being accessed, 
	 * the SetSector function will deal with both sectorAddr and transferAddr. */
	mcd->transferAddr += writeLength;
}

void MemoryCardProtocol::ReadData()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	const u8 readLength = fifoIn.front();
	fifoIn.pop_front();
	fifoOut.push_back(0x00);
	fifoOut.push_back(0x2b);
	std::vector<u8> buf;
	buf.resize(readLength);
	FileMcd_Read(mcd->port, mcd->slot, buf.data(), mcd->transferAddr, buf.size());
	u8 checksum = 0x00;

	for (const u8 readByte : buf)
	{
		checksum ^= readByte;
		fifoOut.push_back(readByte);
	}

	fifoOut.push_back(checksum);
	fifoOut.push_back(mcd->term);

	/* After one read or write, the memcard is almost certainly going to 
	 * be issued a new read or write for the next segment of the same sector. 
	 * Bump the transferAddr to where that segment begins.
	 * If it is the end and a new sector is being accessed, 
	 * the SetSector function will deal with both sectorAddr and transferAddr. */
	mcd->transferAddr += readLength;
}

u8 MemoryCardProtocol::PS1Read(u8 data)
{
	bool sendAck = true;
	u8 ret = 0;

	switch (ps1McState.currentByte)
	{
		case 2:
			ret = 0x5a;
			break;
		case 3:
			ret = 0x5d;
			break;
		case 4:
			ps1McState.sectorAddrMSB = data;
			ret = 0x00;
			break;
		case 5:
			ps1McState.sectorAddrLSB = data;
			ret = 0x00;
			mcd->sectorAddr = ((ps1McState.sectorAddrMSB << 8) | ps1McState.sectorAddrLSB);
			mcd->goodSector = (mcd->sectorAddr <= 0x03ff);
			mcd->transferAddr = 128 * mcd->sectorAddr;
			break;
		case 6:
			ret = 0x5c;
			break;
		case 7:
			ret = 0x5d;
			break;
		case 8:
			ret = ps1McState.sectorAddrMSB;
			break;
		case 9:
			ret = ps1McState.sectorAddrLSB;
			break;
		case 138:
			ret = ps1McState.checksum;
			break;
		case 139:
			ret = 0x47;
			sendAck = false;
			break;
		case 10:
			ps1McState.checksum = ps1McState.sectorAddrMSB ^ ps1McState.sectorAddrLSB;
			FileMcd_Read(mcd->port, mcd->slot, ps1McState.buf, mcd->transferAddr, sizeof(ps1McState.buf));
			/* fallthrough */
		default:
			ret = ps1McState.buf[ps1McState.currentByte - 10];
			ps1McState.checksum ^= ret;
			break;
	}

	if (sendAck)
		sio0.stat |= SIO0_STAT::ACK;

	ps1McState.currentByte++;
	return ret;
}

u8 MemoryCardProtocol::PS1State(u8 data)
{
	return 0x00;
}

u8 MemoryCardProtocol::PS1Write(u8 data)
{
	bool sendAck = true;
	u8 ret = 0;

	switch (ps1McState.currentByte)
	{
		case 2:
			ret = 0x5a;
			break;
		case 3:
			ret = 0x5d;
			break;
		case 4:
			ps1McState.sectorAddrMSB = data;
			ret = 0x00;
			break;
		case 5:
			ps1McState.sectorAddrLSB = data;
			ret = 0x00;
			mcd->sectorAddr = ((ps1McState.sectorAddrMSB << 8) | ps1McState.sectorAddrLSB);
			mcd->goodSector = (mcd->sectorAddr <= 0x03ff);
			mcd->transferAddr = 128 * mcd->sectorAddr;
			break;
		case 134:
			ps1McState.expectedChecksum = data;
			ret = 0;
			break;
		case 135:
			ret = 0x5c;
			break;
		case 136:
			ret = 0x5d;
			break;
		case 137:
			if (!mcd->goodSector)
				ret = 0xff;
			else if (ps1McState.expectedChecksum != ps1McState.checksum)
				ret = 0x4e;
			else
			{
				FileMcd_Save(mcd->port, mcd->slot, ps1McState.buf, mcd->transferAddr, sizeof(ps1McState.buf));
				ret = 0x47;
				// Clear the "directory unread" bit of the flag byte. Per no$psx, this is cleared
				// on writes, not reads.
				mcd->FLAG &= 0x07;
			}

			sendAck = false;
			break;
		case 6:
			ps1McState.checksum = ps1McState.sectorAddrMSB ^ ps1McState.sectorAddrLSB;
			/* fallthrough */
		default:
			ps1McState.buf[ps1McState.currentByte - 6] = data;
			ps1McState.checksum ^= data;
			ret = 0x00;
			break;
	}

	if (sendAck)
		sio0.stat |= SIO0_STAT::ACK;

	ps1McState.currentByte++;
	return ret;
}

u8 MemoryCardProtocol::PS1Pocketstation(u8 data)
{
	sio2.SetRecv1(Recv1::DISCONNECTED);
	return 0x00;
}

void MemoryCardProtocol::ReadWriteEnd()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	The2bTerminator(4);
}

void MemoryCardProtocol::EraseBlock()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	FileMcd_EraseBlock(mcd->port, mcd->slot, mcd->transferAddr);
	The2bTerminator(4);
}

void MemoryCardProtocol::UnknownBoot()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	The2bTerminator(5);
}

void MemoryCardProtocol::AuthXor()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	const u8 modeByte = fifoIn.front();
	fifoIn.pop_front();

	switch (modeByte)
	{
		// When encountered, the command length in RECV3 is guaranteed to be 14,
		// and the PS2 is expecting us to XOR the data it is about to send.
		case 0x01:
		case 0x02:
		case 0x04:
		case 0x0f:
		case 0x11:
		case 0x13:
		{
			// Long + XOR
			fifoOut.push_back(0x00);
			fifoOut.push_back(0x2b);
			u8 xorResult = 0x00;

			for (size_t xorCounter = 0; xorCounter < 8; xorCounter++)
			{
				const u8 toXOR = fifoIn.front();
				fifoIn.pop_front();
				xorResult ^= toXOR;
				fifoOut.push_back(0x00);
			}

			fifoOut.push_back(xorResult);
			fifoOut.push_back(mcd->term);
			break;
		}
		// When encountered, the command length in RECV3 is guaranteed to be 5,
		// and there is no attempt to XOR anything.
		case 0x00:
		case 0x03:
		case 0x05:
		case 0x08:
		case 0x09:
		case 0x0a:
		case 0x0c:
		case 0x0d:
		case 0x0e:
		case 0x10:
		case 0x12:
		case 0x14:
			// Short + No XOR
			The2bTerminator(5);
			break;
		// When encountered, the command length in RECV3 is guaranteed to be 14,
		// and the PS2 is about to send us data, BUT the PS2 does NOT want us
		// to send the XOR, it wants us to send the 0x2b and terminator as the
		// last two bytes.
		case 0x06:
		case 0x07:
		case 0x0b:
			// Long + No XOR
			The2bTerminator(14);
			break;
		default:
			break;
	}
}

void MemoryCardProtocol::AuthF3()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	The2bTerminator(5);
}

void MemoryCardProtocol::AuthF7()
{
	/* Check if the memcard is for PS1, and if we are working on 
	 * a command sent over SIO2. If so, return dead air. */
	if (FileMcd_IsPSX(mcd->port, mcd->slot) && sio2.commandLength > 0)
	{
		while (fifoOut.size() < sio2.commandLength)
			fifoOut.push_back(0x00);
		return;
	}
	The2bTerminator(5);
}

/* ECC code ported from mymc
 * https://sourceforge.net/p/mymc-opl/code/ci/master/tree/ps2mc_ecc.py
 * Public domain license */

static u32 CalculateECC(u8* buf)
{
	const u8 parity_table[256] = {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,
	0,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,
	1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,0,1,1,
	0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,0,
	1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,
	0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,
	1,1,0,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,
	1,0,1,1,0};

	const u8 column_parity_mask[256] = {0,7,22,17,37,34,51,52,52,51,34,37,17,22,
	7,0,67,68,85,82,102,97,112,119,119,112,97,102,82,85,68,67,82,85,68,67,119,112,
	97,102,102,97,112,119,67,68,85,82,17,22,7,0,52,51,34,37,37,34,51,52,0,7,22,17,
	97,102,119,112,68,67,82,85,85,82,67,68,112,119,102,97,34,37,52,51,7,0,17,22,
	22,17,0,7,51,52,37,34,51,52,37,34,22,17,0,7,7,0,17,22,34,37,52,51,112,119,102,
	97,85,82,67,68,68,67,82,85,97,102,119,112,112,119,102,97,85,82,67,68,68,67,82,
	85,97,102,119,112,51,52,37,34,22,17,0,7,7,0,17,22,34,37,52,51,34,37,52,51,7,0,
	17,22,22,17,0,7,51,52,37,34,97,102,119,112,68,67,82,85,85,82,67,68,112,119,102,
	97,17,22,7,0,52,51,34,37,37,34,51,52,0,7,22,17,82,85,68,67,119,112,97,102,102,
	97,112,119,67,68,85,82,67,68,85,82,102,97,112,119,119,112,97,102,82,85,68,67,
	0,7,22,17,37,34,51,52,52,51,34,37,17,22,7,0};

	u8 column_parity = 0x77;
	u8 line_parity_0 = 0x7F;
	u8 line_parity_1 = 0x7F;

	for (int i = 0; i < 128; i++)
	{
		u8 b = buf[i];
		column_parity ^= column_parity_mask[b];
		if (parity_table[b])
		{
			line_parity_0 ^= ~i;
			line_parity_1 ^= i;
		}
	}

	return column_parity | (line_parity_0 << 8) | (line_parity_1 << 16);
}

static bool ConvertNoECCtoRAW(const char* file_in, const char* file_out)
{
	u8 buffer[512];
	RFILE *fin = FileSystem::OpenFile(file_in, "rb");
	if (!fin)
		return false;

	RFILE *fout = FileSystem::OpenFile(file_out, "wb");
	if (!fout)
	{
		filestream_close(fin);
		return false;
	}

	const s64 size = FileSystem::FSize64(fin);

	for (s64 i = 0; i < (size / 512); i++)
	{
		if (rfread(buffer, sizeof(buffer), 1, fin) != 1 ||
			rfwrite(buffer, sizeof(buffer), 1, fout) != 1)
		{
			filestream_close(fin);
			filestream_close(fout);
			return false;
		}

		for (int j = 0; j < 4; j++)
		{
			u32 checksum = CalculateECC(&buffer[j * 128]);
			if (rfwrite(&checksum, 3, 1, fout) != 1)
			{
				filestream_close(fin);
				filestream_close(fout);
				return false;
			}
		}

		u32 nullbytes = 0;
		if (rfwrite(&nullbytes, sizeof(nullbytes), 1, fout) != 1)
		{
			filestream_close(fin);
			filestream_close(fout);
			return false;
		}
	}

	filestream_close(fin);
	if (filestream_flush(fout) != 0)
		return false;
	filestream_close(fout);
	return true;
}

static bool ConvertRAWtoNoECC(const char* file_in, const char* file_out)
{
	u8 buffer[512];
	u8 checksum[16];
	RFILE *fin = FileSystem::OpenFile(file_in, "rb");
	if (!fin)
		return false;

	RFILE *fout = FileSystem::OpenFile(file_out, "wb");
	if (!fout)
		return false;

	const s64 size = FileSystem::FSize64(fin);

	for (s64 i = 0; i < (size / 528); i++)
	{
		if (rfread(buffer, sizeof(buffer), 1, fin) != 1 ||
			rfwrite(buffer, sizeof(buffer), 1, fout) != 1 ||
			rfread(checksum, sizeof(checksum), 1, fin) != 1)
		{
			filestream_close(fin);
			filestream_close(fout);
			return false;
		}
	}

	filestream_close(fin);
	if (filestream_flush(fout) != 0)
		return false;
	filestream_close(fout);
	return true;
}

// --------------------------------------------------------------------------------------
//  FileMemoryCard
// --------------------------------------------------------------------------------------
// Provides thread-safe direct file IO mapping.
//
class FileMemoryCard
{
protected:
	RFILE* m_file[8] = {};
	std::string m_filenames[8] = {};
	std::vector<u8> m_currentdata;
	u64 m_chksum[8] = {};
	bool m_ispsx[8] = {};
	u32 m_chkaddr = 0;

public:
	FileMemoryCard();
	~FileMemoryCard();

	void Lock();
	void Unlock();

	void Open();
	void Close();

	s32 IsPresent(uint slot);
	void GetSizeInfo(uint slot, McdSizeInfo& outways);
	bool IsPSX(uint slot);
	s32 Read(uint slot, u8* dest, u32 adr, int size);
	s32 Save(uint slot, const u8* src, u32 adr, int size);
	s32 EraseBlock(uint slot, u32 adr);
	u64 GetCRC(uint slot);

protected:
	bool Seek(RFILE* f, u32 adr);
	bool Create(const char* mcdFile, uint sizeInMB);
};

uint FileMcd_GetMtapPort(uint slot)
{
	switch (slot)
	{
		case 1:
		case 5:
		case 6:
		case 7:
			return 1;
		case 0:
		case 2:
		case 3:
		case 4:
		default:
			break;
	}

	return 0;
}

// Returns the multitap slot number, range 1 to 3 (slot 0 refers to the standard
// 1st and 2nd player slots).
uint FileMcd_GetMtapSlot(uint slot)
{
	switch (slot)
	{
		case 2:
		case 3:
		case 4:
			return slot - 1;
		case 5:
		case 6:
		case 7:
			return slot - 4;
		case 0:
		case 1:
		default:
			break;
	}

	return 0; // technically unreachable.
}

bool FileMcd_IsMultitapSlot(uint slot)
{
	return (slot > 1);
}

std::string FileMcd_GetDefaultName(uint slot)
{
	if (FileMcd_IsMultitapSlot(slot))
		return StringUtil::StdStringFromFormat("Mcd-Multitap%u-Slot%02u.ps2", FileMcd_GetMtapPort(slot) + 1, FileMcd_GetMtapSlot(slot) + 1);
	return StringUtil::StdStringFromFormat("Mcd%03u.ps2", slot + 1);
}

FileMemoryCard::FileMemoryCard() = default;

FileMemoryCard::~FileMemoryCard() = default;

void FileMemoryCard::Open()
{
	for (int slot = 0; slot < 8; ++slot)
	{
		m_filenames[slot] = {};

		if (FileMcd_IsMultitapSlot(slot))
		{
			if (!EmuConfig.MultitapPort0_Enabled && (FileMcd_GetMtapPort(slot) == 0))
				continue;
			if (!EmuConfig.MultitapPort1_Enabled && (FileMcd_GetMtapPort(slot) == 1))
				continue;
		}

		std::string fname(EmuConfig.FullpathToMcd(slot));
		bool cont = false;

		if (fname.empty())
			cont = true;

		if (!EmuConfig.Mcd[slot].Enabled)
			cont = true;

		if (EmuConfig.Mcd[slot].Type != MemoryCardType::File)
			cont = true;

		if (cont)
			continue;

		/* FIXME : Ideally this should prompt the user for the size of the
		 * memory card file they would like to create, instead of trying to
		 * create one automatically. */
		if (path_get_size(fname.c_str()) <= 0)
			Create(fname.c_str(), 8);

		// [TODO] : Add memcard size detection and report it to the console log.
		//   (8MB, 256Mb, formatted, unformatted, etc ...)

		if (StringUtil::EndsWith(fname, ".bin"))
		{
			std::string newname(fname + "x");
			if (!ConvertNoECCtoRAW(fname.c_str(), newname.c_str()))
			{
				FileSystem::DeleteFilePath(newname.c_str());
				continue;
			}

			// store the original filename
			m_file[slot] = FileSystem::OpenFile(newname.c_str(), "r+b");
		}
		else
			m_file[slot] = FileSystem::OpenFile(fname.c_str(), "r+b");

		if (m_file[slot]) // Load checksum
		{
			m_filenames[slot] = std::move(fname);
			m_ispsx[slot] = FileSystem::FSize64(m_file[slot]) == 0x20000;
			m_chkaddr = 0x210;

			if (!m_ispsx[slot] && FileSystem::FSeek64(m_file[slot], m_chkaddr, SEEK_SET) == 0)
				rfread(&m_chksum[slot], sizeof(m_chksum[slot]), 1, m_file[slot]);
		}
	}
}

void FileMemoryCard::Close()
{
	for (int slot = 0; slot < 8; ++slot)
	{
		if (!m_file[slot])
			continue;

		// Store checksum
		if (!m_ispsx[slot] && FileSystem::FSeek64(m_file[slot], m_chkaddr, SEEK_SET) == 0)
			rfwrite(&m_chksum[slot], sizeof(m_chksum[slot]), 1, m_file[slot]);

		rfclose(m_file[slot]);
		m_file[slot] = nullptr;

		if (StringUtil::EndsWith(m_filenames[slot], ".bin"))
		{
			const std::string name_in(m_filenames[slot] + 'x');
			if (ConvertRAWtoNoECC(name_in.c_str(), m_filenames[slot].c_str()))
				FileSystem::DeleteFilePath(name_in.c_str());
		}

		m_filenames[slot] = {};
	}
}

// Returns FALSE if the seek failed (is outside the bounds of the file).
bool FileMemoryCard::Seek(RFILE* f, u32 adr)
{
	const s64 size = FileSystem::FSize64(f);

	// If anyone knows why this filesize logic is here (it appears to be related to legacy PSX
	// cards, perhaps hacked support for some special emulator-specific memcard formats that
	// had header info?), then please replace this comment with something useful.  Thanks!  -- air

	u32 offset = 0;

	if (size == MCD_SIZE + 64)
		offset = 64;
	else if (size == MCD_SIZE + 3904)
		offset = 3904;
	return (FileSystem::FSeek64(f, adr + offset, SEEK_SET) == 0);
}

// returns FALSE if an error occurred (either permission denied or disk full)
bool FileMemoryCard::Create(const char* mcdFile, uint sizeInMB)
{
	u8 buf[MC2_ERASE_SIZE];
	RFILE *fp = FileSystem::OpenFile(mcdFile, "wb");
	if (!fp)
		return false;

	memset(buf, 0xff, sizeof(buf));

	for (uint i = 0; i < (MC2_MBSIZE * sizeInMB) / sizeof(buf); i++)
	{
		if (rfwrite(buf, sizeof(buf), 1, fp) != 1)
		{
			filestream_close(fp);
			return false;
		}
	}
	filestream_close(fp);
	return true;
}

s32 FileMemoryCard::IsPresent(uint slot)
{
	return m_file[slot] != nullptr;
}

void FileMemoryCard::GetSizeInfo(uint slot, McdSizeInfo& outways)
{
	outways.SectorSize = 512;             // 0x0200
	outways.EraseBlockSizeInSectors = 16; // 0x0010
	outways.Xor = 18;                     // 0x12, XOR 02 00 00 10

	if (m_file[slot])
		outways.McdSizeInSectors = static_cast<u32>(FileSystem::FSize64(m_file[slot])) / (outways.SectorSize + outways.EraseBlockSizeInSectors);
	else
		outways.McdSizeInSectors = 0x4000;

	u8* pdata = (u8*)&outways.McdSizeInSectors;
	outways.Xor ^= pdata[0] ^ pdata[1] ^ pdata[2] ^ pdata[3];
}

bool FileMemoryCard::IsPSX(uint slot)
{
	return m_ispsx[slot];
}

s32 FileMemoryCard::Read(uint slot, u8* dest, u32 adr, int size)
{
	RFILE* mcfp = m_file[slot];
	if (!mcfp)
	{
		memset(dest, 0, size);
		return 1;
	}
	if (!Seek(mcfp, adr))
		return 0;
	return rfread(dest, size, 1, mcfp) == 1;
}

s32 FileMemoryCard::Save(uint slot, const u8* src, u32 adr, int size)
{
	RFILE* mcfp = m_file[slot];

	if (!mcfp)
		return 1;

	if (m_ispsx[slot])
	{
		if (static_cast<int>(m_currentdata.size()) < size)
			m_currentdata.resize(size);
		for (int i = 0; i < size; i++)
			m_currentdata[i] = src[i];
	}
	else
	{
		if (!Seek(mcfp, adr))
			return 0;
		if (static_cast<int>(m_currentdata.size()) < size)
			m_currentdata.resize(size);

		const size_t read_result = rfread(m_currentdata.data(), size, 1, mcfp);

		for (int i = 0; i < size; i++)
			m_currentdata[i] &= src[i];

		// Checksumness
		{
			u64* pdata = (u64*)&m_currentdata[0];
			u32 loops = size / 8;

			for (u32 i = 0; i < loops; i++)
				m_chksum[slot] ^= pdata[i];
		}
	}

	if (!Seek(mcfp, adr))
		return 0;

	if (rfwrite(m_currentdata.data(), size, 1, mcfp) == 1)
		return 1;

	return 0;
}

s32 FileMemoryCard::EraseBlock(uint slot, u32 adr)
{
	u8 buf[MC2_ERASE_SIZE];
	RFILE* mcfp = m_file[slot];
	if (!mcfp)
		return 1;

	if (!Seek(mcfp, adr))
		return 0;
	memset(buf, 0xff, sizeof(buf));
	return rfwrite(buf, sizeof(buf), 1, mcfp) == 1;
}

u64 FileMemoryCard::GetCRC(uint slot)
{
	RFILE* mcfp = m_file[slot];
	if (!mcfp)
		return 0;
	if (m_ispsx[slot])
	{
		u64 retval = 0;
		if (!Seek(mcfp, 0))
			return 0;

		const s64 mcfpsize = FileSystem::FSize64(mcfp);
		if (mcfpsize < 0)
			return 0;

		// Process the file in 4k chunks.  Speeds things up significantly.

		u64 buffer[528 * 8]; // use 528 (sector size), ensures even divisibility

		const uint filesize = static_cast<uint>(mcfpsize) / sizeof(buffer);
		for (uint i = filesize; i; --i)
		{
			if (rfread(buffer, sizeof(buffer), 1, mcfp) != 1)
				return 0;

			for (uint t = 0; t < std::size(buffer); ++t)
				retval ^= buffer[t];
		}
		return retval;
	}
	return m_chksum[slot];
}

// --------------------------------------------------------------------------------------
//  MemoryCard Component API Bindings
// --------------------------------------------------------------------------------------
namespace Mcd
{
	FileMemoryCard impl; // class-based implementations we refer to when API is invoked
}; // namespace Mcd

uint FileMcd_ConvertToSlot(uint port, uint slot)
{
	if (slot == 0)
		return port;
	if (port == 0)
		return slot + 1; // multitap 1
	return slot + 4;     // multitap 2
}

void FileMcd_EmuOpen(void)
{
	if(FileMcd_Open)
		return;
	FileMcd_Open = true;
	// detect inserted memory card types
	for (uint slot = 0; slot < 8; ++slot)
	{
		if (EmuConfig.Mcd[slot].Filename.empty())
			EmuConfig.Mcd[slot].Type = MemoryCardType::Empty;
		else if (EmuConfig.Mcd[slot].Enabled)
			EmuConfig.Mcd[slot].Type = MemoryCardType::File;
	}

	Mcd::impl.Open();
}

void FileMcd_EmuClose(void)
{
	if(!FileMcd_Open)
		return;
	FileMcd_Open = false;
	Mcd::impl.Close();
}

s32 FileMcd_IsPresent(uint port, uint slot)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.IsPresent(combinedSlot);
	return false;
}

void FileMcd_GetSizeInfo(uint port, uint slot, McdSizeInfo* outways)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		Mcd::impl.GetSizeInfo(combinedSlot, *outways);
}

bool FileMcd_IsPSX(uint port, uint slot)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.IsPSX(combinedSlot);
	return false;
}

s32 FileMcd_Read(uint port, uint slot, u8* dest, u32 adr, int size)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.Read(combinedSlot, dest, adr, size);
	return 0;
}

s32 FileMcd_Save(uint port, uint slot, const u8* src, u32 adr, int size)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.Save(combinedSlot, src, adr, size);
	return 0;
}

s32 FileMcd_EraseBlock(uint port, uint slot, u32 adr)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.EraseBlock(combinedSlot, adr);
	return 0;
}

u64 FileMcd_GetCRC(uint port, uint slot)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.GetCRC(combinedSlot);
	return 0;
}

bool FileMcd_ReIndex(uint port, uint slot, const std::string& filter) { return false; }

/* ============================================================================
 * SIO0
 * ============================================================================
 */

Sio0::Sio0()
{
	this->FullReset();
}

Sio0::~Sio0() = default;

void Sio0::SoftReset()
{
	padStarted = false;
	sioMode = SioMode::NOT_SET;
	sioCommand = 0;
	sioStage = SioStage::IDLE;
}

void Sio0::FullReset()
{
	SoftReset();

	port = 0;
	slot = 0;

	for (int i = 0; i < 2; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			mcds[i][j].term = 0x55;
			mcds[i][j].port = i;
			mcds[i][j].slot = j;
			mcds[i][j].FLAG = 0x08;
			mcds[i][j].autoEjectTicks = 0;
		}
	}

	mcd = &mcds[0][0];
}

void Sio0::Interrupt(Sio0Interrupt sio0Interrupt)
{
	if (sio0Interrupt == Sio0Interrupt::TEST_EVENT)
		iopIntcIrq(7);
	else if (sio0Interrupt == Sio0Interrupt::STAT_READ)
		stat &= ~(SIO0_STAT::ACK);
	
	if (!(psxRegs.interrupt & (1 << IopEvt_SIO)))
	{
		PSX_INT(IopEvt_SIO, PSXCLK / 250000); /* PSXCLK/250000); */
	}
}

u8 Sio0::GetRxData()
{
	stat |= (SIO0_STAT::TX_READY | SIO0_STAT::TX_EMPTY);
	stat &= ~(SIO0_STAT::RX_FIFO_NOT_EMPTY);
	return rxData;
}

u32 Sio0::GetStat()
{
	const u32 ret = stat;
	Interrupt(Sio0Interrupt::STAT_READ);
	return ret;
}

void Sio0::SetTxData(u8 value)
{
	stat |= SIO0_STAT::TX_READY | SIO0_STAT::TX_EMPTY;
	stat |= (SIO0_STAT::RX_FIFO_NOT_EMPTY);

	if (!(ctrl & SIO0_CTRL::TX_ENABLE))
		return;

	txData = value;
	u8 res = 0;

	switch (sioStage)
	{
		case SioStage::IDLE:
			sioMode = value;
			stat |= SIO0_STAT::TX_READY;

			switch (sioMode)
			{
				case SioMode::PAD:
					res = PADstartPoll(port, slot);

					if (res)
						stat |= SIO0_STAT::ACK;

					break;
				case SioMode::MEMCARD:
					mcd = &mcds[port][slot];

					// Check if auto ejection is active. If so, set RECV1 to DISCONNECTED,
					// and zero out the fifo to simulate dead air over the wire.
					if (mcd->autoEjectTicks)
					{
						rxData = 0x00;
						mcd->autoEjectTicks--;
						return;
					}
					
					// If memcard is missing, not PS1, or auto ejected, do not let SIO0 stage advance,
					// reply with dead air and no ACK.
					if (!FileMcd_IsPresent(mcd->port, mcd->slot) || !FileMcd_IsPSX(mcd->port, mcd->slot))
					{
						rxData = 0x00;
						return;
					}

					stat |= SIO0_STAT::ACK;
					break;
			}

			rxData   = res;
			sioStage = SioStage::WAITING_COMMAND;
			break;
		case SioStage::WAITING_COMMAND:
			stat &= ~(SIO0_STAT::TX_READY);

			if (IsPadCommand(value))
			{
				res      = PADpoll(value);
				rxData   = res;

				if (!PADcomplete())
					stat |= SIO0_STAT::ACK;

				sioStage = SioStage::WORKING;
			}
			else if (IsMemcardCommand(value))
			{
				rxData     = flag;
				stat      |= SIO0_STAT::ACK;
				sioCommand = value;
				sioStage = SioStage::WORKING;
			}
			else if (IsPocketstationCommand(value))
			{
				// Set the line low, no acknowledge.
				rxData   = 0x00;
				sioStage = SioStage::IDLE;
			}
			else
			{
				rxData   = 0xff;
				SoftReset();
			}

			break;
		case SioStage::WORKING:
			switch (sioMode)
			{
				case SioMode::PAD:
					res = PADpoll(value);
					rxData   = res;

					if (!PADcomplete())
						stat |= SIO0_STAT::ACK;

					break;
				case SioMode::MEMCARD:
					rxData   = Memcard(value);
					break;
				default:
					rxData   = 0xff;
					SoftReset();
					break;
			}

			break;
		default:
			rxData   = 0xff;
			SoftReset();
			break;
	}

	Interrupt(Sio0Interrupt::TX_DATA_WRITE);
}

void Sio0::SetCtrl(u16 value)
{
	ctrl = value;
	port = (ctrl & SIO0_CTRL::PORT) > 0;

	/* CTRL appears to be set to 0 between every "transaction".
	 * Not documented anywhere, but we'll use this to "reset"
	 * the SIO0 state, particularly during the annoying probes
	 * to memcards that occur when a game boots. */
	if (ctrl == 0)
	{
		g_MemoryCardProtocol.ResetPS1State();
		SoftReset();
	}

	/* If CTRL acknowledge, reset STAT bits 3 and 9 */
	if (ctrl & SIO0_CTRL::ACK)
		stat &= ~(SIO0_STAT::IRQ | SIO0_STAT::RX_PARITY_ERROR);

	if (ctrl & SIO0_CTRL::RESET)
	{
		stat = 0;
		ctrl = 0;
		mode = 0;
		SoftReset();
	}
}

bool Sio0::IsPadCommand(u8 command)
{
	return command >= PadCommand::UNK_0 && command <= PadCommand::ANALOG;
}

bool Sio0::IsMemcardCommand(u8 command)
{
	return command == MemcardCommand::PS1_READ || command == MemcardCommand::PS1_STATE || command == MemcardCommand::PS1_WRITE;
}

bool Sio0::IsPocketstationCommand(u8 command)
{
	return command == MemcardCommand::PS1_POCKETSTATION;
}

u8 Sio0::Pad(u8 value)
{
	if (PADcomplete())
		padStarted = false;
	else if (!padStarted)
	{
		padStarted = true;
		PADstartPoll(port, slot);
		stat |= SIO0_STAT::ACK;
	}

	return PADpoll(value);
}

u8 Sio0::Memcard(u8 value)
{
	switch (sioCommand)
	{
		case MemcardCommand::PS1_READ:
			return g_MemoryCardProtocol.PS1Read(value);
		case MemcardCommand::PS1_STATE:
			return g_MemoryCardProtocol.PS1State(value);
		case MemcardCommand::PS1_WRITE:
			return g_MemoryCardProtocol.PS1Write(value);
		case MemcardCommand::PS1_POCKETSTATION:
			return g_MemoryCardProtocol.PS1Pocketstation(value);
		default:
			SoftReset();
			break;
	}

	return 0xff;
}

/* ============================================================================
 * SIO2
 * ============================================================================
 */


Sio2::Sio2()
{
	this->FullReset();
}

Sio2::~Sio2() = default;

void Sio2::SoftReset()
{
	send3Read = false;
	send3Position = 0;
	commandLength = 0;
	processedLength = 0;
	/* Clear dmaBlockSize, in case the next SIO2 command is not sent over DMA11. */
	dmaBlockSize = 0;
	send3Complete = false;

	/* Anything in fifoIn which was not necessary to consume should be cleared out prior to the next SIO2 cycle. */
	while (!fifoIn.empty())
	{
		fifoIn.pop_front();
	}
}

void Sio2::FullReset()
{
	size_t i;
	this->SoftReset();

	for (i = 0; i < 16; i++)
		send3[i] = 0;

	for (i = 0; i < 4; i++)
	{
		send1[i] = 0;
		send2[i] = 0;
	}

	dataIn = 0;
	dataOut = 0;
	SetCtrl(Sio2Ctrl::SIO2MAN_RESET);
	SetRecv1(Recv1::DISCONNECTED);
	recv2 = Recv2::DEFAULT;
	recv3 = Recv3::DEFAULT;
	unknown1 = 0;
	unknown2 = 0;
	iStat = 0;

	port = 0;
	slot = 0;

	while (!fifoOut.empty())
	{
		fifoOut.pop_front();
	}

	for (int i = 0; i < 2; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			mcds[i][j].term = 0x55;
			mcds[i][j].port = i;
			mcds[i][j].slot = j;
			mcds[i][j].FLAG = 0x08;
			mcds[i][j].autoEjectTicks = 0;
		}
	}

	mcd = &mcds[0][0];
}


void Sio2::Interrupt()
{
	iopIntcIrq(17);
}

void Sio2::SetCtrl(u32 value)
{
	this->ctrl = value;

	if (this->ctrl & Sio2Ctrl::START_TRANSFER)
		iopIntcIrq(17);
}

void Sio2::SetSend3(size_t position, u32 value)
{
	this->send3[position] = value;

	if (position == 0)
		SoftReset();
}

void Sio2::SetRecv1(u32 value)
{
	this->recv1 = value;
}

void Sio2::Pad()
{
	// Send PAD our current port, and get back whatever it says the first response byte should be.
	const u8 firstResponseByte = PADstartPoll(port, slot);
	fifoOut.push_back(firstResponseByte);
	// Some games will refuse to read ALL pads, if RECV1 is not set to the CONNECTED value when ANY pad is polled,
	// REGARDLESS of whether that pad is truly connected or not.
	SetRecv1(Recv1::CONNECTED);

	// Then for every byte in fifoIn, pass to PAD and see what it kicks back to us.
	while (!fifoIn.empty())
	{
		const u8 commandByte = fifoIn.front();
		fifoIn.pop_front();
		const u8 responseByte = PADpoll(commandByte);
		fifoOut.push_back(responseByte);
	}
}

void Sio2::Multitap()
{
	fifoOut.push_back(0x00);

	const bool multitapEnabled = (port == 0 && EmuConfig.MultitapPort0_Enabled) || (port == 1 && EmuConfig.MultitapPort1_Enabled);
	SetRecv1(multitapEnabled ? Recv1::CONNECTED : Recv1::DISCONNECTED);

	if (multitapEnabled)
	{
		g_MultitapProtocol.SendToMultitap();
	}
	else 
	{
		while (fifoOut.size() < commandLength)
		{
			fifoOut.push_back(0x00);
		}
	}
}

void Sio2::Infrared()
{
	SetRecv1(Recv1::DISCONNECTED);

	fifoIn.pop_front();
	const u8 responseByte = 0xff;
	
	while (fifoOut.size() < commandLength)
	{
		fifoOut.push_back(responseByte);
	}
}

void Sio2::Memcard()
{
	mcd = &mcds[port][slot];

	/* Check if auto ejection is active. If so, set RECV1 to DISCONNECTED,
	 * and zero out the fifo to simulate dead air over the wire. */
	if (mcd->autoEjectTicks)
	{
		SetRecv1(Recv1::DISCONNECTED);
		fifoOut.push_back(0x00); /* Because Sio2::Write pops the first fifoIn member */

		while (!fifoIn.empty())
		{
			fifoIn.pop_front();
			fifoOut.push_back(0x00);
		}

		mcd->autoEjectTicks--;
		return;
	}

	SetRecv1(FileMcd_IsPresent(mcd->port, mcd->slot) ? Recv1::CONNECTED : Recv1::DISCONNECTED);
	
	const u8 commandByte = fifoIn.front();
	fifoIn.pop_front();
	const u8 responseByte = FileMcd_IsPresent(mcd->port, mcd->slot) ? 0x00 : 0xff;
	fifoOut.push_back(responseByte);
	// Technically, the FLAG byte is only for PS1 memcards. However,
	// since this response byte is still a dud on PS2 memcards, we can
	// basically just cheat and always make this our second response byte for memcards.
	fifoOut.push_back(mcd->FLAG);
	u8 ps1Input = 0;
	u8 ps1Output = 0;

	switch (commandByte)
	{
		case MemcardCommand::PROBE:
			g_MemoryCardProtocol.Probe();
			break;
		case MemcardCommand::UNKNOWN_WRITE_DELETE_END:
			g_MemoryCardProtocol.UnknownWriteDeleteEnd();
			break;
		case MemcardCommand::SET_ERASE_SECTOR:
		case MemcardCommand::SET_WRITE_SECTOR:
		case MemcardCommand::SET_READ_SECTOR:
			g_MemoryCardProtocol.SetSector();
			break;
		case MemcardCommand::GET_SPECS:
			g_MemoryCardProtocol.GetSpecs();
			break;
		case MemcardCommand::SET_TERMINATOR:
			g_MemoryCardProtocol.SetTerminator();
			break;
		case MemcardCommand::GET_TERMINATOR:
			g_MemoryCardProtocol.GetTerminator();
			break;
		case MemcardCommand::WRITE_DATA:
			g_MemoryCardProtocol.WriteData();
			break;
		case MemcardCommand::READ_DATA:
			g_MemoryCardProtocol.ReadData();
			break;
		case MemcardCommand::PS1_READ:
			g_MemoryCardProtocol.ResetPS1State();

			while (!fifoIn.empty())
			{
				ps1Input = fifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1Read(ps1Input);
				fifoIn.pop_front();
				fifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::PS1_STATE:
			g_MemoryCardProtocol.ResetPS1State();

			while (!fifoIn.empty())
			{
				ps1Input = fifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1State(ps1Input);
				fifoIn.pop_front();
				fifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::PS1_WRITE:
			g_MemoryCardProtocol.ResetPS1State();

			while (!fifoIn.empty())
			{
				ps1Input = fifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1Write(ps1Input);
				fifoIn.pop_front();
				fifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::PS1_POCKETSTATION:
			g_MemoryCardProtocol.ResetPS1State();

			while (!fifoIn.empty())
			{
				ps1Input = fifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1Pocketstation(ps1Input);
				fifoIn.pop_front();
				fifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::READ_WRITE_END:
			g_MemoryCardProtocol.ReadWriteEnd();
			break;
		case MemcardCommand::ERASE_BLOCK:
			g_MemoryCardProtocol.EraseBlock();
			break;
		case MemcardCommand::UNKNOWN_BOOT:
			g_MemoryCardProtocol.UnknownBoot();
			break;
		case MemcardCommand::AUTH_XOR:
			g_MemoryCardProtocol.AuthXor();
			break;
		case MemcardCommand::AUTH_F3:
			g_MemoryCardProtocol.AuthF3();
			break;
		case MemcardCommand::AUTH_F7:
			g_MemoryCardProtocol.AuthF7();
			break;
		default:
			break;
	}
}

void Sio2::Write(u8 data)
{
	if (!send3Read)
	{
		/* No more SEND3 positions to access, but the game is still sending us SIO2 writes. Lets ignore them. */
		if (send3Position > 16)
			return;

		const u32 currentSend3 = send3[send3Position];
		port = currentSend3 & Send3::PORT;
		commandLength = (currentSend3 >> 8) & Send3::COMMAND_LENGTH_MASK;
		send3Read = true;

		/* The freshly read SEND3 position had a length of 0, so we are done handling SIO2 commands until
		 * the next SEND3 writes. */
		if (commandLength == 0)
			send3Complete = true;

		/* If the prior command did not need to fully pop fifoIn, do so now, 
		 * so that the next command isn't trying to read the last command's leftovers. */
		while (!fifoIn.empty())
		{
			fifoIn.pop_front();
		}
	}

	if (send3Complete)
	{
		return;
	}
	
	fifoIn.push_back(data);

	// We have received as many command bytes as we expect, and...
	//
	// ... These were from direct writes into IOP memory (DMA block size is zero when direct writes occur)
	// ... These were from SIO2 DMA (DMA block size is non-zero when SIO2 DMA occurs)
	if ((fifoIn.size() == sio2.commandLength && sio2.dmaBlockSize == 0) || fifoIn.size() == sio2.dmaBlockSize)
	{
		// Go ahead and prep so the next write triggers a load of the new SEND3 value.
		sio2.send3Read = false;
		sio2.send3Position++;

		// Check the SIO mode
		const u8 sioMode = fifoIn.front();
		fifoIn.pop_front();

		switch (sioMode)
		{
			case SioMode::PAD:
				this->Pad();
				break;
			case SioMode::MULTITAP:
				this->Multitap();
				break;
			case SioMode::INFRARED:
				this->Infrared();
				break;
			case SioMode::MEMCARD:
				this->Memcard();
				break;
			default:
				fifoOut.push_back(0x00);
				SetRecv1(Recv1::DISCONNECTED);
				break;
		}

		// If command was sent over SIO2 DMA, align fifoOut to the block size
		if (sio2.dmaBlockSize > 0)
		{
			const size_t dmaDiff = fifoOut.size() % sio2.dmaBlockSize;

			if (dmaDiff > 0)
			{
				const size_t padding = sio2.dmaBlockSize - dmaDiff;

				for (size_t i = 0; i < padding; i++)
				{
					fifoOut.push_back(0x00);
				}
			}
		}
	}
}

u8 Sio2::Read()
{
	u8 ret = 0x00;
	
	if (!fifoOut.empty())
	{
		ret = fifoOut.front();
		fifoOut.pop_front();
	}
	
	return ret;
}

void sioSetGameSerial( const std::string& serial )
{
	for ( uint port = 0; port < 2; ++port )
	{
		for ( uint slot = 0; slot < 4; ++slot )
		{
			if ( FileMcd_ReIndex(port, slot, serial) )
				AutoEject::Set( port, slot );
		}
	}
}

bool SaveStateBase::sio2Freeze()
{
	if (!(FreezeTag("sio2")))
		return false;

	Freeze(sio2);
	FreezeDeque(fifoIn);
	FreezeDeque(fifoOut);
	if (!IsOkay())
		return false;

	/* CRCs for memory cards.
	 * If the memory card hasn't changed when loading state, we can safely skip ejecting it. */
	u64 mcdCrcs[SIO::PORTS][SIO::SLOTS];
	if (IsSaving())
	{
		for (u32 port = 0; port < SIO::PORTS; port++)
		{
			for (u32 slot = 0; slot < SIO::SLOTS; slot++)
				mcdCrcs[port][slot] = FileMcd_GetCRC(port, slot);
		}
	}
	Freeze(mcdCrcs);
	if (!IsOkay())
		return false;

	if (IsLoading())
	{
		bool ejected = false;
		for (u32 port = 0; port < SIO::PORTS && !ejected; port++)
		{
			for (u32 slot = 0; slot < SIO::SLOTS; slot++)
			{
				if (mcdCrcs[port][slot] != FileMcd_GetCRC(port, slot))
				{
					AutoEject::SetAll();
					ejected = true;
					break;
				}
			}
		}
	}

	return true;
}

bool SaveStateBase::sioFreeze()
{
	if (!(FreezeTag("sio0")))
		return false;

	Freeze(sio0);

	return IsOkay();
}

std::tuple<u32, u32> sioConvertPadToPortAndSlot(u32 index)
{
	if (index > 4) /* [5,6,7] */
		return std::make_tuple(1, index - 4); /* 2B,2C,2D */
	else if (index > 1) /* [2,3,4] */
		return std::make_tuple(0, index - 1); /* 1B,1C,1D */
	/* [0,1] */
	return std::make_tuple(index, 0); /* 1A,2A */
}

u32 sioConvertPortAndSlotToPad(u32 port, u32 slot)
{
	if (slot == 0)
		return port;
	else if (port == 0) // slot=[0,1]
		return slot + 1; // 2,3,4
	return slot + 4; // 5,6,7
}

bool sioPadIsMultitapSlot(u32 index)
{
	return (index >= 2);
}

bool sioPortAndSlotIsMultitap(u32 port, u32 slot)
{
	return (slot != 0);
}

void AutoEject::Set(size_t port, size_t slot)
{
	if (EmuConfig.McdEnableEjection)
		mcds[port][slot].autoEjectTicks = 60;
}

void AutoEject::Clear(size_t port, size_t slot)
{
	mcds[port][slot].autoEjectTicks = 0;
}

void AutoEject::SetAll()
{
	for (size_t port = 0; port < SIO::PORTS; port++)
	{
		for (size_t slot = 0; slot < SIO::SLOTS; slot++)
			AutoEject::Set(port, slot);
	}
}

void AutoEject::ClearAll()
{
	for (size_t port = 0; port < SIO::PORTS; port++)
	{
		for (size_t slot = 0; slot < SIO::SLOTS; slot++)
			AutoEject::Clear(port, slot);
	}
}
