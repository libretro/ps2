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

#include <ctype.h>
#include <time.h>
#include <exception>
#include <memory>
#include <cstring>

#include "../../common/Console.h"

#include "IsoFS/IsoFS.h"
#include "IsoFS/SectorSource.h"
#include "IsoFileFormats.h"

#include "../Config.h"

CDVD_API* CDVD = NULL;

// ----------------------------------------------------------------------------
// diskTypeCached
// Internal disc type cache, to reduce the overhead of disc type checks, which are
// performed quite liberally by many games (perhaps intended to keep the PS2 DVD
// from spinning down due to idle activity?).
// Cache is set to -1 for init and when the disc is removed/changed, which invokes
// a new DiskTypeCheck.  All subsequent checks use the non-negative value here.
//
static int diskTypeCached = -1;

////////////////////////////////////////////////////////
//
// CDVD null interface for Run BIOS menu
static s32 CALLBACK NODISCopen(const char* pTitle) { return 0; }
static void CALLBACK NODISCclose(void) { }
static s32 CALLBACK NODISCreadTrack(u32 lsn, int mode) { return -1; }
static s32 CALLBACK NODISCgetBuffer(u8* buffer) { return -1; }
static s32 CALLBACK NODISCreadSubQ(u32 lsn, cdvdSubQ* subq) { return -1; }
static s32 CALLBACK NODISCgetTN(cdvdTN* Buffer) { return -1; }
static s32 CALLBACK NODISCgetTD(u8 Track, cdvdTD* Buffer) { return -1; }
static s32 CALLBACK NODISCgetTOC(void* toc) { return -1; }
static s32 CALLBACK NODISCgetDiskType(void) { return CDVD_TYPE_NODISC; }
static s32 CALLBACK NODISCgetTrayStatus(void) { return CDVD_TRAY_CLOSE; }
static s32 CALLBACK NODISCdummyS32(void) { return 0; }
static void CALLBACK NODISCnewDiskCB(void (*)(void)) { }
static s32 CALLBACK NODISCreadSector(u8* tempbuffer, u32 lsn, int mode) { return -1; }
static s32 CALLBACK NODISCgetDualInfo(s32* dualType, u32* _layer1start) { return -1; }

static CDVD_API CDVDapi_NoDisc =
{
	NODISCclose,
	NODISCopen,
	NODISCreadTrack,
	NODISCgetBuffer,
	NODISCreadSubQ,
	NODISCgetTN,
	NODISCgetTD,
	NODISCgetTOC,
	NODISCgetDiskType,
	NODISCgetTrayStatus,
	NODISCdummyS32,
	NODISCdummyS32,

	NODISCnewDiskCB,

	NODISCreadSector,
	NODISCgetDualInfo,
};

//////////////////////////////////////////////////////////////////////////////////////////
// Disk Type detection stuff (from cdvdGigaherz)
//
/* Generous: real ones are well under a kilobyte. */
static constexpr u32 MAX_SYSTEM_CNF_SIZE = 1024 * 1024;

