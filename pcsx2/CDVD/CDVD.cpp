/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include <cstring> /* memset */

#include "R3000A.h"
#include "Common.h"
#include "IopHw.h"
#include "IopDma.h"

#include <cctype>
#include <ctime>
#include <cstring>
#include <memory>

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Threading.h"

#include "Ps1CD.h"
#include "CDVD.h"
#include "CDVD_internal.h"
#include "IsoFileFormats.h"

#include "GS.h" // for gsVideoMode
#include "Elfheader.h"
#include "ps2/BiosTools.h"
#include "Host.h"
#include "VMManager.h"

/* TODO/FIXME - forward declaration */
extern MemorySettingsInterface s_settings_interface;

// This typically reflects the Sony-assigned serial code for the Disc, if one exists.
//  (examples:  SLUS-2113, etc).
// If the disc is homebrew then it probably won't have a valid serial; in which case
// this string will be empty.
std::string DiscSerial;

cdvdStruct cdvd;

s64 PSXCLK = 36864000;

static constexpr size_t NVRAM_SIZE = 1024;
static u8 s_nvram[NVRAM_SIZE];

static constexpr u32 DEFAULT_MECHA_VERSION = 0x00020603;
static u32 s_mecha_version = 0;

static __fi void SetSCMDResultSize(u8 size)
{
	cdvd.SCMDResultCnt  = size;
	cdvd.SCMDResultPos  = 0;
	cdvd.sDataIn       &= ~0x40;
	memset(&cdvd.SCMDResultBuff[0], 0, size);
}

static void CDVDCancelReadAhead(void)
{
	cdvd.nextSectorsBuffered = 0;
	psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);
}

static void CDVDSECTORREADY_INT(u32 eCycle)
{
	if (psxRegs.interrupt & (1 << IopEvt_CdvdSectorReady))
		return;

	if (EmuConfig.Speedhacks.fastCDVD)
	{
		if (eCycle < Cdvd_FullSeek_Cycles && eCycle > 1)
			eCycle *= 0.5f;
	}

	PSX_INT(IopEvt_CdvdSectorReady, eCycle);
}

static void CDVDREAD_INT(u32 eCycle)
{
	// Give it an arbitary FAST value. Good for ~5000kb/s in ULE when copying a file from CDVD to HDD
	// Keep long seeks out though, as games may try to push dmas while seeking. (Tales of the Abyss)
	if (EmuConfig.Speedhacks.fastCDVD)
	{
		if (eCycle < Cdvd_FullSeek_Cycles && eCycle > 1)
			eCycle *= 0.5f;
	}

	PSX_INT(IopEvt_CdvdRead, eCycle);
}

static void CDVD_INT(int eCycle)
{
	if (eCycle == 0)
		cdvdActionInterrupt();
	else
		PSX_INT(IopEvt_Cdvd, eCycle);
}

// Sets the cdvd IRQ and the reason for the IRQ, and signals the IOP for a branch
// test (which will cause the exception to be handled).
static void cdvdSetIrq(uint id = (1 << Irq_CommandComplete))
{
	cdvd.IntrStat |= id;
	cdvd.AbortRequested = false;
	iopIntcIrq(2);
	psxSetNextBranchDelta(20);
}

static int mg_BIToffset(u8* buffer)
{
	int i, ofs = 0x20;
	for (i = 0; i < *(u16*)&buffer[0x1A]; i++)
		ofs += 0x10;

	if (*(u16*)&buffer[0x18] & 1)
		ofs += buffer[ofs];
	if ((*(u16*)&buffer[0x18] & 0xF000) == 0)
		ofs += 8;

	return ofs + 0x20;
}

static void cdvdGetMechaVer(u8* ver)
{
}

const NVMLayout* getNvmLayout(void)
{
	return (nvmlayouts[1].biosVer <= BiosVersion) ? &nvmlayouts[1] : &nvmlayouts[0];
}

static void cdvdCreateNewNVM(void)
{
	memset(s_nvram, 0, sizeof(s_nvram));

	// Write NVM ILink area with dummy data (Age of Empires 2)
	// Also write language data defaulting to English (Guitar Hero 2)
	// Also write PStwo region defaults

	const NVMLayout* nvmLayout = getNvmLayout();

	if (((BiosVersion >> 8) == 2) && ((BiosVersion & 0xff) != 10)) // bios >= 200, except of 0x210 for PSX2 DESR
		memcpy(&s_nvram[nvmLayout->regparams], PStwoRegionDefaults[BiosRegion], 12);

	static constexpr u8 ILinkID_Data[8] = {0x00, 0xAC, 0xFF, 0xFF, 0xFF, 0xFF, 0xB9, 0x86};
	memcpy(&s_nvram[nvmLayout->ilinkId], ILinkID_Data, sizeof(ILinkID_Data));
	if (nvmlayouts[1].biosVer <= BiosVersion)
	{
		static constexpr u8 ILinkID_checksum[2] = {0x00, 0x18};
		memcpy(&s_nvram[nvmLayout->ilinkId + 0x08], ILinkID_checksum, sizeof(ILinkID_checksum));
	}

	// Config sections first 16 bytes are generally blank expect the last byte which is PS1 mode stuff
	// So let's ignore that and just write the PS2 mode stuff
	memcpy(&s_nvram[nvmLayout->config1 + 0x10], biosLangDefaults[BiosRegion], 16);
}

static std::string cdvdGetNVRAMPath(void)
{
	return Path::ReplaceExtension(BiosPath, "nvm");
}

void cdvdLoadNVRAM(void)
{
	const char *nvmfile = Path::ReplaceExtension(BiosPath, "nvm").c_str();
	RFILE *fp = FileSystem::OpenFile(nvmfile, "rb");
	if (!fp || rfread(s_nvram, sizeof(s_nvram), 1, fp) != 1)
	{
		Console.Warning("Failed to open or read NVRAM: %s", nvmfile);
		cdvdCreateNewNVM();
	}
	else
	{
		// Verify NVRAM is sane.
		const NVMLayout* nvmLayout = getNvmLayout();
		constexpr u8 zero[16] = {0};

		Console.WriteLn("Reading NVRAM file: %s", nvmfile);

		if (memcmp(&s_nvram[nvmLayout->config1 + 0x10], zero, 16) == 0 ||
			(((BiosVersion >> 8) == 2) && ((BiosVersion & 0xff) != 10) &&
				(memcmp(&s_nvram[nvmLayout->regparams], zero, 12) == 0)))
		{
			Console.Warning("Language or Region Parameters missing, filling in defaults");
			cdvdCreateNewNVM();
		}
	}

	const char * mecfile = Path::ReplaceExtension(BiosPath, "mec").c_str();
	fp = FileSystem::OpenFile(mecfile, "rb");
	if (!fp || rfread(&s_mecha_version, sizeof(s_mecha_version), 1, fp) != 1)
	{
		s_mecha_version = DEFAULT_MECHA_VERSION;
		Console.Error("Failed to open or read MEC file at %s, creating default.", mecfile);
		fp = FileSystem::OpenFile(mecfile, "w+b");
		if (!fp || rfwrite(&s_mecha_version, sizeof(s_mecha_version), 1, fp) != 1)
			Console.Error("Failed to write MEC file. Check your BIOS setup/permission settings.");
	}
	filestream_close(fp);
}

void cdvdSaveNVRAM(void)
{
	const char *nvmfile = Path::ReplaceExtension(BiosPath, "nvm").c_str();
	RFILE *fp = FileSystem::OpenFile(nvmfile, "w+b");
	if (!fp)
	{
		Console.Error("Failed to open NVRAM for updating: %s...", nvmfile);
		return;
	}

	u8 existing_nvram[NVRAM_SIZE];
	if (rfread(existing_nvram, sizeof(existing_nvram), 1, fp) == 1 &&
		memcmp(existing_nvram, s_nvram, NVRAM_SIZE) == 0)
	{
		Console.Warning("NVRAM has not changed, not writing to disk.");
		filestream_close(fp);
		return;
	}

	if (FileSystem::FSeek64(fp, 0, SEEK_SET) == 0 &&
		rfwrite(s_nvram, NVRAM_SIZE, 1, fp) == 1)
	{
		Console.WriteLn("NVRAM saved to %s.", nvmfile);
	}
	else
	{
		Console.Error("Failed to save NVRAM to %s", nvmfile);
	}
	filestream_close(fp);
}

static void cdvdReadNVM(u8* dst, int offset, int bytes)
{
	int to_read = bytes;
	if ((offset + bytes) > sizeof(s_nvram))
	{
		Console.Warning("CDVD: Out of bounds NVRAM read: offset={}, bytes={}", offset, bytes);
		to_read = std::max(static_cast<int>(sizeof(s_nvram)) - offset, 0);
		memset(dst + to_read, 0, bytes - to_read);
	}

	if (to_read > 0)
		memcpy(dst, &s_nvram[offset], to_read);
}

static void cdvdWriteNVM(const u8* src, int offset, int bytes)
{
	int to_write = bytes;
	if ((offset + bytes) > sizeof(s_nvram))
	{
		Console.Warning("CDVD: Out of bounds NVRAM write: offset={}, bytes={}", offset, bytes);
		to_write = std::max(static_cast<int>(sizeof(s_nvram)) - offset, 0);
	}

	if (to_write > 0)
		memcpy(&s_nvram[offset], src, to_write);
}

void cdvdReadLanguageParams(u8* config)
{
	cdvdReadNVM(config, getNvmLayout()->config1 + 0xF, 16);
}

