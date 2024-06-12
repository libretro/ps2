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

#include "Common.h"

#include <cstring>
#include <list>

#include <libretro.h>

#include "GS.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "Elfheader.h"

#include "Host.h"
#include "VMManager.h"

// Mask to apply to ring buffer indices to wrap the pointer from end to
// start (the wrapping is what makes it a ringbuffer, yo!)
static const unsigned int RINGBUFFERMASK = RINGBUFFERSIZE - 1;

union PacketTagType
{
	struct
	{
		u32 command;
		u32 data[3];
	};
	struct
	{
		u32 _command;
		u32 _data[1];
		uptr pointer;
	};
};

// =====================================================================================================
//  MTGS Threaded Class Implementation
// =====================================================================================================

alignas(__cachelinesize) static u128 m_Ring[RINGBUFFERSIZE];

extern struct retro_hw_render_callback hw_render;

namespace MTGS
{
	static void SetEvent();

	// note: when s_ReadPos == s_WritePos, the fifo is empty
	// Threading info: s_ReadPos is updated by the MTGS thread. s_WritePos is updated by the EE thread
	static volatile unsigned int s_ReadPos      = 0; // cur pos gs is reading from
	static volatile unsigned int s_WritePos     = 0; // cur pos ee thread is writing to

	static volatile int  s_QueuedFrameCount      = 0;
	static volatile bool s_VsyncSignalListener  = false;

	static std::mutex s_mtx_RingBufferBusy2; // Gets released on semaXGkick waiting...
	static Threading::WorkSema s_sem_event;
	static Threading::UserspaceSemaphore s_sem_Vsync;

	// Used to delay the sending of events.  Performance is better if the ringbuffer
	// has more than one command in it when the thread is kicked.
	static int s_CopyDataTally                      = 0;

	static std::thread::id s_thread;
	static Threading::ThreadHandle s_thread_handle;
	static volatile bool s_open_flag = false;
	static Threading::UserspaceSemaphore s_open_or_close_done;
};

const Threading::ThreadHandle& MTGS::GetThreadHandle() { return s_thread_handle; }
bool MTGS::IsOpen() { return s_open_flag; }

void MTGS::ResetGS(bool hardware_reset)
{
	// MTGS Reset process:
	//  * clear the ringbuffer.
	//  * Signal a reset.
	//  * clear the path and byRegs structs (used by GIFtagDummy)
	if (hardware_reset)
	{
		s_ReadPos             = s_WritePos;
		s_QueuedFrameCount    = 0;
		s_VsyncSignalListener = 0;
	}

	PacketTagType& tag          = (PacketTagType&)m_Ring[s_WritePos];

	tag.command                 = GS_RINGTYPE_RESET;
	tag.data[0]                 = static_cast<int>(hardware_reset);
	tag.data[1]                 = 0;
	tag.data[2]                 = 0;

	s_WritePos = (s_WritePos + 1) & RINGBUFFERMASK;
	++s_CopyDataTally;

	if (hardware_reset)
		SetEvent();
}

void MTGS::PostVsyncStart()
{
	// Command qword: Low word is the command, and the high word is the packet
	// length in SIMDs (128 bits).
	PacketTagType& tag                = (PacketTagType&)m_Ring[s_WritePos];
	tag.command                       = GS_RINGTYPE_VSYNC;
	tag.data[0]                       = 0;

	s_WritePos = (s_WritePos + 1) & RINGBUFFERMASK;
	++s_CopyDataTally;

	// Vsyncs should always start the GS thread, regardless of how little has actually be queued.
	SetEvent();

	// If the MTGS is allowed to queue a lot of frames in advance, it creates input lag.
	// Use the Queued FrameCount to stall the EE if another vsync (or two) are already queued
	// in the ringbuffer.  The queue limit is disabled when both FrameLimiting and Vsync are
	// disabled, since the queue can have perverse effects on framerate benchmarking.

	// Edit: It's possible that MTGS is that much faster than GS that it creates so much lag,
	// a game becomes uncontrollable (software rendering for example).
	// For that reason it's better to have the limit always in place, at the cost of a few max FPS in benchmarks.
	// If those are needed back, it's better to increase the VsyncQueueSize via PCSX_vm.ini.
	// (The Xenosaga engine is known to run into this, due to it throwing bulks of data in one frame followed by 2 empty frames.)

	if ((s_QueuedFrameCount++ < EmuConfig.GS.VsyncQueueSize))
		return;

	s_VsyncSignalListener = true;

	s_sem_Vsync.Wait();
}