static int CheckDiskTypeFS(int baseType)
{
	IsoDirectory rootdir;
	if (rootdir.OpenRootDirectory())
	{
		if (IsoFile file; file.open(rootdir, "SYSTEM.CNF;1"))
		{
			/* The length is a u32 read straight out of the directory
			 * record, so it is whatever the image says it is - up to
			 * 4 GiB - and it used to go through int on the way here.
			 *
			 * A record declaring 0xFFFFFFFF gave size == -1: the
			 * allocation became make_unique<char[]>(0), the read
			 * returned 0 because it rejects negative lengths, and then
			 * buffer[size] wrote a NUL one byte *before* the block.
			 * strstr() then walked an uninitialised zero-length buffer
			 * looking for "BOOT2".  A record declaring 0x80000000
			 * instead asked new[] for ~2 GiB, which this build cannot
			 * report as a failure - it is compiled -fno-exceptions.
			 *
			 * A real SYSTEM.CNF is a few hundred bytes.  Cap what a
			 * disc can make us allocate, and terminate at the number of
			 * bytes actually read rather than the number claimed. */
			const u32 claimed = file.getLength();
			if (claimed == 0 || claimed > MAX_SYSTEM_CNF_SIZE)
				return CDVD_TYPE_ILLEGAL;

			const std::unique_ptr<char[]> buffer = std::make_unique<char[]>(claimed + 1);
			const s32 got = file.read(buffer.get(), static_cast<s32>(claimed));
			buffer[(got > 0) ? got : 0] = '\0';

			char* pos = strstr(buffer.get(), "BOOT2");
			if (!pos)
			{
				pos = strstr(buffer.get(), "BOOT");
				if (!pos)
					return CDVD_TYPE_ILLEGAL;
				return CDVD_TYPE_PSCD;
			}

			return (baseType == CDVD_TYPE_DETCTCD) ? CDVD_TYPE_PS2CD : CDVD_TYPE_PS2DVD;
		}

		// PS2 Linux disc 2, doesn't have a System.CNF or a normal ELF
		if (rootdir.Exists("P2L_0100.02;1"))
			return CDVD_TYPE_PS2DVD;

		if (rootdir.Exists("PSX.EXE;1"))
			return CDVD_TYPE_PSCD;

		if (rootdir.Exists("VIDEO_TS/VIDEO_TS.IFO;1"))
			return CDVD_TYPE_DVDV;
	}

	return CDVD_TYPE_ILLEGAL; // << Only for discs which aren't ps2 at all.
}

static int FindDiskType(int mType)
{
	int dataTracks = 0;
	int audioTracks = 0;
	int iCDType = mType;
	cdvdTN tn;

	CDVD->getTN(&tn);

	if (tn.strack != tn.etrack) // multitrack == CD.
		iCDType = CDVD_TYPE_DETCTCD;
	else if (mType < 0)
	{
		static u8 bleh[CD_FRAMESIZE_RAW];
		cdvdTD td;

		CDVD->getTD(0, &td);
		if (td.lsn > 452849)
			iCDType = CDVD_TYPE_DETCTDVDS;
		else
		{
			if (DoCDVDreadSector(bleh, 16, CDVD_MODE_2048) == 0)
			{
				//Horrible hack! in CD images position 166 and 171 have block size but not DVD's
				//It's not always 2048 however (can be 4096)
				//Test Impossible Mission if thia is changed.
				u16 bs166_, bs171_;
				memcpy(&bs166_, bleh + 166, sizeof(u16));
				memcpy(&bs171_, bleh + 171, sizeof(u16));
				if (bs166_ == bs171_)
					iCDType = CDVD_TYPE_DETCTCD;
				else
					iCDType = CDVD_TYPE_DETCTDVDS;
			}
		}
	}

	if (iCDType == CDVD_TYPE_DETCTDVDS)
	{
		s32 dlt = 0;
		u32 l1s = 0;

		if (CDVD->getDualInfo(&dlt, &l1s) == 0)
		{
			if (dlt > 0)
				iCDType = CDVD_TYPE_DETCTDVDD;
		}
	}

	switch (iCDType)
	{
		case CDVD_TYPE_DETCTCD:
			Console.WriteLn(" * CDVD Disk Open: CD, %d tracks (%d to %d):", tn.etrack - tn.strack + 1, tn.strack, tn.etrack);
			break;

		case CDVD_TYPE_DETCTDVDS:
			Console.WriteLn(" * CDVD Disk Open: DVD, Single layer or unknown:");
			break;

		case CDVD_TYPE_DETCTDVDD:
			Console.WriteLn(" * CDVD Disk Open: DVD, Double layer:");
			break;
	}

	audioTracks = dataTracks = 0;
	for (int i = tn.strack; i <= tn.etrack; i++)
	{
		cdvdTD td, td2;

		CDVD->getTD(i, &td);

		if (tn.etrack > i)
			CDVD->getTD(i + 1, &td2);
		else
			CDVD->getTD(0, &td2);

		int tlength = td2.lsn - td.lsn;

		if (td.type == CDVD_AUDIO_TRACK)
		{
			audioTracks++;
			Console.WriteLn(" * * Track %d: Audio (%d sectors)", i, tlength);
		}
		else
		{
			dataTracks++;
			Console.WriteLn(" * * Track %d: Data (Mode %d) (%d sectors)", i, ((td.type == CDVD_MODE1_TRACK) ? 1 : 2), tlength);
		}
	}

	if (dataTracks > 0)
		iCDType = CheckDiskTypeFS(iCDType);

	if (audioTracks > 0)
	{
		switch (iCDType)
		{
			case CDVD_TYPE_PS2CD:
				iCDType = CDVD_TYPE_PS2CDDA;
				break;
			case CDVD_TYPE_PSCD:
				iCDType = CDVD_TYPE_PSCDDA;
				break;
			default:
				iCDType = CDVD_TYPE_CDDA;
				break;
		}
	}

	return iCDType;
}