static s32 cdvdReadConfig(u8* config)
{
	// make sure its in read mode
	if (cdvd.CReadWrite != 0)
	{
		config[0] = 0x80;
		memset(&config[1], 0x00, 15);
		return 1;
	}
	// check if block index is in bounds
	else if (cdvd.CBlockIndex >= cdvd.CNumBlocks)
		return 1;
	else if (
		   ((cdvd.COffset == 0) && (cdvd.CBlockIndex >= 4))
		|| ((cdvd.COffset == 1) && (cdvd.CBlockIndex >= 2))
		|| ((cdvd.COffset == 2) && (cdvd.CBlockIndex >= 7)))
	{
		memset(config, 0, 16);
		return 0;
	}

	// get config data
	const NVMLayout* nvmLayout = getNvmLayout();
	switch (cdvd.COffset)
	{
		case 0:
			cdvdReadNVM(config, nvmLayout->config0 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		case 2:
			cdvdReadNVM(config, nvmLayout->config2 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		default:
			{
				cdvdReadNVM(config, nvmLayout->config1 + (cdvd.CBlockIndex * 16), 16);
				if (cdvd.CBlockIndex == 1 && (NoOSD || s_settings_interface.GetBoolValue("EmuCore", "EnableFastBoot", false)))
				{
					// HACK: Set the "initialized" flag when fast booting, otherwise some games crash (e.g. Jak 1).
					config[2] |= 0x80;
				}

				cdvd.CBlockIndex++;
			}
			break;
	}
	return 0;
}

static s32 cdvdWriteConfig(const u8* config)
{
	// make sure its in write mode && the block index is in bounds
	if ((cdvd.CReadWrite != 1) || (cdvd.CBlockIndex >= cdvd.CNumBlocks))
		return 1;
	else if (
		((cdvd.COffset == 0) && (cdvd.CBlockIndex >= 4)) ||
		((cdvd.COffset == 1) && (cdvd.CBlockIndex >= 2)) ||
		((cdvd.COffset == 2) && (cdvd.CBlockIndex >= 7)))
		return 0;

	// get config data
	const NVMLayout* nvmLayout = getNvmLayout();
	switch (cdvd.COffset)
	{
		case 0:
			cdvdWriteNVM(config, nvmLayout->config0 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		case 2:
			cdvdWriteNVM(config, nvmLayout->config2 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		default:
			cdvdWriteNVM(config, nvmLayout->config1 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
	}
	return 0;
}

// Sets ElfCRC to the CRC of the game bound to the CDVD source.
static bool cdvdLoadElf(ElfObject *elfo, std::string elfpath, bool isPSXElf)
{
	if (StringUtil::StartsWith(elfpath, "host:"))
	{
		std::string host_filename(elfpath.substr(5));
		if (!elfo->OpenFile(host_filename, isPSXElf))
			return false;
	}
	else
	{
		// Mimic PS2 behavior!
		// Much trial-and-error with changing the ISOFS and BOOT2 contents of an image have shown that
		// the PS2 BIOS performs the peculiar task of *ignoring* the version info from the parsed BOOT2
		// filename *and* the ISOFS, when loading the game's ELF image.  What this means is:
		//
		//   1. a valid PS2 ELF can have any version (ISOFS), and the version need not match the one in SYSTEM.CNF.
		//   2. the version info on the file in the BOOT2 parameter of SYSTEM.CNF can be missing, 10 chars long,
		//      or anything else.  Its all ignored.
		//   3. Games loading their own files do *not* exhibit this behavior; likely due to using newer IOP modules
		//      or lower level filesystem APIs (fortunately that doesn't affect us).
		//
		// FIXME: Properly mimicing this behavior is troublesome since we need to add support for "ignoring"
		// version information when doing file searches.  I'll add this later.  For now, assuming a ;1 should
		// be sufficient (no known games have their ELF binary as anything but version ;1)
		const std::string::size_type semi_pos = elfpath.rfind(';');
		if (semi_pos != std::string::npos && std::string_view(elfpath).substr(semi_pos) != ";1")
		{
			Console.WriteLn(Color_Blue, "(LoadELF) Non-conforming version suffix (%s) detected and replaced.", elfpath.c_str());
			elfpath.erase(semi_pos);
			elfpath += ";1";
		}

		// Fix cdrom:path, the iso reader doesn't like it.
		if (StringUtil::StartsWith(elfpath, "cdrom:") && elfpath[6] != '\\' && elfpath[6] != '/')
			elfpath.insert(6, 1, '\\');

		IsoFSCDVD isofs;
		IsoFile file(isofs);
		if (!file.open(elfpath) || !elfo->OpenIsoFile(elfpath, file, isPSXElf))
			return false;
	}
	return true;
}

static __fi void _reloadElfInfo(std::string elfpath)
{
	// Now's a good time to reload the ELF info...
	if (elfpath == LastELF)
		return;

	ElfObject elfo;
	if (!cdvdLoadElf(&elfo, elfpath, false))
		return;

	elfo.LoadHeaders();
	ElfCRC       = elfo.GetCRC();
	ElfEntry     = elfo.GetHeader().e_entry;
	ElfTextRange = elfo.GetTextRange();
	LastELF      = std::move(elfpath);

	Console.WriteLn(Color_StrongBlue, "ELF (%s) Game CRC = 0x%08X, EntryPoint = 0x%08X", LastELF.c_str(), ElfCRC, ElfEntry);

	// Note: Do not load game database info here.  This code is generic and called from
	// BIOS key encryption as well as eeloadReplaceOSDSYS.  The first is actually still executing
	// BIOS code, and patches and cheats should not be applied yet.  (they are applied when
	// eeGameStarting is invoked, which is when the VM starts executing the actual game ELF
	// binary).
}

static std::string ExecutablePathToSerial(const std::string& path)
{
	// cdrom:\SCES_123.45;1
	std::string::size_type pos = path.rfind('\\');
	std::string serial;
	if (pos != std::string::npos)
		serial = path.substr(pos + 1);
	else
	{
		// cdrom:SCES_123.45;1
		pos = path.rfind(':');
		if (pos != std::string::npos)
			serial = path.substr(pos + 1);
		else
			serial = path;
	}

	// strip off ; or version number
	pos = serial.rfind(';');
	if (pos != std::string::npos)
		serial.erase(pos);

	// check that it matches our expected format.
	// this maintains the old behavior of PCSX2.
	if (!StringUtil::WildcardMatch(serial.c_str(), "????_???.??*") &&
		!StringUtil::WildcardMatch(serial.c_str(), "????""-???.??*")) // double quote because trigraphs
		serial.clear();

	// SCES_123.45 -> SCES-12345
	for (std::string::size_type pos = 0; pos < serial.size();)
	{
		if (serial[pos] == '.')
		{
			serial.erase(pos, 1);
			continue;
		}

		if (serial[pos] == '_')
			serial[pos] = '-';
		else
			serial[pos] = static_cast<char>(std::toupper(serial[pos]));

		pos++;
	}

	return serial;
}

void cdvdReloadElfInfo(std::string elfoverride)
{
	std::string elfpath;
	const u32 disc_type = GetPS2ElfName(elfpath);
	DiscSerial   = ExecutablePathToSerial(elfpath);

	// Use the serial from the disc (if any), and the ELF CRC of the override.
	if (!elfoverride.empty())
	{
		_reloadElfInfo(std::move(elfoverride));
		return;
	}

	// PCSX2 currently only recognizes *.elf executables in proper PS2 format.
	// To support different PSX titles in the console title and for savestates, this code bypasses all the detection,
	// simply using the exe name, stripped of problematic characters.
	if (disc_type == 1)
		return;

	// Isn't a disc we recognize?
	if (disc_type == 0)
		return;

	// Recognized and PS2 (BOOT2).  Good job, user.
	_reloadElfInfo(std::move(elfpath));
}

static void cdvdReadKey(u8, u16, u32 arg2, u8* key)
{
	s32 numbers = 0, letters = 0;
	u32 key_0_3;
	u8 key_4, key_14;

	cdvdReloadElfInfo();

	// clear key values
	memset(key, 0, 16);

	if (!DiscSerial.empty())
	{
		// convert the number characters to a real 32 bit number
		numbers = StringUtil::FromChars<s32>(std::string_view(DiscSerial).substr(5, 5)).value_or(0);

		// combine the lower 7 bits of each char
		// to make the 4 letters fit into a single u32
		letters =         (s32)((DiscSerial[3] & 0x7F) << 0)  |
				  (s32)((DiscSerial[2] & 0x7F) << 7)  |
				  (s32)((DiscSerial[1] & 0x7F) << 14) |
				  (s32)((DiscSerial[0] & 0x7F) << 21);
	}

	// calculate magic numbers
	key_0_3 = ((numbers & 0x1FC00) >> 10) | ((0x01FFFFFF & letters) << 7); // numbers = 7F  letters = FFFFFF80
	key_4   = ((numbers & 0x0001F) << 3)  | ((0x0E000000 & letters) >> 25);   // numbers = F8  letters = 07
	key_14  = ((numbers & 0x003E0) >> 2)  | 0x04;                            // numbers = F8  extra   = 04  unused = 03

	// store key values
	key[0] = (key_0_3 & 0x000000FF) >> 0;
	key[1] = (key_0_3 & 0x0000FF00) >> 8;
	key[2] = (key_0_3 & 0x00FF0000) >> 16;
	key[3] = (key_0_3 & 0xFF000000) >> 24;
	key[4] = key_4;

	switch (arg2)
	{
		case 75:
			key[14] = key_14;
			key[15] = 0x05;
			break;

			//      case 3075:
			//          key[15] = 0x01;
			//          break;

		case 4246:
			// 0x0001F2F707 = sector 0x0001F2F7  dec 0x07
			key[0] = 0x07;
			key[1] = 0xF7;
			key[2] = 0xF2;
			key[3] = 0x01;
			key[4] = 0x00;
			key[15] = 0x01;
			break;

		default:
			key[15] = 0x01;
			break;
	}
}

static s32 cdvdReadSubQ(s32 lsn, cdvdSubQ* subq)
{
	s32 ret = CDVD->readSubQ(lsn, subq);
	if (ret != -1)
		return ret;
	return 0x80;
}

static void cdvdDetectDisk(void)
{
	cdvd.DiscType = DoCDVDdetectDiskType();

	if (cdvd.DiscType != 0)
	{
		cdvdTD td;
		CDVD->getTD(0, &td);
		cdvd.MaxSector = td.lsn;
	}
}

#define cdvdUpdateStatus(NewStatus) \
{ \
	cdvd.Status        = NewStatus; \
	cdvd.StatusSticky |= NewStatus; \
}

// We don't really use the MECHA bit but Cold Fear will kick back to the BIOS if it's not set
#define cdvdUpdateReady(NewReadyStatus) cdvd.Ready = ((NewReadyStatus) | (CDVD_DRIVE_MECHA_INIT | CDVD_DRIVE_DEV9CON))

s32 cdvdCtrlTrayOpen(void)
{
	if (cdvd.Status & CDVD_STATUS_TRAY_OPEN)
		return 0x80;

	// If we switch using a source change we need to pretend it's a new disc
	if (CDVDsys_GetSourceType() == CDVD_SourceType::Disc)
	{
		cdvdNewDiskCB();
	}
	else
	{
		cdvdDetectDisk();
		cdvdUpdateStatus(CDVD_STATUS_TRAY_OPEN);
		cdvdUpdateReady(0);
		cdvd.Spinning = false;
		cdvdSetIrq(1 << Irq_Eject);
	}

	return 0; // needs to be 0 for success according to homebrew test "CDVD"
}

s32 cdvdCtrlTrayClose(void)
{
	if (!(cdvd.Status & CDVD_STATUS_TRAY_OPEN))
		return 0x80;

	if (!g_GameStarted && g_SkipBiosHack)
	{
		cdvdUpdateReady(CDVD_DRIVE_READY);
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		cdvd.Spinning               = true;
		cdvd.Tray.trayState         = CDVD_DISC_ENGAGED;
		cdvd.Tray.cdvdActionSeconds = 0;
	}
	else
	{
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvdUpdateStatus(CDVD_STATUS_STOP);
		cdvd.Spinning               = false;
		cdvd.Tray.trayState         = CDVD_DISC_DETECTING;
		cdvd.Tray.cdvdActionSeconds = 3;
	}
	cdvdDetectDisk();

	return 0; // needs to be 0 for success according to homebrew test "CDVD"
}

static bool cdvdIsDVD(void)
{
	if (               cdvd.DiscType == CDVD_TYPE_DETCTDVDS 
			|| cdvd.DiscType == CDVD_TYPE_DETCTDVDD 
			|| cdvd.DiscType == CDVD_TYPE_PS2DVD 
			|| cdvd.DiscType == CDVD_TYPE_DVDV)
		return true;
	return false;
}

static int cdvdTrayStateDetecting(void)
{
	if (cdvd.Tray.trayState != CDVD_DISC_DETECTING)
	{
		if (cdvdIsDVD())
		{
			u32 layer1Start = 0;
			s32 dualType    = 0;
			CDVD->getDualInfo(&dualType, &layer1Start);

			if (dualType > 0)
				return CDVD_TYPE_DETCTDVDD;
			return CDVD_TYPE_DETCTDVDS;
		}

		if (cdvd.DiscType != CDVD_TYPE_NODISC)
			return CDVD_TYPE_DETCTCD;
	}
	return CDVD_TYPE_DETCT; //Detecting any kind of disc existing
}

static u32 cdvdRotationTime(CDVD_MODE_TYPE mode)
{
	float msPerRotation;
	// CAV rotation is constant (minimum speed to maintain exact speed on outer dge
	if (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV)
	{
		// Calculate rotations per second from RPM
		const float rotationPerSecond = static_cast<float>(((mode == MODE_CDROM) ? CD_MAX_ROTATION_X1 : DVD_MAX_ROTATION_X1) * cdvd.Speed) / 60.0f;
		// Calculate MS per rotation by dividing 1 second of milliseconds by the number of rotations.
		msPerRotation = 1000.0f / rotationPerSecond;
		// Calculate how many cycles 1 millisecond takes in IOP clocks, multiply by the time for 1 rotation.
	}
	else
	{
		u32 layer1Start = 0;
		s32 dualType    = 0;
		int numSectors  = 360000; // Pretty much every CD format
		int offset      = 0;

		//CLV adjusts its speed based on where it is on the disc, so we can take the max RPM and use the sector to work it out
		// Sector counts are taken from google for Single layer, Dual layer DVD's and for 700MB CD's
		switch (cdvd.DiscType)
		{
			case CDVD_TYPE_DETCTDVDS:
			case CDVD_TYPE_PS2DVD:
			case CDVD_TYPE_DETCTDVDD:
				numSectors      = 2298496;
				// Layer 1 needs an offset as it goes back to the middle of the disc
				CDVD->getDualInfo(&dualType, &layer1Start);
				if (cdvd.SeekToSector >= layer1Start)
					offset = layer1Start;
				break;
			default: // Pretty much every CD format
				break;
		}
		// CLV speeds are reversed, so the centre is the fastest position.
		const float sectorSpeed = (1.0f - (((float)(cdvd.SeekToSector - offset) / numSectors) * 0.60f)) + 0.40f;
		const float rotationPerSecond = static_cast<float>(((mode == MODE_CDROM) ? CD_MAX_ROTATION_X1 : DVD_MAX_ROTATION_X1) * std::min(static_cast<float>(cdvd.Speed), (mode == MODE_CDROM) ? 10.3f : 1.6f) * sectorSpeed) / 60.0f;
		msPerRotation = 1000.0f / rotationPerSecond;
	}
	return ((PSXCLK / 1000) * msPerRotation);
}

static uint cdvdBlockReadTime(CDVD_MODE_TYPE mode)
{
	float cycles;
	// CAV Read speed is roughly 41% in the centre full speed on outer edge. I imagine it's more logarithmic than this
	if (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV)
	{
		int numSectors = 360000; // Pretty much every CD format
		int offset = 0;

		// Sector counts are taken from google for Single layer, Dual layer DVD's and for 700MB CD's
		if (       (cdvd.DiscType == CDVD_TYPE_DETCTDVDS)
			|| (cdvd.DiscType == CDVD_TYPE_PS2DVD)
			|| (cdvd.DiscType == CDVD_TYPE_DETCTDVDD))
		{
			numSectors       = 2298496;
			u32 layer1Start  = 0;
			s32 dualType     = 0;
			// Layer 1 needs an offset as it goes back to the middle of the disc
			CDVD->getDualInfo(&dualType, &layer1Start);
			if (cdvd.SeekToSector >= layer1Start)
				offset = layer1Start;
		}

		// 0.40f is the "base" inner track speed.
		const float sectorSpeed = ((static_cast<float>(cdvd.SeekToSector - offset) / static_cast<float>(numSectors)) * 0.60f) + 0.40f;
		cycles = static_cast<float>(PSXCLK) / (static_cast<float>(((mode == MODE_CDROM) ? CD_SECTORS_PERSECOND : DVD_SECTORS_PERSECOND) * cdvd.Speed) * sectorSpeed);
	}
	else
	{
		// CLV Read Speed is constant
		cycles = static_cast<float>(PSXCLK) / static_cast<float>(((mode == MODE_CDROM) ? CD_SECTORS_PERSECOND : DVD_SECTORS_PERSECOND) * std::min(static_cast<float>(cdvd.Speed), (mode == MODE_CDROM) ? 10.3f : 1.6f));
	}

	return static_cast<int>(cycles);
}

void cdvdReset(void)
{
	memset(&cdvd, 0, sizeof(cdvd));

	cdvd.DiscType = CDVD_TYPE_NODISC;
	cdvd.Spinning = false;

	cdvd.sDataIn = 0x40;
	cdvdUpdateReady(CDVD_DRIVE_READY);
	cdvdUpdateStatus(CDVD_STATUS_TRAY_OPEN);
	cdvd.Speed = 4;
	cdvd.BlockSize = 2064;
	cdvd.Action = cdvdAction_None;
	cdvd.ReadTime = cdvdBlockReadTime(MODE_DVDROM);
	cdvd.RotSpeed = cdvdRotationTime(MODE_DVDROM);

	{
		// CDVD internally uses GMT+9.  If you think the time's wrong, you're wrong.
		// Set up your time zone and winter/summer in the BIOS.  No PS2 BIOS I know of features automatic DST.
		const std::time_t utc_time = std::time(nullptr);
		const std::time_t gmt9_time = (utc_time + (60 * 60 * 9));
		struct tm curtime = {};
#ifdef _MSC_VER
		gmtime_s(&curtime, &gmt9_time);
#else
		gmtime_r(&gmt9_time, &curtime);
#endif
		cdvd.RTC.second = (u8)curtime.tm_sec;
		cdvd.RTC.minute = (u8)curtime.tm_min;
		cdvd.RTC.hour = (u8)curtime.tm_hour;
		cdvd.RTC.day = (u8)curtime.tm_mday;
		cdvd.RTC.month = (u8)curtime.tm_mon + 1; // WX returns Jan as "0"
		cdvd.RTC.year = (u8)(curtime.tm_year - 100); // offset from 2000
	}

	g_GameStarted = false;
	g_GameLoading = false;
	g_SkipBiosHack = EmuConfig.UseBOOT2Injection;

	cdvdCtrlTrayClose();
}

bool SaveStateBase::cdvdFreeze()
{
	if (!(FreezeTag("cdvd")))
		return false;

	Freeze(cdvd);
	if (!IsOkay())
		return false;

	if (IsLoading())
	{
		// Make sure the Cdvd source has the expected track loaded into the buffer.
		// If cdvd.SeekCompleted is cleared it means we need to load the SeekToSector (ie, a
		// seek is in progress!)

		if (cdvd.Reading)
			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekCompleted ? cdvd.CurrentSector : cdvd.SeekToSector, cdvd.ReadMode);
	}

	return true;
}

void cdvdNewDiskCB(void)
{
	DoCDVDresetDiskTypeCache();
	cdvdDetectDisk();

	// If not ejected but we've swapped source pretend it got ejected
	if ((g_GameStarted || !g_SkipBiosHack) && cdvd.Tray.trayState != CDVD_DISC_EJECT)
	{
		cdvdUpdateStatus(CDVD_STATUS_TRAY_OPEN);
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvd.Tray.trayState = CDVD_DISC_EJECT;
		cdvd.Spinning = false;
		cdvdSetIrq(1 << Irq_Eject);
		// If it really got ejected, the DVD Reader will report Type 0, so no need to simulate ejection
		if (cdvd.DiscType > 0)
			cdvd.Tray.cdvdActionSeconds = 3;
	}
	else if (cdvd.DiscType > 0)
	{
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvdUpdateStatus(CDVD_STATUS_SEEK);
		cdvd.Spinning = true;
		cdvd.Tray.trayState = CDVD_DISC_DETECTING;
		cdvd.Tray.cdvdActionSeconds = 3;
	}
}

static void mechaDecryptBytes(u32 madr, int size)
{
	int shiftAmount = (cdvd.decSet >> 4) & 7;
	int doXor = (cdvd.decSet) & 1;
	int doShift = (cdvd.decSet) & 2;

	u8* curval = iopPhysMem(madr);
	for (int i = 0; i < size; ++i, ++curval)
	{
		if (doXor)
			*curval ^= cdvd.Key[4];
		if (doShift)
			*curval = (*curval >> shiftAmount) | (*curval << (8 - shiftAmount));
	}
}

int cdvdReadSector(void)
{
	s32 bcr = (HW_DMA3_BCR_H16 * HW_DMA3_BCR_L16) * 4;
	if (bcr < cdvd.BlockSize || !(HW_DMA3_CHCR & 0x01000000))
	{
		if (HW_DMA3_CHCR & 0x01000000)
		{
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
		}
		return -1;
	}

	// DMAs use physical addresses (air)
	u8* mdest = iopPhysMem(HW_DMA3_MADR);

	// if raw dvd sector 'fill in the blanks'
	if (cdvd.BlockSize == 2064)
	{
		// get info on dvd type and layer1 start
		s32 layerNum    = 0;
		u32 layer1Start = 0;
		s32 dualType    = 0;
		u32 lsn         = cdvd.CurrentSector;

		CDVD->getDualInfo(&dualType, &layer1Start);

		if ((dualType == 1) && (lsn >= layer1Start))
		{
			// dual layer ptp disc
			layerNum = 1;
			lsn = lsn - layer1Start + 0x30000;
		}
		else if ((dualType == 2) && (lsn >= layer1Start))
		{
			// dual layer otp disc
			layerNum = 1;
			lsn = ~(layer1Start + 0x30000 - 1);
		}
		else
		{
			// Assuming the other dualType is 0,
			// single layer disc, or on first layer of dual layer disc.
			layerNum = 0;
			lsn += 0x30000;
		}

		mdest[0] = 0x20 | layerNum;
		mdest[1] = (u8)(lsn >> 16);
		mdest[2] = (u8)(lsn >> 8);
		mdest[3] = (u8)(lsn);

		// sector IED (not calculated at present)
		mdest[4] = 0;
		mdest[5] = 0;

		// sector CPR_MAI (not calculated at present)
		mdest[6] = 0;
		mdest[7] = 0;
		mdest[8] = 0;
		mdest[9] = 0;
		mdest[10] = 0;
		mdest[11] = 0;

		// normal 2048 bytes of sector data
		memcpy(&mdest[12], cdr.Transfer, 2048);

		// 4 bytes of edc (not calculated at present)
		mdest[2060] = 0;
		mdest[2061] = 0;
		mdest[2062] = 0;
		mdest[2063] = 0;
	}
	else
		memcpy(mdest, cdr.Transfer, cdvd.BlockSize);

	// decrypt sector's bytes
	if (cdvd.decSet)
		mechaDecryptBytes(HW_DMA3_MADR, cdvd.BlockSize);

	// Added a clear after memory write .. never seemed to be necessary before but *should*
	// be more correct. (air)
	psxCpu->Clear(HW_DMA3_MADR, cdvd.BlockSize / 4);

	HW_DMA3_BCR_H16 -= (cdvd.BlockSize / (HW_DMA3_BCR_L16 * 4));
	HW_DMA3_MADR += cdvd.BlockSize;

	if (!HW_DMA3_BCR_H16)
	{
		if (HW_DMA3_CHCR & 0x01000000)
		{
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
		}
	}

	return 0;
}

// inlined due to being referenced in only one place.
__fi void cdvdActionInterrupt(void)
{
	u8 ready_status = CDVD_DRIVE_READY;
	if (cdvd.AbortRequested)
	{
		cdvd.Error = 0x1; // Abort Error
		ready_status |= CDVD_DRIVE_ERROR;
		cdvdUpdateReady(ready_status);
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		cdvd.WaitingDMA = false;
		CDVDCancelReadAhead();
		psxRegs.interrupt &= ~(1 << IopEvt_Cdvd); // Stop any current reads
	}

	switch (cdvd.Action)
	{
		case cdvdAction_Seek:
			cdvd.Spinning = true;
			cdvdUpdateReady(ready_status);
			cdvd.CurrentSector = cdvd.SeekToSector;
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case cdvdAction_Standby:
			cdvd.Spinning = true; //check (rama)
			cdvdUpdateReady(ready_status);
			cdvd.CurrentSector = cdvd.SeekToSector;
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case cdvdAction_Stop:
			cdvd.Spinning = false;
			cdvdUpdateReady(ready_status);
			cdvd.CurrentSector = 0;
			cdvdUpdateStatus(CDVD_STATUS_STOP);
			break;

		case cdvdAction_Error:
			cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			break;
	}
	
	cdvd.Action = cdvdAction_None;
	cdvdSetIrq();
}

__fi void cdvdSectorReady(void)
{
	if (cdvd.nextSectorsBuffered < 16)
		cdvd.nextSectorsBuffered++;

	if (cdvd.nextSectorsBuffered < 16)
		CDVDSECTORREADY_INT(cdvd.ReadTime);
	else if (!cdvd.Reading)
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
}

// inlined due to being referenced in only one place.
__fi void cdvdReadInterrupt(void)
{
	cdvdUpdateReady(CDVD_DRIVE_BUSY);
	cdvdUpdateStatus(CDVD_STATUS_READ);
	cdvd.WaitingDMA = false;

	if (!cdvd.SeekCompleted)
	{
		// Seeking finished.  Process the track we requested before, and
		// then schedule another CDVD read int for when the block read finishes.

		// NOTE: The first CD track was read when the seek was initiated, so no need
		// to call CDVDReadTrack here.

		cdvd.Spinning        = true;
		cdvd.CurrentRetryCnt = 0;
		cdvd.Reading         = 1;
		cdvd.SeekCompleted   = 1;
		cdvd.CurrentSector   = cdvd.SeekToSector;
	}

	if (cdvd.AbortRequested)
	{
		// Code in the CDVD controller suggest there is an alignment thing with DVD's but this seems to just break stuff (Auto Modellista).
		// Needs more investigation
		//if (!cdvdIsDVD() || !(cdvd.CurrentSector & 0xF))
		{
			Console.Warning("Read Abort");
			cdvd.Error = 0x1; // Abort Error
			cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.WaitingDMA = false;
			CDVDCancelReadAhead();
			cdvdSetIrq();
			return;
		}
	}

	if (cdvd.CurrentSector >= cdvd.MaxSector)
	{
		cdvd.Error = 0x32; // Outermost track reached during playback
		cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		cdvd.WaitingDMA = false;
		cdvdSetIrq();
		return;
	}

	if (cdvd.Reading)
	{
		if (cdvd.ReadErr == 0)
		{
			while ((cdvd.ReadErr = DoCDVDgetBuffer(cdr.Transfer)), cdvd.ReadErr == -2) { }
		}

		if (cdvd.ReadErr == -1)
		{
			cdvd.CurrentRetryCnt++;

			if (cdvd.CurrentRetryCnt <= cdvd.RetryCntMax)
			{
				cdvd.ReadErr = DoCDVDreadTrack(cdvd.CurrentSector, cdvd.ReadMode);
				CDVDREAD_INT(cdvd.ReadTime);
			}

			return;
		}

		cdvd.Reading = false;
	}

	if (cdvd.SectorCnt > 0 && cdvd.nextSectorsBuffered)
	{
		if (cdvdReadSector() == -1)
		{
			// This means that the BCR/DMA hasn't finished yet, and rather than fire off the
			// sector-finished notice too early (which might overwrite game data) we delay a
			// bit and try to read the sector again later.
			// An arbitrary delay of some number of cycles probably makes more sense here,
			// but for now it's based on the cdvd.ReadTime value. -- air
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.WaitingDMA = true;
			return;
		}

		cdvd.nextSectorsBuffered--;
		CDVDSECTORREADY_INT(cdvd.ReadTime);

		cdvd.CurrentSector++;
		cdvd.SeekToSector++;

		if (--cdvd.SectorCnt <= 0)
		{
			// Setting the data ready flag fixes a black screen loading issue in
			// Street Fighter EX3 (NTSC-J version).
			cdvdSetIrq();
			cdvdUpdateReady(CDVD_DRIVE_READY);

			cdvd.Reading = 0;
			if (cdvd.nextSectorsBuffered < 16)
			{
				cdvdUpdateStatus(CDVD_STATUS_READ);
			}
			else
			{
				cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			}
			// Timing issues on command end
			// Star Ocean (1.1 Japan) expects the DMA to end and interrupt at least 128 or more cycles before the CDVD command ends.
			// However the time required seems to increase slowly, so delaying the end of the command is not the solution.
			//cdvd.Status = CDVD_STATUS_PAUSE; // Needed here but could be smth else than Pause (rama)
			// All done! :D
			return;
		}
	}
	else
	{
		if (cdvd.SectorCnt <= 0)
		{
			cdvdSetIrq();

			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			return;
		}
		if (cdvd.nextSectorsBuffered)
			CDVDREAD_INT((cdvd.BlockSize / 4) * 12);
		else
			CDVDREAD_INT(psxRemainingCycles(IopEvt_CdvdSectorReady) + ((cdvd.BlockSize / 4) * 12));
		return;
	}

	cdvd.CurrentRetryCnt = 0;
	cdvd.Reading = 1;
	cdvd.ReadErr = DoCDVDreadTrack(cdvd.CurrentSector, cdvd.ReadMode);
	if (cdvd.nextSectorsBuffered)
		CDVDREAD_INT((cdvd.BlockSize / 4) * 12);
	else
		CDVDREAD_INT(psxRemainingCycles(IopEvt_CdvdSectorReady) + ((cdvd.BlockSize / 4) * 12));
}

// Returns the number of IOP cycles until the event completes.
static uint cdvdStartSeek(uint newsector, CDVD_MODE_TYPE mode, bool transition_to_CLV)
{
	cdvd.SeekToSector = newsector;

	uint delta = abs(static_cast<s32>(cdvd.SeekToSector - cdvd.CurrentSector));
	uint seektime = 0;
	bool isSeeking = false;

	cdvdUpdateReady(CDVD_DRIVE_BUSY);
	cdvd.Reading = 1;
	cdvd.SeekCompleted = 0;
	// Okay so let's explain this, since people keep messing with it in the past and just poking it.
	// So when the drive is spinning, bit 0x2 is set on the Status, and bit 0x8 is set when the drive is not reading.
	// So In the case where it's seeking to data it will be Spinning (0x2) not reading (0x8) and Seeking (0x10, but because seeking is also spinning 0x2 is also set))
	// Update - Apparently all that was rubbish and some games don't like it. WRC was the one in this scenario which hated SEEK |ZPAUSE, so just putting it back to pause for now.
	// We should really run some tests for this behaviour.
	int drive_speed_change_cycles = 0;
	const int old_rotspeed = cdvd.RotSpeed;
	cdvd.RotSpeed = cdvdRotationTime(mode);

	cdvd.ReadTime = cdvdBlockReadTime(mode);

	if (cdvd.Spinning && transition_to_CLV)
	{
		const float psx_clk_cycles = static_cast<float>(PSXCLK);
		const float old_rpm = (psx_clk_cycles / static_cast<float>(old_rotspeed)) * 60.0f;
		const float new_rpm = (psx_clk_cycles / static_cast<float>(cdvd.RotSpeed)) * 60.0f;
		// A rough cycles per RPM change based on 333ms for a full spin up.
		drive_speed_change_cycles = (psx_clk_cycles / 1000.0f) * (0.054950495049505f * std::abs(new_rpm - old_rpm));
		CDVDCancelReadAhead();
	}
	cdvdUpdateStatus(CDVD_STATUS_SEEK);

	if (!cdvd.Spinning)
	{
		seektime = PSXCLK / 3; // 333ms delay
		cdvd.Spinning = true;
		cdvd.nextSectorsBuffered = 0;
		CDVDSECTORREADY_INT(seektime + cdvd.ReadTime);
	}
	else if ((tbl_ContigiousSeekDelta[mode] == 0) || (delta >= tbl_ContigiousSeekDelta[mode]))
	{
		// Select either Full or Fast seek depending on delta:
		CDVDCancelReadAhead();

		if (delta >= tbl_FastSeekDelta[mode]) // Full Seek
			seektime = Cdvd_FullSeek_Cycles;
		else
			seektime = Cdvd_FastSeek_Cycles;
		isSeeking = true;
	}
	else if(!drive_speed_change_cycles)
	{
		// if delta > 0 it will read a new sector so the readInterrupt will account for this.
		
		isSeeking = false;

		if (cdvd.Action != cdvdAction_Seek)
		{
			if (delta == 0)
			{
				//cdvd.Status = CDVD_STATUS_PAUSE;
				cdvdUpdateStatus(CDVD_STATUS_READ);
				cdvd.SeekCompleted = 1; // Note: 1, not 0, as implied by the next comment. Need to look into this. --arcum42
				cdvd.Reading = 1; // We don't need to wait for it to read a sector as it's already queued up, or we adjust for it here.
				cdvd.CurrentRetryCnt = 0;

				// setting SeekCompleted to 0 skips the seek logic, which means the next call to
				// cdvdReadInterrupt will load a block.  So make sure it's properly scheduled
				// based on sector read speeds:

				if (!cdvd.nextSectorsBuffered)//Buffering time hasn't completed yet so cancel it and simulate the remaining time
				{
					if (psxRegs.interrupt & (1 << IopEvt_CdvdSectorReady))
						seektime = psxRemainingCycles(IopEvt_CdvdSectorReady) + ((cdvd.BlockSize / 4) * 12);
					else
						delta = 1; // Forces it to use the rotational delay since we have no sectors buffered and it isn't buffering any.
				}
				else
					return (cdvd.BlockSize / 4) * 12;
			}
			else
			{
				if (delta >= cdvd.nextSectorsBuffered)
				{
					CDVDCancelReadAhead();
				}
				else
					cdvd.nextSectorsBuffered -= delta;
			}
		}
	}

	seektime += drive_speed_change_cycles;

	// Only do this on reads, the seek kind of accounts for this and then it reads the sectors after
	if ((delta || cdvd.Action == cdvdAction_Seek) && !isSeeking && !cdvd.nextSectorsBuffered)
	{
		const u32 rotationalLatency = cdvdRotationTime(static_cast<CDVD_MODE_TYPE>(cdvdIsDVD())) / 2; // Half it to average the rotational latency.
		if (cdvd.Action == cdvdAction_Seek)
		{
			seektime += rotationalLatency;
			CDVDCancelReadAhead();
		}
		else
		{
			seektime += rotationalLatency + cdvd.ReadTime;
			CDVDSECTORREADY_INT(seektime);
			seektime += (cdvd.BlockSize / 4) * 12;
		}
	}
	else if (!isSeeking) // Not seeking but we have buffered stuff, need to just account for DMA time (and kick the read DMA if it's not running for some reason.
	{
		if (!(psxRegs.interrupt & (1 << IopEvt_CdvdSectorReady)))
		{
			seektime += cdvd.ReadTime;
			CDVDSECTORREADY_INT(seektime);
		}
		seektime += (cdvd.BlockSize / 4) * 12;
	}
	else // We're seeking, so kick off the buffering after the seek finishes.
	{
		CDVDSECTORREADY_INT(seektime);
	}

	return seektime;
}

void cdvdUpdateTrayState(void)
{
	if (cdvd.Tray.cdvdActionSeconds > 0)
	{
		if (--cdvd.Tray.cdvdActionSeconds == 0)
		{
			switch (cdvd.Tray.trayState)
			{
				case CDVD_DISC_OPEN:
					cdvdCtrlTrayOpen();
					if (cdvd.DiscType > 0 || CDVDsys_GetSourceType() == CDVD_SourceType::NoDisc)
					{
						cdvd.Tray.cdvdActionSeconds = 3;
						cdvd.Tray.trayState = CDVD_DISC_EJECT;
					}

				break;
				case CDVD_DISC_EJECT:
					cdvdCtrlTrayClose();
					break;
				case CDVD_DISC_DETECTING:
					cdvd.Tray.trayState = CDVD_DISC_SEEKING;
					cdvdUpdateStatus(CDVD_STATUS_SEEK);
					cdvd.Tray.cdvdActionSeconds = 2;
					// If we're swapping disc, reload the elf, patches etc to reflect the new disc.
					if (g_GameStarted)
					{
						cdvdReloadElfInfo();
						VMManager::Internal::GameStartingOnCPUThread();
					}
					break;
				case CDVD_DISC_SEEKING:
					cdvd.Spinning = true;
					/* fallthrough */
				case CDVD_DISC_ENGAGED:
					cdvd.Tray.trayState = CDVD_DISC_ENGAGED;
					cdvdUpdateReady(CDVD_DRIVE_READY);
					cdvdUpdateStatus(CDVD_STATUS_PAUSE);
					break;
			}
		}
	}
}

void cdvdVsync(void)
{
	static u8 monthmap[13] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	cdvd.RTCcount++;
	if (cdvd.RTCcount < GetVerticalFrequency())
		return;
	cdvd.RTCcount = 0;

	cdvdUpdateTrayState();

	cdvd.RTC.second++;
	if (cdvd.RTC.second < 60)
		return;
	cdvd.RTC.second = 0;

	cdvd.RTC.minute++;
	if (cdvd.RTC.minute < 60)
		return;
	cdvd.RTC.minute = 0;

	cdvd.RTC.hour++;
	if (cdvd.RTC.hour < 24)
		return;
	cdvd.RTC.hour = 0;

	cdvd.RTC.day++;
	if (cdvd.RTC.day <= (cdvd.RTC.month == 2 && cdvd.RTC.year % 4 == 0 ? 29 : monthmap[cdvd.RTC.month - 1]))
		return;
	cdvd.RTC.day = 1;

	cdvd.RTC.month++;
	if (cdvd.RTC.month <= 12)
		return;
	cdvd.RTC.month = 1;

	cdvd.RTC.year++;
	if (cdvd.RTC.year < 100)
		return;
	cdvd.RTC.year = 0;
}

static __fi u8 cdvdRead18(void) // SDATAOUT
{
	u8 ret = 0;

	if (((cdvd.sDataIn & 0x40) == 0) && (cdvd.SCMDResultPos < cdvd.SCMDResultCnt))
	{
		cdvd.SCMDResultPos++;
		if (cdvd.SCMDResultPos >= cdvd.SCMDResultCnt)
			cdvd.sDataIn |= 0x40;
		ret = cdvd.SCMDResultBuff[cdvd.SCMDResultPos - 1];
	}

	return ret;
}

u8 cdvdRead(u8 key)
{
	switch (key)
	{
		case 0x04: // NCOMMAND
			return cdvd.nCommand;

		case 0x05: // N-READY
			return cdvd.Ready;

		case 0x06: // ERROR
		{
			u8 ret = cdvd.Error;
			cdvd.Error = 0;
			return ret;
		}
		case 0x07: // BREAK
			return 0;

		case 0x08: // INTR_STAT
			return cdvd.IntrStat;

		case 0x0A: // STATUS
			return cdvd.Status;

		case 0x0B: // STATUS STICKY
			return cdvd.StatusSticky;
		case 0x0C: // CRT MINUTE
			return itob((u8)(cdvd.CurrentSector / (60 * 75)));

		case 0x0D: // CRT SECOND
			return itob((u8)((cdvd.CurrentSector / 75) % 60) + 2);

		case 0x0E: // CRT FRAME
			return itob((u8)(cdvd.CurrentSector % 75));

		case 0x0F: // TYPE
			if (cdvd.Tray.trayState == CDVD_DISC_ENGAGED)
				return cdvd.DiscType;
			return (cdvd.Tray.trayState <= CDVD_DISC_SEEKING) ? cdvdTrayStateDetecting() : 0; // Detecting Disc / No Disc

		case 0x13: // SPEED
		{
			u8 speedCtrl = cdvd.SpindlCtrl & 0x3F;

			if (speedCtrl == 0)
				speedCtrl = cdvdIsDVD() ? 3 : 5;

			if (cdvdIsDVD())
				speedCtrl += 0xF;
			else
				speedCtrl--;

			if (cdvd.Tray.trayState != CDVD_DISC_ENGAGED || cdvd.Spinning == false)
				speedCtrl = 0;

			return speedCtrl;
		}


		case 0x15: // RSV
			return 0x0; //  PSX DESR related, but confirmed to be 0 on normal PS2

		case 0x16: // SCOMMAND
			return cdvd.sCommand;

		case 0x17: // SREADY
			return cdvd.sDataIn;

		case 0x18:
			return cdvdRead18();

		case 0x20:
		case 0x21:
		case 0x22:
		case 0x23:
		case 0x24:
		{
			int temp = key - 0x20;
			return cdvd.Key[temp];
		}
		case 0x28:
		case 0x29:
		case 0x2A:
		case 0x2B:
		case 0x2C:
		{
			int temp = key - 0x23;
			return cdvd.Key[temp];
		}

		case 0x30:
		case 0x31:
		case 0x32:
		case 0x33:
		case 0x34:
		{
			int temp = key - 0x26;
			return cdvd.Key[temp];
		}

		case 0x38: // valid parts of key data (first and last are valid)
			return cdvd.Key[15];

		case 0x39: // KEY-XOR
			return cdvd.KeyXor;

		case 0x3A: // DEC_SET
			return cdvd.decSet;

		default:
			// note: notify the console since this is a potentially serious emulation problem:
			// return -1 (all bits set) instead of 0, improves chances of the software being happy
			break;
	}
	return -1;
}

static bool cdvdReadErrorHandler(void)
{
	if (cdvd.SectorCnt <= 0)
	{
		cdvd.Error = 0x21; // Number of read sectors abnormal
		return false;
	}

	if (cdvd.SeekToSector >= cdvd.MaxSector)
	{
		// Probably should be 0x20 (bad LSN) but apparently Silent Hill 2 Black Ribbon has a fade at the end of the first trailer
		// And the only way you can throw an error and it still does that is to use 0x30 (Read error), anything else it skips the fade.
		// This'll do for now but needs investigation
		cdvd.Error = 0x30; // Problem occurred during read
		return false;
	}

	return true;
}

static bool cdvdCommandErrorHandler(void)
{
	static u8 cdvdParamLength[16] = { 0, 0, 0, 0, 0, 4, 11, 11, 11, 1, 255, 255, 7, 2, 11, 1 };

	if (cdvd.nCommand > N_CD_NOP) // Command needs a disc, so check the tray is closed
	{
		if ((cdvd.Status & CDVD_STATUS_TRAY_OPEN) || (cdvd.DiscType == CDVD_TYPE_NODISC))
		{
			cdvd.Error = (cdvd.DiscType == CDVD_TYPE_NODISC) ? 0x12 : 0x11; // No Disc Tray is open
			cdvd.Ready |= CDVD_DRIVE_ERROR;
			cdvdSetIrq();
			return false;
		}
	}

	if (cdvd.NCMDParamCnt != cdvdParamLength[cdvd.nCommand] && cdvdParamLength[cdvd.nCommand] != 255)
	{
		cdvd.Error = 0x22; // Invalid parameter for command
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq();
		return false;
	}

	if (cdvd.nCommand > N_CD_CHG_SPDL_CTRL)
	{
		cdvd.Error = 0x10; // Unsupported Command
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq();
		return false;
	}

	return true;
}

static void cdvdWrite04(u8 rt) /* NCOMMAND */
{
	if (!(cdvd.Ready & CDVD_DRIVE_READY))
	{
		cdvd.Error = 0x13; // Not Ready
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq();
		cdvd.NCMDParamPos = 0;
		cdvd.NCMDParamCnt = 0;
		return;
	}

	cdvd.nCommand = rt;
	cdvd.AbortRequested = false;

	if (!cdvdCommandErrorHandler())
	{
		cdvd.NCMDParamPos = 0;
		cdvd.NCMDParamCnt = 0;
		return;
	}

	switch (rt)
	{
		case N_CD_RESET: // CdSync
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvd.SCMDParamPos = 0;
			cdvd.SCMDParamCnt = 0;
			cdvdUpdateStatus(CDVD_STATUS_STOP);
			cdvd.Spinning = false;
			memset(cdvd.SCMDResultBuff, 0, sizeof(cdvd.SCMDResultBuff));
			cdvdSetIrq();
			break;

		case N_CD_STANDBY: // CdStandby

			// Seek to sector zero.  The cdvdStartSeek function will simulate
			// spinup times if needed.
			CDVD_INT(cdvdStartSeek(0, static_cast<CDVD_MODE_TYPE>(cdvdIsDVD()), false));
			// Might not seek, but makes sense since it does move to the inner most track
			// It's only temporary until the interrupt anyway when it sets itself ready
			cdvdUpdateStatus(CDVD_STATUS_SEEK);
			cdvd.Action = cdvdAction_Standby;
			break;

		case N_CD_STOP: // CdStop
			cdvdUpdateReady(CDVD_DRIVE_BUSY);
			CDVDCancelReadAhead();
			cdvdUpdateStatus(CDVD_STATUS_SPIN);
			CDVD_INT(PSXCLK / 6); // 166ms delay?
			cdvd.Action = cdvdAction_Stop;
			break;

		case N_CD_PAUSE: // CdPause
			// A few games rely on PAUSE setting the Status correctly.
			// However we should probably stop any read in progress too, just to be safe
			psxRegs.interrupt &= ~(1 << IopEvt_Cdvd);
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvdSetIrq();
			//After Pausing needs to buffer the next sector
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case N_CD_SEEK: // CdSeek
			cdvd.Action = cdvdAction_Seek; // Have to do this first, the StartSeek relies on it
			CDVD_INT(cdvdStartSeek(*reinterpret_cast<uint*>(cdvd.NCMDParamBuff + 0), static_cast<CDVD_MODE_TYPE>(cdvdIsDVD()), false));
			cdvdUpdateStatus(CDVD_STATUS_SEEK);
			break;

		case N_CD_READ: // CdRead
		{
			// Assign the seek to sector based on cdvd.Param[0]-[3], and the number of  sectors based on cdvd.Param[4]-[7].
			cdvd.SeekToSector  = *(u32*)(cdvd.NCMDParamBuff + 0);
			cdvd.SectorCnt     = *(u32*)(cdvd.NCMDParamBuff + 4);
			cdvd.RetryCntMax   = (cdvd.NCMDParamBuff[8] == 0) ? 0x100 : cdvd.NCMDParamBuff[8];
			u32 oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParamBuff[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParamBuff[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParamBuff[9] & 0x80) | (cdvdIsDVD() ? 3 : 5); // Max speed for DVD/CD

			bool ParamError = false;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1: // x1
					cdvd.Speed = 1;
					break;
				case 2: // x2
					cdvd.Speed = 2;
					break;
				case 3: // x4
					cdvd.Speed = 4;
					break;
				case 4: // x12
					if (cdvdIsDVD())
						ParamError = true;
					else
						cdvd.Speed = 12;
					break;
				case 5: // x24
					if (cdvdIsDVD())
						ParamError = true;
					else
						cdvd.Speed = 24;
					break;
				default:
					ParamError = true;
					break;
			}

			if (cdvdIsDVD() && cdvd.NCMDParamBuff[10] != 0)
				ParamError = true;
			else
			{
				switch (cdvd.NCMDParamBuff[10])
				{
					case 2:
						cdvd.ReadMode = CDVD_MODE_2340;
						cdvd.BlockSize = 2340;
						break;
					case 1:
						cdvd.ReadMode = CDVD_MODE_2328;
						cdvd.BlockSize = 2328;
						break;
					case 0:
						cdvd.ReadMode = CDVD_MODE_2048;
						cdvd.BlockSize = 2048;
						break;
					default:
						ParamError = true;
						break;
				}
			}

			if (ParamError)
			{
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error = 0x22; // Invalid Parameter
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			if (!cdvdReadErrorHandler())
			{
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvdRotationTime((CDVD_MODE_TYPE)cdvdIsDVD()));
				break;
			}

			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, static_cast<CDVD_MODE_TYPE>(cdvdIsDVD()), !(cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) && (oldSpindleCtrl & CDVD_SPINDLE_CAV)));

			// Read-ahead by telling CDVD about the track now.
			// This helps improve performance on actual from-cd emulation
			// (ie, not using the hard drive)
			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			// Set the reading block flag.  If a seek is pending then SeekCompleted will
			// take priority in the handler anyway.  If the read is contiguous then
			// this'll skip the seek delay.
			cdvd.Reading = 1;
			break;
		}
		case N_CD_READ_CDDA: // CdReadCDDA
		case N_CD_READ_XCDDA: // CdReadXCDDA
		{
			if (cdvdIsDVD())
			{
				cdvd.Error = 0x14; // Invalid for current disc type
				cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
				cdvdSetIrq();
				return;
			}
			// Assign the seek to sector based on cdvd.Param[0]-[3], and the number of  sectors based on cdvd.Param[4]-[7].
			cdvd.SeekToSector  = *(u32*)(cdvd.NCMDParamBuff + 0);
			cdvd.SectorCnt     = *(u32*)(cdvd.NCMDParamBuff + 4);
			cdvd.RetryCntMax   = (cdvd.NCMDParamBuff[8] == 0) ? 0x100 : cdvd.NCMDParamBuff[8];

			u32 oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParamBuff[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParamBuff[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParamBuff[9] & 0x80) | 5; // Max speed for CD

			bool ParamError = false;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1: // x1
					cdvd.Speed = 1;
					break;
				case 2: // x2
					cdvd.Speed = 2;
					break;
				case 3: // x4
					cdvd.Speed = 4;
					break;
				case 4: // x12
					cdvd.Speed = 12;
					break;
				case 5: // x24
					cdvd.Speed = 24;
					break;
				default:
					ParamError = true;
					break;
			}

			switch (cdvd.NCMDParamBuff[10])
			{
				case 1:
					cdvd.ReadMode = CDVD_MODE_2368;
					cdvd.BlockSize = 2368;
					break;
				case 0:
					cdvd.ReadMode = CDVD_MODE_2352;
					cdvd.BlockSize = 2352;
					break;
				default:
					ParamError = true;
					break;
			}

			if (ParamError)
			{
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error = 0x22; // Invalid Parameter
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, MODE_CDROM, !(cdvd.SpindlCtrl& CDVD_SPINDLE_CAV) && (oldSpindleCtrl& CDVD_SPINDLE_CAV)));

			// Read-ahead by telling CDVD about the track now.
			// This helps improve performance on actual from-cd emulation
			// (ie, not using the hard drive)
			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			// Set the reading block flag.  If a seek is pending then SeekCompleted will
			// take priority in the handler anyway.  If the read is contiguous then
			// this'll skip the seek delay.
			cdvd.Reading = 1;
			break;
		}
		case N_DVD_READ: // DvdRead
		{
			if (!cdvdIsDVD())
			{
				cdvd.Error = 0x14; // Invalid for current disc type
				cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
				cdvdSetIrq();
				return;
			}
			// Assign the seek to sector based on cdvd.Param[0]-[3], and the number of  sectors based on cdvd.Param[4]-[7].
			cdvd.SeekToSector  = *(u32*)(cdvd.NCMDParamBuff + 0);
			cdvd.SectorCnt     = *(u32*)(cdvd.NCMDParamBuff + 4);

			u32 oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParamBuff[8] == 0)
				cdvd.RetryCntMax = 0x100;
			else
				cdvd.RetryCntMax = cdvd.NCMDParamBuff[8];

			if (cdvd.NCMDParamBuff[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParamBuff[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParamBuff[9] & 0x80) | 3; // Max speed for DVD

			bool ParamError = false;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1: // x1
					cdvd.Speed = 1;
					break;
				case 2: // x2
					cdvd.Speed = 2;
					break;
				case 3: // x4
					cdvd.Speed = 4;
					break;
				default:
					ParamError = true;
					break;
			}

			if (cdvd.NCMDParamBuff[10] != 0)
				ParamError = true;

			cdvd.ReadMode = CDVD_MODE_2048;
			cdvd.BlockSize = 2064;

			if (ParamError)
			{
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error      = 0x22; // Invalid Parameter
				cdvd.Action     = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			if (!cdvdReadErrorHandler())
			{
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvdRotationTime((CDVD_MODE_TYPE)cdvdIsDVD()));
				break;
			}

			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, MODE_DVDROM, !(cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) && (oldSpindleCtrl& CDVD_SPINDLE_CAV)));

			// Read-ahead by telling CDVD about the track now.
			// This helps improve performance on actual from-cd emulation
			// (ie, not using the hard drive)
			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			// Set the reading block flag.  If a seek is pending then SeekCompleted will
			// take priority in the handler anyway.  If the read is contiguous then
			// this'll skip the seek delay.
			cdvd.Reading = 1;
			break;
		}
		case N_CD_GET_TOC: // CdGetToc & cdvdman_call19
			//Param[0] is 0 for CdGetToc and any value for cdvdman_call19
			CDVD->getTOC(iopPhysMem(HW_DMA3_MADR));
			cdvdSetIrq();
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
			cdvdUpdateReady(CDVD_DRIVE_READY);
			//After reading the TOC it needs to go back to buffer the next sector
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case N_CD_READ_KEY: // CdReadKey
			{
				u8 arg0 = cdvd.NCMDParamBuff[0];
				u16 arg1 = cdvd.NCMDParamBuff[1] | (cdvd.NCMDParamBuff[2] << 8);
				u32 arg2 = cdvd.NCMDParamBuff[3] | (cdvd.NCMDParamBuff[4] << 8) | (cdvd.NCMDParamBuff[5] << 16) | (cdvd.NCMDParamBuff[6] << 24);
				cdvdReadKey(arg0, arg1, arg2, cdvd.Key);
				cdvd.KeyXor = 0x00;
				cdvdSetIrq();
				//After reading the key it needs to go back to buffer the next sector
				cdvdUpdateStatus(CDVD_STATUS_PAUSE);
				cdvdUpdateReady(CDVD_DRIVE_READY);
				cdvd.nextSectorsBuffered = 0;
				CDVDSECTORREADY_INT(cdvd.ReadTime);
			}
			break;
		case N_CD_NOP: // CdNop_
			cdvdUpdateReady(CDVD_DRIVE_READY);
			/* fallthrough */
		case N_CD_CHG_SPDL_CTRL: // CdChgSpdlCtrl
		default: // Should be unreachable, handled in the error handler earlier
			cdvdSetIrq();
			break;
	}
	cdvd.NCMDParamPos = 0;
	cdvd.NCMDParamCnt = 0;
}