void MTGS::InitAndReadFIFO(u8* mem, u32 qwc)
{
	if (EmuConfig.GS.HWDownloadMode >= GSHardwareDownloadMode::Unsynchronized && GSConfig.UseHardwareRenderer())
	{
		if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
			GSReadLocalMemoryUnsync(mem, qwc, vif1.BITBLTBUF._u64, vif1.TRXPOS._u64, vif1.TRXREG._u64);
		else
			memset(mem, 0, qwc * 16);

		return;
	}

	PacketTagType& tag          = (PacketTagType&)m_Ring[s_WritePos];

	tag.command                 = GS_RINGTYPE_INIT_AND_READ_FIFO;
	tag.data[0]                 = qwc;
	tag.pointer                 = (uptr)mem;

	s_WritePos = (s_WritePos + 1) & RINGBUFFERMASK;
	++s_CopyDataTally;
	WaitGS(false, false);
}

bool MTGS::TryOpenGS(void)
{
	s_thread = std::this_thread::get_id();

	GSopen(EmuConfig.GS, EmuConfig.GS.Renderer, hw_render.context_type, PS2MEM_GS);

	s_open_flag = true;
	// notify emu thread that we finished opening (or failed)
	s_open_or_close_done.Post();
	return true;
}

void MTGS::MainLoop(bool flush_all)
{
	// Threading info: run in MTGS thread
	// s_ReadPos is only update by the MTGS thread so it is safe to load it with a relaxed atomic

	std::unique_lock mtvu_lock(s_mtx_RingBufferBusy2);

	for (;;)
	{
		if (flush_all)
		{
			if(!s_sem_event.CheckForWork())
				return;
		}
		else
		{
			mtvu_lock.unlock();
			s_sem_event.WaitForWork();
			mtvu_lock.lock();
		}

		if (!s_open_flag)
			break;

		// note: s_ReadPos is intentionally not volatile, because it should only
		// ever be modified by this thread.
		while (s_ReadPos != s_WritePos)
		{
			const PacketTagType& tag = (PacketTagType&)m_Ring[s_ReadPos];

			switch (tag.command)
			{
				case GS_RINGTYPE_GSPACKET:
				{
					Gif_Path& path = gifUnit.gifPath[tag.data[2]];
					u32 offset     = tag.data[0];
					u32 size       = tag.data[1];
					if (offset != ~0u)
						GSgifTransfer((u8*)&path.buffer[offset], size / 16);
					path.readAmount.fetch_sub(size, std::memory_order_acq_rel);
				}
					break;

				case GS_RINGTYPE_MTVU_GSPACKET:
				{
					if (!vu1Thread.semaXGkick.TryWait())
					{
						mtvu_lock.unlock();
						// Wait for MTVU to complete vu1 program
						vu1Thread.semaXGkick.Wait();
						mtvu_lock.lock();
					}
					Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
					GS_Packet gsPack = path.GetGSPacketMTVU(); // Get vu1 program's xgkick packet(s)
					if (gsPack.size)
						GSgifTransfer((u8*)&path.buffer[gsPack.offset], gsPack.size / 16);
					path.readAmount.fetch_sub(gsPack.size + gsPack.readAmount, std::memory_order_acq_rel);
					path.PopGSPacketMTVU(); // Should be done last, for proper Gif_MTGS_Wait()
				}
					break;
				case GS_RINGTYPE_VSYNC:
					// CSR & 0x2000; is the pageflip id.
					if(!flush_all)
						GSvsync((((u32&)PS2MEM_GS[0x1000]) & 0x2000) ? 0 : 1, s_GSRegistersWritten);
					s_GSRegistersWritten = false;

					s_QueuedFrameCount--;
					if (s_VsyncSignalListener)
					{
						s_sem_Vsync.Post();
						s_VsyncSignalListener = false;
					}
					s_ReadPos = (s_ReadPos + 1) & RINGBUFFERMASK;
					if (!flush_all)
						s_sem_event.NotifyOfWork();
					return;
				case GS_RINGTYPE_ASYNC_CALL:
				{
					AsyncCallType* const func = (AsyncCallType*)tag.pointer;
					(*func)();
					delete func;
				}
					break;
				case GS_RINGTYPE_FREEZE:
				{
					MTGS_FreezeData* data = (MTGS_FreezeData*)tag.pointer;
					int mode = tag.data[0];
					GSfreeze((FreezeAction)mode, (freezeData*)data->fdata);
				}
					break;
				case GS_RINGTYPE_RESET:
					GSreset(tag.data[0] != 0);
					break;

				case GS_RINGTYPE_SOFTRESET:
					GSgifSoftReset(tag.data[0]);
					break;
				case GS_RINGTYPE_INIT_AND_READ_FIFO:
					GSInitAndReadFIFO((u8*)tag.pointer, tag.data[0]);
					break;
				// Optimized performance in non-Dev builds.
				default:
					break;
			}

			s_ReadPos = (s_ReadPos + 1) & RINGBUFFERMASK;
		}
	}

	// Unblock any threads in WaitGS in case MTGS gets cancelled while still processing work
	s_ReadPos = s_WritePos;
	s_sem_event.Kill();
}