static void DetectDiskType(void)
{
	if (CDVD->getTrayStatus() == CDVD_TRAY_OPEN)
	{
		diskTypeCached = CDVD_TYPE_NODISC;
		return;
	}

	int baseMediaType = CDVD->getDiskType();

	if (baseMediaType == CDVD_TYPE_NODISC)
		diskTypeCached = CDVD_TYPE_NODISC;
	else
		diskTypeCached = FindDiskType(-1);
}

static std::string m_SourceFilename[3];
static CDVD_SourceType m_CurrentSourceType = CDVD_SourceType::NoDisc;

void CDVDsys_SetFile(CDVD_SourceType srctype, std::string newfile)
{
	m_SourceFilename[enum_cast(srctype)] = std::move(newfile);
}

const std::string& CDVDsys_GetFile(CDVD_SourceType srctype)
{
	return m_SourceFilename[enum_cast(srctype)];
}

CDVD_SourceType CDVDsys_GetSourceType(void)
{
	return m_CurrentSourceType;
}

void CDVDsys_ClearFiles()
{
	for (u32 i = 0; i < C89_ARRAY_SIZE(m_SourceFilename); i++)
		m_SourceFilename[i] = {};
}

void CDVDsys_ChangeSource(CDVD_SourceType type)
{
	if (CDVD)
		DoCDVDclose();

	switch (m_CurrentSourceType = type)
	{
		case CDVD_SourceType::Iso:
			CDVD = &CDVDapi_Iso;
			break;
		case CDVD_SourceType::NoDisc:
			CDVD = &CDVDapi_NoDisc;
			break;
		case CDVD_SourceType::Disc:
			/* TODO/FIXME - removed physical CD/DVD code */
		default:
			break;
	}
}

bool DoCDVDopen(void)
{
	CDVD->newDiskCB(cdvdNewDiskCB);

	auto CurrentSourceType = enum_cast(m_CurrentSourceType);
	int ret = CDVD->open(!m_SourceFilename[CurrentSourceType].empty() ? m_SourceFilename[CurrentSourceType].c_str() : nullptr);
	if (ret == -1)
		return false; // error! (handled by caller)

	DoCDVDdetectDiskType();
	return true;
}

void DoCDVDclose(void)
{
	if (CDVD->close)
		CDVD->close();

	DoCDVDresetDiskTypeCache();
}

s32 DoCDVDreadSector(u8* buffer, u32 lsn, int mode)
{
	return CDVD->readSector(buffer, lsn, mode);
}

s32 DoCDVDreadTrack(u32 lsn, int mode)
{
	return CDVD->readTrack(lsn, mode);
}

s32 DoCDVDgetBuffer(u8* buffer)
{
	return CDVD->getBuffer(buffer);
}

s32 DoCDVDdetectDiskType(void)
{
	if (diskTypeCached < 0)
		DetectDiskType();
	return diskTypeCached;
}

void DoCDVDresetDiskTypeCache(void)
{
	diskTypeCached = -1;
}