static __fi void cdvdWrite05(u8 rt) /* NDATAIN */
{
	if (cdvd.NCMDParamPos >= 16)
	{
		cdvd.NCMDParamPos = 0;
		cdvd.NCMDParamCnt = 0;
	}

	cdvd.NCMDParamBuff[cdvd.NCMDParamPos++] = rt;
	cdvd.NCMDParamCnt++;
}

static __fi void cdvdWrite07(u8 rt) // BREAK
{
	// If we're already in a Ready state or already Breaking, then do nothing:
	if (!(cdvd.Ready & CDVD_DRIVE_BUSY) || cdvd.AbortRequested)
		return;

	cdvd.AbortRequested = true;
}

static void cdvdWrite16(u8 rt) // SCOMMAND
{
	//	cdvdTN	diskInfo;
	//	cdvdTD	trackInfo;
	//	int i, lbn, type, min, sec, frm, address;
	int address;
	u8 tmp;
	cdvd.sCommand = rt;
	memset(cdvd.SCMDResultBuff, 0, sizeof(cdvd.SCMDResultBuff));

	switch (rt)
	{
		//		case 0x01: // GetDiscType - from cdvdman (0:1)
		//			SetResultSize(1);
		//			cdvd.Result[0] = 0;
		//			break;

		case 0x02: // CdReadSubQ  (0:11)
			SetSCMDResultSize(11);
			cdvd.SCMDResultBuff[0] = cdvdReadSubQ(cdvd.CurrentSector, (cdvdSubQ*)&cdvd.SCMDResultBuff[1]);
			break;

		case 0x03: // Mecacon-command
			switch (cdvd.SCMDParamBuff[0])
			{
				case 0x00: // get mecha version (1:4)
					SetSCMDResultSize(4);
					memcpy(&cdvd.SCMDResultBuff[0], &s_mecha_version, sizeof(u32));
					break;
				case 0x30:
					SetSCMDResultSize(2);
					cdvd.SCMDResultBuff[0] = cdvd.Status;
					cdvd.SCMDResultBuff[1] = (cdvd.Status & 0x1) ? 8 : 0;
					//Console.Warning("Tray check param[1]=%02X", cdvd.Param[1]);
					break;
				case 0x44: // write console ID (9:1)
					SetSCMDResultSize(1);
					cdvdWriteNVM(&cdvd.SCMDParamBuff[1], getNvmLayout()->consoleId, 8);
					break;

				case 0x45: // read console ID (1:9)
					SetSCMDResultSize(9);
					cdvdReadNVM(&cdvd.SCMDResultBuff[1], getNvmLayout()->consoleId, 8);
					break;

				case 0xFD: // _sceCdReadRenewalDate (1:6) BCD
					SetSCMDResultSize(6);
					cdvd.SCMDResultBuff[0] = 0;
					cdvd.SCMDResultBuff[1] = 0x04; //year
					cdvd.SCMDResultBuff[2] = 0x12; //month
					cdvd.SCMDResultBuff[3] = 0x10; //day
					cdvd.SCMDResultBuff[4] = 0x01; //hour
					cdvd.SCMDResultBuff[5] = 0x30; //min
					break;

				case 0xEF: // read console temperature (1:3)
					   // This returns a fixed value of 30.5 C
					SetSCMDResultSize(3);
					cdvd.SCMDResultBuff[0] = 0; // returns 0 on success
					cdvd.SCMDResultBuff[1] = 0x0F; // last 8 bits for integer
					cdvd.SCMDResultBuff[2] = 0x05; // leftmost bit for integer, other 7 bits for decimal place
					break;

				default:
					SetSCMDResultSize(1);
					cdvd.SCMDResultBuff[0] = 0x81;
					break;
			}
			break;

		case 0x05: // CdTrayReqState (0:1) - resets the tray open detection
			   //Console.Warning("CdTrayReqState. cdvd.Status = %d", cdvd.Status);
			   // This function sets the Sticky tray flag to the same value as Status for detecting change
			cdvd.StatusSticky = cdvd.Status & CDVD_STATUS_TRAY_OPEN;

			SetSCMDResultSize(1);
			cdvd.SCMDResultBuff[0] = 0; // Could be a bit to say it's busy, but actual function is unknown, it expects 0 to continue.
			break;

		case 0x06: // CdTrayCtrl  (1:1)
			SetSCMDResultSize(1);
			if (cdvd.SCMDParamBuff[0] == 0)
				cdvd.SCMDResultBuff[0] = cdvdCtrlTrayOpen();
			else
				cdvd.SCMDResultBuff[0] = cdvdCtrlTrayClose();
			break;

		case 0x08: // CdReadRTC (0:8)
			SetSCMDResultSize(8);
			cdvd.SCMDResultBuff[0] = 0;
			cdvd.SCMDResultBuff[1] = itob(cdvd.RTC.second); //Seconds
			cdvd.SCMDResultBuff[2] = itob(cdvd.RTC.minute); //Minutes
			cdvd.SCMDResultBuff[3] = itob(cdvd.RTC.hour);   //Hours
			cdvd.SCMDResultBuff[4] = 0;                     //Nothing
			cdvd.SCMDResultBuff[5] = itob(cdvd.RTC.day);    //Day
			cdvd.SCMDResultBuff[6] = itob(cdvd.RTC.month);  //Month
			cdvd.SCMDResultBuff[7] = itob(cdvd.RTC.year);   //Year
			break;

		case 0x09: // sceCdWriteRTC (7:1)
			SetSCMDResultSize(1);
			cdvd.SCMDResultBuff[0] = 0;
			cdvd.RTC.pad = 0;

			cdvd.RTC.second = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 7]);
			cdvd.RTC.minute = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 6]) % 60;
			cdvd.RTC.hour = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 5]) % 24;
			cdvd.RTC.day = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 3]);
			cdvd.RTC.month = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 2] & 0x7f);
			cdvd.RTC.year = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 1]);
			break;

		case 0x0A: // sceCdReadNVM (2:3)
			address = (cdvd.SCMDParamBuff[0] << 8) | cdvd.SCMDParamBuff[1];

			if (address < 512)
			{
				SetSCMDResultSize(3);
				cdvdReadNVM(&cdvd.SCMDResultBuff[1], address * 2, 2);
				// swap bytes around
				tmp = cdvd.SCMDResultBuff[1];
				cdvd.SCMDResultBuff[1] = cdvd.SCMDResultBuff[2];
				cdvd.SCMDResultBuff[2] = tmp;
			}
			else
			{
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0xff;
			}
			break;

		case 0x0B: // sceCdWriteNVM (4:1)
			SetSCMDResultSize(1);
			address = (cdvd.SCMDParamBuff[0] << 8) | cdvd.SCMDParamBuff[1];

			if (address < 512)
			{
				// swap bytes around
				tmp = cdvd.SCMDParamBuff[2];
				cdvd.SCMDParamBuff[2] = cdvd.SCMDParamBuff[3];
				cdvd.SCMDParamBuff[3] = tmp;
				cdvdWriteNVM(&cdvd.SCMDParamBuff[2], address * 2, 2);
			}
			else
				cdvd.SCMDResultBuff[0] = 0xff;
			break;

			//		case 0x0C: // sceCdSetHDMode (1:1)
			//			break;


		case 0x0F: // sceCdPowerOff (0:1)- Call74 from Xcdvdman
			VMManager::SetState(VMState::Stopping);
			break;

		case 0x12: // sceCdReadILinkId (0:9)
			SetSCMDResultSize(9);
			cdvdReadNVM(&cdvd.SCMDResultBuff[1], getNvmLayout()->ilinkId, 8);
			if ((!cdvd.SCMDResultBuff[3]) && (!cdvd.SCMDResultBuff[4])) // nvm file is missing correct iLinkId, return hardcoded one
			{
				cdvd.SCMDResultBuff[0] = 0x00;
				cdvd.SCMDResultBuff[1] = 0x00;
				cdvd.SCMDResultBuff[2] = 0xAC;
				cdvd.SCMDResultBuff[3] = 0xFF;
				cdvd.SCMDResultBuff[4] = 0xFF;
				cdvd.SCMDResultBuff[5] = 0xFF;
				cdvd.SCMDResultBuff[6] = 0xFF;
				cdvd.SCMDResultBuff[7] = 0xB9;
				cdvd.SCMDResultBuff[8] = 0x86;
			}
			break;

		case 0x13: // sceCdWriteILinkID (8:1)
			SetSCMDResultSize(1);
			cdvdWriteNVM(&cdvd.SCMDResultBuff[1], getNvmLayout()->ilinkId, 8);
			break;

		case 0x14: // CdCtrlAudioDigitalOut (1:1)
			   //parameter can be 2, 0, ...
			SetSCMDResultSize(1);
			cdvd.SCMDResultBuff[0] = 0; //8 is a flag; not used
			break;

		case 0x15: // sceCdForbidDVDP (0:1)
			SetSCMDResultSize(1);
			cdvd.SCMDResultBuff[0] = 5;
			break;

		case 0x16: // AutoAdjustCtrl - from cdvdman (1:1)
			SetSCMDResultSize(1);
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x17: // CdReadModelNumber (1:9) - from xcdvdman
			SetSCMDResultSize(9);
			cdvdReadNVM(&cdvd.SCMDResultBuff[1], getNvmLayout()->modelNum + cdvd.SCMDResultBuff[0], 8);
			break;

		case 0x18: // CdWriteModelNumber (9:1) - from xcdvdman
			SetSCMDResultSize(1);
			cdvdWriteNVM(&cdvd.SCMDParamBuff[1], getNvmLayout()->modelNum + cdvd.SCMDResultBuff[0], 8);
			break;

			//		case 0x19: // sceCdForbidRead (0:1) - from xcdvdman
			//			break;

		case 0x1A: // sceCdBootCertify (4:1)//(4:16 in psx?)
			SetSCMDResultSize(1); //on input there are 4 bytes: 1;?10;J;C for 18000; 1;60;E;C for 39002 from ROMVER
			cdvd.SCMDResultBuff[0] = 1; //i guess that means okay
			break;

		case 0x1B: // sceCdCancelPOffRdy (0:1) - Call73 from Xcdvdman (1:1)
			SetSCMDResultSize(1);
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x1C: // sceCdBlueLEDCtl (1:1) - Call72 from Xcdvdman
			SetSCMDResultSize(1);
			cdvd.SCMDResultBuff[0] = 0;
			break;

			//		case 0x1D: // cdvdman_call116 (0:5) - In V10 Bios
			//			break;

		case 0x1E: // sceRemote2Read (0:5) - // 00 14 AA BB CC -> remote key code
			SetSCMDResultSize(5);
			cdvd.SCMDResultBuff[0] = 0x00;
			cdvd.SCMDResultBuff[1] = 0x14;
			cdvd.SCMDResultBuff[2] = 0x00;
			cdvd.SCMDResultBuff[3] = 0x00;
			cdvd.SCMDResultBuff[4] = 0x00;
			break;

			//		case 0x1F: // sceRemote2_7 (2:1) - cdvdman_call117
			//			break;

		case 0x20: // sceRemote2_6 (0:3)	// 00 01 00
			SetSCMDResultSize(3);
			cdvd.SCMDResultBuff[0] = 0x00;
			cdvd.SCMDResultBuff[1] = 0x01;
			cdvd.SCMDResultBuff[2] = 0x00;
			break;

			//		case 0x21: // sceCdWriteWakeUpTime (8:1)
			//			break;

		case 0x22: // sceCdReadWakeUpTime (0:10)
			SetSCMDResultSize(10);
			cdvd.SCMDResultBuff[0] = 0;
			cdvd.SCMDResultBuff[1] = 0;
			cdvd.SCMDResultBuff[2] = 0;
			cdvd.SCMDResultBuff[3] = 0;
			cdvd.SCMDResultBuff[4] = 0;
			cdvd.SCMDResultBuff[5] = 0;
			cdvd.SCMDResultBuff[6] = 0;
			cdvd.SCMDResultBuff[7] = 0;
			cdvd.SCMDResultBuff[8] = 0;
			cdvd.SCMDResultBuff[9] = 0;
			break;

		case 0x24: // sceCdRCBypassCtrl (1:1) - In V10 Bios
			   // FIXME: because PRId<0x23, the bit 0 of sio2 don't get updated 0xBF808284
			SetSCMDResultSize(1);
			cdvd.SCMDResultBuff[0] = 0;
			break;

			//		case 0x25: // cdvdman_call120 (1:1) - In V10 Bios
			//			break;

			//		case 0x26: // cdvdman_call128 (0,3) - In V10 Bios
			//			break;

		case 0x27: // GetPS1BootParam (0:13) - called only by China region PS2 models

			// Return Disc Serial which is passed to PS1DRV and later used to find matching config.
			SetSCMDResultSize(13);
			cdvd.SCMDResultBuff[0] = 0;
			cdvd.SCMDResultBuff[1] = DiscSerial[0];
			cdvd.SCMDResultBuff[2] = DiscSerial[1];
			cdvd.SCMDResultBuff[3] = DiscSerial[2];
			cdvd.SCMDResultBuff[4] = DiscSerial[3];
			cdvd.SCMDResultBuff[5] = DiscSerial[4];
			cdvd.SCMDResultBuff[6] = DiscSerial[5];
			cdvd.SCMDResultBuff[7] = DiscSerial[6];
			cdvd.SCMDResultBuff[8] = DiscSerial[7];
			cdvd.SCMDResultBuff[9] = DiscSerial[9]; // Skipping dot here is required.
			cdvd.SCMDResultBuff[10] = DiscSerial[10];
			cdvd.SCMDResultBuff[11] = DiscSerial[11];
			cdvd.SCMDResultBuff[12] = DiscSerial[12];
			break;

			//		case 0x28: // cdvdman_call150 (1:1) - In V10 Bios
			//			break;

		case 0x29: //sceCdNoticeGameStart (1:1)
			SetSCMDResultSize(1);
			cdvd.SCMDResultBuff[0] = 0;
			break;

			//		case 0x2C: //sceCdXBSPowerCtl (2:2)
			//			break;

			//		case 0x2D: //sceCdXLEDCtl (2:2)
			//			break;

			//		case 0x2E: //sceCdBuzzerCtl (0:1)
			//			break;

			//		case 0x2F: //cdvdman_call167 (16:1)
			//			break;

			//		case 0x30: //cdvdman_call169 (1:9)
			//			break;

		case 0x31: //sceCdSetMediumRemoval (1:1)
			SetSCMDResultSize(1);
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x32: //sceCdGetMediumRemoval (0:2)
			SetSCMDResultSize(2);
			cdvd.SCMDResultBuff[0] = 0;
			//cdvd.Result[0] = 0; // fixme: I'm pretty sure that the same variable shouldn't be set twice here. Perhaps cdvd.Result[1]?
			break;

			//		case 0x33: //sceCdXDVRPReset (1:1)
			//			break;

		case 0x36: //cdvdman_call189 [__sceCdReadRegionParams - made up name] (0:15) i think it is 16, not 15
			SetSCMDResultSize(15);

			memcpy(&cdvd.SCMDResultBuff[1], &s_mecha_version, sizeof(u32));
			cdvdReadNVM(&cdvd.SCMDResultBuff[3], getNvmLayout()->regparams, 8);
			cdvd.SCMDResultBuff[1] = 1 << cdvd.SCMDResultBuff[1]; //encryption zone; see offset 0x1C in encrypted headers
								      //////////////////////////////////////////
			cdvd.SCMDResultBuff[2] = 0; //??
						//			cdvd.Result[3] == ROMVER[4] == *0xBFC7FF04
						//			cdvd.Result[4] == OSDVER[4] == CAP			Jjpn, Aeng, Eeng, Heng, Reng, Csch, Kkor?
						//			cdvd.Result[5] == OSDVER[5] == small
						//			cdvd.Result[6] == OSDVER[6] == small
						//			cdvd.Result[7] == OSDVER[7] == small
						//			cdvd.Result[8] == VERSTR[0x22] == *0xBFC7FF52
						//			cdvd.Result[9] == DVDID						J U O E A R C M
						//			cdvd.Result[10]== 0;					//??
			cdvd.SCMDResultBuff[11] = 0; //??
			cdvd.SCMDResultBuff[12] = 0; //??
						 //////////////////////////////////////////
			cdvd.SCMDResultBuff[13] = 0; //0xFF - 77001
			cdvd.SCMDResultBuff[14] = 0; //??
			break;

		case 0x37: //called from EECONF [sceCdReadMAC - made up name] (0:9)
			SetSCMDResultSize(9);
			cdvdReadNVM(&cdvd.SCMDResultBuff[1], getNvmLayout()->mac, 8);
			break;

		case 0x38: //used to fix the MAC back after accidentally trashed it :D [sceCdWriteMAC - made up name] (8:1)
			SetSCMDResultSize(1);
			cdvdWriteNVM(&cdvd.SCMDResultBuff[0], getNvmLayout()->mac, 8);
			break;

		case 0x3E: //[__sceCdWriteRegionParams - made up name] (15:1) [Florin: hum, i was expecting 14:1]
			SetSCMDResultSize(1);
			cdvdWriteNVM(&cdvd.SCMDParamBuff[2], getNvmLayout()->regparams, 8);
			break;

		case 0x40: // CdOpenConfig (3:1)
			SetSCMDResultSize(1);
			cdvd.CReadWrite = cdvd.SCMDParamBuff[0];
			cdvd.COffset = cdvd.SCMDParamBuff[1];
			cdvd.CNumBlocks = cdvd.SCMDParamBuff[2];
			cdvd.CBlockIndex = 0;
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x41: // CdReadConfig (0:16)
			SetSCMDResultSize(16);
			cdvdReadConfig(&cdvd.SCMDResultBuff[0]);
			break;

		case 0x42: // CdWriteConfig (16:1)
			SetSCMDResultSize(1);
			cdvdWriteConfig(&cdvd.SCMDParamBuff[0]);
			break;

		case 0x43: // CdCloseConfig (0:1)
			SetSCMDResultSize(1);
			cdvd.CReadWrite = 0;
			cdvd.COffset = 0;
			cdvd.CNumBlocks = 0;
			cdvd.CBlockIndex = 0;
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x80: // secrman: __mechacon_auth_0x80
			SetSCMDResultSize(1); //in:1
			cdvd.mg_datatype = 0; //data
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x81: // secrman: __mechacon_auth_0x81
			SetSCMDResultSize(1); //in:1
			cdvd.mg_datatype = 0; //data
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x82: // secrman: __mechacon_auth_0x82
			SetSCMDResultSize(1); //in:16
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x83: // secrman: __mechacon_auth_0x83
			SetSCMDResultSize(1); //in:8
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x84: // secrman: __mechacon_auth_0x84
			SetSCMDResultSize(1 + 8 + 4); //in:0
			cdvd.SCMDResultBuff[0] = 0;

			cdvd.SCMDResultBuff[1] = 0x21;
			cdvd.SCMDResultBuff[2] = 0xdc;
			cdvd.SCMDResultBuff[3] = 0x31;
			cdvd.SCMDResultBuff[4] = 0x96;
			cdvd.SCMDResultBuff[5] = 0xce;
			cdvd.SCMDResultBuff[6] = 0x72;
			cdvd.SCMDResultBuff[7] = 0xe0;
			cdvd.SCMDResultBuff[8] = 0xc8;

			cdvd.SCMDResultBuff[9] = 0x69;
			cdvd.SCMDResultBuff[10] = 0xda;
			cdvd.SCMDResultBuff[11] = 0x34;
			cdvd.SCMDResultBuff[12] = 0x9b;
			break;

		case 0x85: // secrman: __mechacon_auth_0x85
			SetSCMDResultSize(1 + 4 + 8); //in:0
			cdvd.SCMDResultBuff[0] = 0;

			cdvd.SCMDResultBuff[1] = 0xeb;
			cdvd.SCMDResultBuff[2] = 0x01;
			cdvd.SCMDResultBuff[3] = 0xc7;
			cdvd.SCMDResultBuff[4] = 0xa9;

			cdvd.SCMDResultBuff[5] = 0x3f;
			cdvd.SCMDResultBuff[6] = 0x9c;
			cdvd.SCMDResultBuff[7] = 0x5b;
			cdvd.SCMDResultBuff[8] = 0x19;
			cdvd.SCMDResultBuff[9] = 0x31;
			cdvd.SCMDResultBuff[10] = 0xa0;
			cdvd.SCMDResultBuff[11] = 0xb3;
			cdvd.SCMDResultBuff[12] = 0xa3;
			break;

		case 0x86: // secrman: __mechacon_auth_0x86
			SetSCMDResultSize(1); //in:16
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x87: // secrman: __mechacon_auth_0x87
			SetSCMDResultSize(1); //in:8
			cdvd.SCMDResultBuff[0] = 0;
			break;

		case 0x8D: // sceMgWriteData
			SetSCMDResultSize(1); //in:length<=16
			if (cdvd.mg_size + cdvd.SCMDParamCnt > cdvd.mg_maxsize)
				cdvd.SCMDResultBuff[0] = 0x80;
			else
			{
				memcpy(&cdvd.mg_buffer[cdvd.mg_size], cdvd.SCMDParamBuff, cdvd.SCMDParamCnt);
				cdvd.mg_size += cdvd.SCMDParamCnt;
				cdvd.SCMDResultBuff[0] = 0; // 0 complete ; 1 busy ; 0x80 error
			}
			break;

		case 0x8E: // sceMgReadData
			SetSCMDResultSize(std::min(16, cdvd.mg_size));
			memcpy(&cdvd.SCMDResultBuff[0], &cdvd.mg_buffer[0], cdvd.SCMDResultCnt);
			cdvd.mg_size -= cdvd.SCMDResultCnt;
			memcpy(&cdvd.mg_buffer[0], &cdvd.mg_buffer[cdvd.SCMDResultCnt], cdvd.mg_size);
			break;

		case 0x88: // secrman: __mechacon_auth_0x88	//for now it is the same; so, fall;)
		case 0x8F: // secrman: __mechacon_auth_0x8F
			SetSCMDResultSize(1); //in:0
			if (cdvd.mg_datatype == 1) // header data
			{
				int bit_ofs;

				if ((cdvd.mg_maxsize != cdvd.mg_size) || (cdvd.mg_size < 0x20) || (cdvd.mg_size != *(u16*)&cdvd.mg_buffer[0x14]))
				{
					cdvd.SCMDResultBuff[0] = 0x80;
					break;
				}

				bit_ofs = mg_BIToffset(&cdvd.mg_buffer[0]);

				memcpy(&cdvd.mg_kbit[0], &cdvd.mg_buffer[bit_ofs - 0x20], 0x10);
				memcpy(&cdvd.mg_kcon[0], &cdvd.mg_buffer[bit_ofs - 0x10], 0x10);

				if ((cdvd.mg_buffer[bit_ofs + 5] || cdvd.mg_buffer[bit_ofs + 6] || cdvd.mg_buffer[bit_ofs + 7]) ||
						(cdvd.mg_buffer[bit_ofs + 4] * 16 + bit_ofs + 8 + 16 != *(u16*)&cdvd.mg_buffer[0x14]))
				{
					cdvd.SCMDResultBuff[0] = 0x80;
					break;
				}
			}
			cdvd.SCMDResultBuff[0] = 0; // 0 complete ; 1 busy ; 0x80 error
			break;

		case 0x90: // sceMgWriteHeaderStart
			SetSCMDResultSize(1); //in:5
			cdvd.mg_size = 0;
			cdvd.mg_datatype = 1; //header data

			cdvd.SCMDResultBuff[0] = 0; // 0 complete ; 1 busy ; 0x80 error
			break;

		case 0x91: // sceMgReadBITLength
			{
				SetSCMDResultSize(3); //in:0
				const int bit_ofs = mg_BIToffset(&cdvd.mg_buffer[0]);
				memcpy(&cdvd.mg_buffer[0], &cdvd.mg_buffer[bit_ofs], static_cast<size_t>(8 + 16 * static_cast<int>(cdvd.mg_buffer[bit_ofs + 4])));

				cdvd.mg_maxsize = 0; // don't allow any write
				cdvd.mg_size = 8 + 16 * cdvd.mg_buffer[4]; //new offset, i just moved the data

				cdvd.SCMDResultBuff[0] = (cdvd.mg_datatype == 1) ? 0 : 0x80; // 0 complete ; 1 busy ; 0x80 error
				cdvd.SCMDResultBuff[1] = (cdvd.mg_size >> 0) & 0xFF;
				cdvd.SCMDResultBuff[2] = (cdvd.mg_size >> 8) & 0xFF;
				break;
			}
		case 0x92: // sceMgWriteDatainLength
			SetSCMDResultSize(1); //in:2
			cdvd.mg_size           = 0;
			cdvd.mg_datatype       = 0; //data (encrypted)
			cdvd.mg_maxsize        = cdvd.SCMDParamBuff[0] | (((int)cdvd.SCMDParamBuff[1]) << 8);
			cdvd.SCMDResultBuff[0] = 0; // 0 complete ; 1 busy ; 0x80 error
			break;

		case 0x93: // sceMgWriteDataoutLength
			SetSCMDResultSize(1); //in:2
			if (((cdvd.SCMDParamBuff[0] | (((int)cdvd.SCMDParamBuff[1]) << 8)) == cdvd.mg_size) && (cdvd.mg_datatype == 0))
			{
				cdvd.mg_maxsize = 0; // don't allow any write
				cdvd.SCMDResultBuff[0] = 0; // 0 complete ; 1 busy ; 0x80 error
			}
			else
				cdvd.SCMDResultBuff[0] = 0x80;
			break;

		case 0x94: // sceMgReadKbit - read first half of BIT key
			SetSCMDResultSize(1 + 8); //in:0
			cdvd.SCMDResultBuff[0] = 0;
			memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kbit, 8);
			break;

		case 0x95: // sceMgReadKbit2 - read second half of BIT key
			SetSCMDResultSize(1 + 8); //in:0
			cdvd.SCMDResultBuff[0] = 0;
			memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kbit + 8, 8);
			break;

		case 0x96: // sceMgReadKcon - read first half of content key
			SetSCMDResultSize(1 + 8); //in:0
			cdvd.SCMDResultBuff[0] = 0;
			memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kcon, 8);
			break;

		case 0x97: // sceMgReadKcon2 - read second half of content key
			SetSCMDResultSize(1 + 8); //in:0
			cdvd.SCMDResultBuff[0] = 0;
			memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kcon + 8, 8);
			break;

		default:
			SetSCMDResultSize(1); //in:0
			cdvd.SCMDResultBuff[0] = 0x80; // 0 complete ; 1 busy ; 0x80 error
			break;
	} // end switch

	cdvd.SCMDParamPos = 0;
	cdvd.SCMDParamCnt = 0;
}