void MTGS::CloseGS(void)
{
	if (s_VsyncSignalListener)
	{
		s_VsyncSignalListener = false;
		s_sem_Vsync.Post();
	}
	GSclose();
	s_open_flag = false;
	s_open_or_close_done.Post();
}

// Waits for the GS to empty out the entire ring buffer contents.
// If weakWait, then this function is allowed to exit after MTGS finished a path1 packet
// If isMTVU, then this implies this function is being called from the MTVU thread...
void MTGS::WaitGS(bool weakWait, bool isMTVU)
{
	if(std::this_thread::get_id() == s_thread)
	{
		MainLoop(true);
		return;
	}
	if (!s_open_flag) /* WaitGS issued on a closed thread! */
		return;

	SetEvent();
	if (weakWait && isMTVU)
	{
		Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];

		// On weakWait we will stop waiting on the MTGS thread if the
		// MTGS thread has processed a vu1 xgkick packet, or is pending on
		// its final vu1 xgkick packet (!curP1Packs)...
		// Note: s_WritePos doesn't seem to have proper atomic write
		// code, so reading it from the MTVU thread might be dangerous;
		// hence it has been avoided...
		u32 startP1Packs = path.GetPendingGSPackets();
		if (startP1Packs)
		{
			for (;;)
			{
				s_mtx_RingBufferBusy2.lock();
				s_mtx_RingBufferBusy2.unlock();
				if (path.GetPendingGSPackets() != startP1Packs)
					break;
			}
		}
	}
	else
	{
		/* if it returns false, MTGS thread died */
		if (!s_sem_event.WaitForEmpty()) { }
	}
}

// Sets the gsEvent flag and releases a timeslice.
// For use in loops that wait on the GS thread to do certain things.
void MTGS::SetEvent()
{
	s_sem_event.NotifyOfWork();
	s_CopyDataTally = 0;
}

void MTGS::WaitForClose()
{
	// and kick the thread if it's sleeping
	s_sem_event.NotifyOfWork();

	// and wait for it to finish up..
	s_open_or_close_done.Wait();

	s_thread = {};
}

void MTGS::Freeze(FreezeAction mode, MTGS_FreezeData& data)
{
	PacketTagType& tag          = (PacketTagType&)m_Ring[s_WritePos];

	tag.command                 = GS_RINGTYPE_FREEZE;
	tag.data[0]                 = (int)mode;
	tag.pointer                 = (uptr)&data;

	s_WritePos = (s_WritePos + 1) & RINGBUFFERMASK;
	++s_CopyDataTally;
	WaitGS(false, false);
}