static __fi void cdvdWrite17(u8 rt) /* SDATAIN */
{
	if (cdvd.SCMDParamPos >= 16)
	{
		cdvd.SCMDParamPos = 0;
		cdvd.SCMDParamCnt = 0;
	}

	cdvd.SCMDParamBuff[cdvd.SCMDParamPos++] = rt;
	cdvd.SCMDParamCnt++;
}

void cdvdWrite(u8 key, u8 rt)
{
	switch (key)
	{
		case 0x04:
			cdvdWrite04(rt);
			break;
		case 0x05:
			cdvdWrite05(rt);
			break;
		case 0x06:
			cdvd.HowTo = rt;
			break;
		case 0x07:
			cdvdWrite07(rt);
			break;
		case 0x08:
			cdvd.IntrStat &= ~rt;
			break;
		case 0x0A: /* STATUS */
			break;
		case 0x0F: /* TYPE */
			break;
		case 0x14:
			// Rama Or WISI guessed that "2" literally meant 2x but we can get 
			// 0x02 or 0xfe for "Standard" or "Fast" it appears. It is unsure what those values are meant to be
			// Tests with ref suggest this register is write only? - Weirdbeard
			break;
		case 0x16:
			cdvdWrite16(rt);
			break;
		case 0x17:
			cdvdWrite17(rt);
			break;
		case 0x18: /* SDATAOUT */
			break;
		case 0x3A: /* DEC-SET */
			cdvd.decSet = rt;
			break;
		default:
			break;
	}
}

// return value:
//   0 - Invalid or unknown disc.
//   1 - PS1 CD
//   2 - PS2 CD
int GetPS2ElfName( std::string& name )
{
	int retype = 0;

	IsoFSCDVD isofs;
	IsoFile file(isofs);

	if (!file.open("SYSTEM.CNF;1"))
		return 0;

	int size = file.getLength();
	if( size == 0 ) return 0;

	while( !file.eof() )
	{
		const std::string line(file.readLine());
		std::string_view key, value;
		if (!StringUtil::ParseAssignmentString(line, &key, &value))
			continue;

		if( value.empty() && file.getLength() != file.getSeekPos() )
		{ // Some games have a character on the last line of the file, don't print the error in those cases.
			Console.Warning( "(SYSTEM.CNF) Unusual or malformed entry in SYSTEM.CNF ignored:" );
			Console.WriteLn(line.c_str());
			continue;
		}

		if( key == "BOOT2" )
		{
			Console.WriteLn( Color_StrongBlue, "(SYSTEM.CNF) Detected PS2 Disc = %.*s",
					static_cast<int>(value.size()), value.data());
			name = value;
			retype = 2;
		}
		else if( key == "BOOT" )
		{
			Console.WriteLn( Color_StrongBlue, "(SYSTEM.CNF) Detected PSX/PSone Disc = %.*s",
					static_cast<int>(value.size()), value.data());
			name = value;
			retype = 1;
		}
		else if( key == "VMODE" )
		{
			Console.WriteLn( Color_Blue, "(SYSTEM.CNF) Disc region type = %.*s",
					static_cast<int>(value.size()), value.data());
		}
		else if( key == "VER" )
		{
			Console.WriteLn( Color_Blue, "(SYSTEM.CNF) Software version = %.*s",
					static_cast<int>(value.size()), value.data());
		}
	}

	if( retype == 0 )
		return 0;

	return retype;
}