void MTGS::RunOnGSThread(AsyncCallType func)
{
	PacketTagType& tag          = (PacketTagType&)m_Ring[s_WritePos];

	tag.command                 = GS_RINGTYPE_ASYNC_CALL;
	tag.data[0]                 = 0;
	tag.pointer                 = (uptr)new AsyncCallType(std::move(func));

	s_WritePos = (s_WritePos + 1) & RINGBUFFERMASK;
	++s_CopyDataTally;

	// wake the gs thread in case it's sleeping
	SetEvent();
}

void MTGS::GameChanged()
{
	RunOnGSThread(GSGameChanged);
}

void MTGS::ApplySettings()
{
	RunOnGSThread([opts = EmuConfig.GS]() {
		GSUpdateConfig(opts, hw_render.context_type);
	});

	// We need to synchronize the thread when changing any settings when the download mode
	// is unsynchronized, because otherwise we might potentially read in the middle of
	// the GS renderer being reopened.
	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false, false);
}

void MTGS::SwitchRenderer(GSRendererType renderer, GSInterlaceMode interlace)
{
	RunOnGSThread([renderer, interlace]() {
		GSSwitchRenderer(renderer, hw_render.context_type, interlace);
	});

	// See note in ApplySettings() for reasoning here.
	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false, false);
}

void MTGS::SetSoftwareRendering(bool software)
{
	// for hardware, use the chosen api in the base config, or auto if base is set to sw
	GSRendererType new_renderer;
	if (!software)
		new_renderer = EmuConfig.GS.UseHardwareRenderer() ? EmuConfig.GS.Renderer : GSRendererType::Auto;
	else
		new_renderer = GSRendererType::SW;

	SwitchRenderer(new_renderer, EmuConfig.GS.InterlaceMode);
}

void MTGS::ToggleSoftwareRendering()
{
	// reading from the GS thread.. but should be okay here
	SetSoftwareRendering(GSConfig.Renderer != GSRendererType::SW);
}

// Used in MTVU mode... MTVU will later complete a real packet
void Gif_AddGSPacketMTVU(GIF_PATH _path)
{
	PacketTagType& tag          = (PacketTagType&)m_Ring[MTGS::s_WritePos];

	tag.command                 = GS_RINGTYPE_MTVU_GSPACKET;
	tag.data[0]                 = 0;
	tag.data[1]                 = (int)0;
	tag.data[2]                 = (int)_path;

	MTGS::s_WritePos = (MTGS::s_WritePos + 1) & RINGBUFFERMASK;
	++MTGS::s_CopyDataTally;
	if (MTGS::s_CopyDataTally > 0x2000)
		MTGS::SetEvent();
}

void Gif_AddCompletedGSPacket(GS_Packet& _gsPack, GIF_PATH _path)
{
	gifUnit.gifPath[_path].readAmount.fetch_add(_gsPack.size);
	PacketTagType& tag          = (PacketTagType&)m_Ring[MTGS::s_WritePos];

	tag.command                 = GS_RINGTYPE_GSPACKET;
	tag.data[0]                 = (int)_gsPack.offset;
	tag.data[1]                 = (int)_gsPack.size;
	tag.data[2]                 = (int)_path;

	MTGS::s_WritePos = (MTGS::s_WritePos + 1) & RINGBUFFERMASK;
	++MTGS::s_CopyDataTally;
	MTGS::s_CopyDataTally += _gsPack.size / 16;
	if (MTGS::s_CopyDataTally > 0x2000)
		MTGS::SetEvent();
}

void Gif_AddBlankGSPacket(u32 _size, GIF_PATH _path)
{
	gifUnit.gifPath[_path].readAmount.fetch_add(_size);
	PacketTagType& tag          = (PacketTagType&)m_Ring[MTGS::s_WritePos];

	tag.command                 = GS_RINGTYPE_GSPACKET;
	tag.data[0]                 = (int)~0u;
	tag.data[1]                 = (int)_size;
	tag.data[2]                 = (int)_path;

	MTGS::s_WritePos = (MTGS::s_WritePos + 1) & RINGBUFFERMASK;
	if (MTGS::s_CopyDataTally > 0x2000)
		MTGS::SetEvent();
}

