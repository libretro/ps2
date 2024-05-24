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

struct MTGS_BufferedData
{
	u128 m_Ring[RINGBUFFERSIZE];
};

// =====================================================================================================
//  MTGS Threaded Class Implementation
// =====================================================================================================

alignas(32) MTGS_BufferedData RingBuffer;

extern struct retro_hw_render_callback hw_render;

namespace MTGS
{
	static void SetEvent();
	static void GenericStall();

	static void SendSimplePacket(MTGS_RingCommand type, int data0, int data1, int data2);

	// note: when s_ReadPos == s_WritePos, the fifo is empty
	// Threading info: s_ReadPos is updated by the MTGS thread. s_WritePos is updated by the EE thread
	static std::atomic<unsigned int> s_ReadPos      = 0; // cur pos gs is reading from
	static std::atomic<unsigned int> s_WritePos     = 0; // cur pos ee thread is writing to

	static std::atomic<bool> s_SignalRingEnable     = false;
	static std::atomic<int> s_SignalRingPosition    = 0;

	static std::atomic<int> s_QueuedFrameCount      = 0;
	static std::atomic<bool> s_VsyncSignalListener  = false;

	static std::mutex s_mtx_RingBufferBusy2; // Gets released on semaXGkick waiting...
	static Threading::WorkSema s_sem_event;
	static Threading::UserspaceSemaphore s_sem_OnRingReset;
	static Threading::UserspaceSemaphore s_sem_Vsync;

	// Used to delay the sending of events.  Performance is better if the ringbuffer
	// has more than one command in it when the thread is kicked.
	static int s_CopyDataTally                      = 0;

	static std::thread::id s_thread;
	static Threading::ThreadHandle s_thread_handle;
	static std::atomic_bool s_open_flag{false};
	static Threading::UserspaceSemaphore s_open_or_close_done;
};

const Threading::ThreadHandle& MTGS::GetThreadHandle() { return s_thread_handle; }
bool MTGS::IsOpen() { return s_open_flag.load(std::memory_order_acquire); }

void MTGS::ResetGS(bool hardware_reset)
{
	// MTGS Reset process:
	//  * clear the ringbuffer.
	//  * Signal a reset.
	//  * clear the path and byRegs structs (used by GIFtagDummy)
	if (hardware_reset)
	{
		s_ReadPos             = s_WritePos.load();
		s_QueuedFrameCount    = 0;
		s_VsyncSignalListener = 0;
	}

	SendSimplePacket(GS_RINGTYPE_RESET, static_cast<int>(hardware_reset), 0, 0);

	if (hardware_reset)
		SetEvent();
}

void MTGS::PostVsyncStart()
{
	GenericStall();

	// Command qword: Low word is the command, and the high word is the packet
	// length in SIMDs (128 bits).
	const unsigned int writepos       = s_WritePos.load(std::memory_order_relaxed);

	PacketTagType& tag                = (PacketTagType&)RingBuffer.m_Ring[writepos];
	tag.command                       = GS_RINGTYPE_VSYNC;
	tag.data[0]                       = 0;

	s_WritePos.store((writepos + 1) & RINGBUFFERMASK, std::memory_order_release);
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

	if ((s_QueuedFrameCount.fetch_add(1) < EmuConfig.GS.VsyncQueueSize))
		return;

	s_VsyncSignalListener.store(true, std::memory_order_release);

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

	GenericStall();
	const unsigned int writepos = s_WritePos.load(std::memory_order_relaxed);
	PacketTagType& tag          = (PacketTagType&)RingBuffer.m_Ring[writepos];

	tag.command                 = GS_RINGTYPE_INIT_AND_READ_FIFO;
	tag.data[0]                 = qwc;
	tag.pointer                 = (uptr)mem;

	s_WritePos.store((writepos + 1) & RINGBUFFERMASK, std::memory_order_release);
	++s_CopyDataTally;
	WaitGS(false, false);
}

bool MTGS::TryOpenGS(void)
{
	s_thread = std::this_thread::get_id();

	GSopen(EmuConfig.GS, EmuConfig.GS.Renderer, hw_render.context_type, PS2MEM_GS);

	s_open_flag.store(true, std::memory_order_release);
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

		if (!s_open_flag.load(std::memory_order_acquire))
			break;

		// note: s_ReadPos is intentionally not volatile, because it should only
		// ever be modified by this thread.
		while (s_ReadPos.load(std::memory_order_relaxed) != s_WritePos.load(std::memory_order_acquire))
		{
			const unsigned int local_ReadPos = s_ReadPos.load(std::memory_order_relaxed);
			const PacketTagType& tag = (PacketTagType&)RingBuffer.m_Ring[local_ReadPos];

			switch (tag.command)
			{
				case GS_RINGTYPE_GSPACKET:
				{
					Gif_Path& path = gifUnit.gifPath[tag.data[2]];
					u32 offset = tag.data[0];
					u32 size = tag.data[1];
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

					s_QueuedFrameCount.fetch_sub(1);
					if (s_VsyncSignalListener.exchange(false))
						s_sem_Vsync.Post();
					break;
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

			uint newringpos = (local_ReadPos + 1) & RINGBUFFERMASK;

			s_ReadPos.store(newringpos, std::memory_order_release);

			if(!flush_all && tag.command == GS_RINGTYPE_VSYNC) {
				s_sem_event.NotifyOfWork();
				return;
			}

			if (s_SignalRingEnable.load(std::memory_order_acquire))
			{
				// The EEcore has requested a signal after some amount of processed data.
				if (s_SignalRingPosition.fetch_sub(1) <= 0)
				{
					// Make sure to post the signal after the m_ReadPos has been updated...
					s_SignalRingEnable.store(false, std::memory_order_release);
					s_sem_OnRingReset.Post();
					continue;
				}
			}
		}

		// TODO: With the new race-free WorkSema do we still need these?

		// Safety valve in case standard signals fail for some reason -- this ensures the EEcore
		// won't sleep the eternity, even if SignalRingPosition didn't reach 0 for some reason.
		// Important: Need to unlock the MTGS busy signal PRIOR, so that EEcore SetEvent() calls
		// parallel to this handler aren't accidentally blocked.
		if (s_SignalRingEnable.exchange(false))
		{
			s_SignalRingPosition.store(0, std::memory_order_release);
			s_sem_OnRingReset.Post();
		}

		if (s_VsyncSignalListener.exchange(false))
			s_sem_Vsync.Post();
	}

	// Unblock any threads in WaitGS in case MTGS gets cancelled while still processing work
	s_ReadPos.store(s_WritePos.load(std::memory_order_acquire), std::memory_order_relaxed);
	s_sem_event.Kill();
}

void MTGS::CloseGS(void)
{
	if( s_SignalRingEnable.exchange(false) )
	{
		s_SignalRingPosition.store(0, std::memory_order_release);
		s_sem_OnRingReset.Post();
	}
	if (s_VsyncSignalListener.exchange(false))
		s_sem_Vsync.Post();
	GSclose();
	s_open_flag.store(false, std::memory_order_release);
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
	if (!IsOpen()) /* WaitGS issued on a closed thread! */
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

void MTGS::GenericStall()
{
	const uint size     = 1;
	// Note on volatiles: s_WritePos is not modified by the GS thread, so there's no need
	// to use volatile reads here.  We do cache it though, since we know it never changes,
	// except for calls to RingbufferRestert() -- handled below.
	const uint writepos = s_WritePos.load(std::memory_order_relaxed);

	// generic gs wait/stall.
	// if the writepos is past the readpos then we're safe.
	// But if not then we need to make sure the readpos is outside the scope of
	// the block about to be written (writepos + size)

	uint readpos = s_ReadPos.load(std::memory_order_acquire);
	uint freeroom;

	if (writepos < readpos)
		freeroom = readpos - writepos;
	else
		freeroom = RINGBUFFERSIZE - (writepos - readpos);

	if (freeroom <= size)
	{
		// writepos will overlap readpos if we commit the data, so we need to wait until
		// readpos is out past the end of the future write pos, or until it wraps around
		// (in which case writepos will be >= readpos).

		// Ideally though we want to wait longer, because if we just toss in this packet
		// the next packet will likely stall up too.  So lets set a condition for the MTGS
		// thread to wake up the EE once there's a sizable chunk of the ringbuffer emptied.

		uint somedone = (RINGBUFFERSIZE - freeroom) / 4;
		if (somedone < size + 1)
			somedone = size + 1;

		// FMV Optimization: FMVs typically send *very* little data to the GS, in some cases
		// every other frame is nothing more than a page swap.  Sleeping the EEcore is a
		// waste of time, and we get better results using a spinwait.

		if (somedone > 0x80)
		{
			s_SignalRingPosition.store(somedone, std::memory_order_release);

			for (;;)
			{
				s_SignalRingEnable.store(true, std::memory_order_release);
				SetEvent();
				s_sem_OnRingReset.Wait();
				readpos = s_ReadPos.load(std::memory_order_acquire);
				if (writepos < readpos)
					freeroom = readpos - writepos;
				else
					freeroom = RINGBUFFERSIZE - (writepos - readpos);

				if (freeroom > size)
					break;
			}
		}
		else
		{
			SetEvent();
			for (;;)
			{
				readpos = s_ReadPos.load(std::memory_order_acquire);

				if (writepos < readpos)
					freeroom = readpos - writepos;
				else
					freeroom = RINGBUFFERSIZE - (writepos - readpos);

				if (freeroom > size)
					break;
			}
		}
	}
}

void MTGS::SendSimplePacket(MTGS_RingCommand type, int data0, int data1, int data2)
{
	GenericStall();
	const unsigned int writepos = s_WritePos.load(std::memory_order_relaxed);
	PacketTagType& tag          = (PacketTagType&)RingBuffer.m_Ring[writepos];

	tag.command                 = type;
	tag.data[0]                 = data0;
	tag.data[1]                 = data1;
	tag.data[2]                 = data2;

	s_WritePos.store((writepos + 1) & RINGBUFFERMASK, std::memory_order_release);
	++s_CopyDataTally;
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
	GenericStall();
	const unsigned int writepos = s_WritePos.load(std::memory_order_relaxed);
	PacketTagType& tag          = (PacketTagType&)RingBuffer.m_Ring[writepos];

	tag.command                 = GS_RINGTYPE_FREEZE;
	tag.data[0]                 = (int)mode;
	tag.pointer                 = (uptr)&data;

	s_WritePos.store((writepos + 1) & RINGBUFFERMASK, std::memory_order_release);
	++s_CopyDataTally;
	WaitGS(false, false);
}

void MTGS::RunOnGSThread(AsyncCallType func)
{
	GenericStall();
	const unsigned int writepos = s_WritePos.load(std::memory_order_relaxed);
	PacketTagType& tag          = (PacketTagType&)RingBuffer.m_Ring[writepos];

	tag.command                 = GS_RINGTYPE_ASYNC_CALL;
	tag.data[0]                 = 0;
	tag.pointer                 = (uptr)new AsyncCallType(std::move(func));

	s_WritePos.store((writepos + 1) & RINGBUFFERMASK, std::memory_order_release);
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
void Gif_AddGSPacketMTVU(GS_Packet& gsPack, GIF_PATH path)
{
	MTGS::SendSimplePacket(GS_RINGTYPE_MTVU_GSPACKET, 0, (int)0, (int)path);
	if (MTGS::s_CopyDataTally > 0x2000)
		MTGS::SetEvent();
}

void Gif_AddCompletedGSPacket(GS_Packet& gsPack, GIF_PATH path)
{
	gifUnit.gifPath[path].readAmount.fetch_add(gsPack.size);
	MTGS::SendSimplePacket(GS_RINGTYPE_GSPACKET, (int)gsPack.offset, (int)gsPack.size, (int)path);

	MTGS::s_CopyDataTally += gsPack.size / 16;
	if (MTGS::s_CopyDataTally > 0x2000)
		MTGS::SetEvent();
}

void Gif_AddBlankGSPacket(u32 size, GIF_PATH path)
{
	gifUnit.gifPath[path].readAmount.fetch_add(size);
	MTGS::SendSimplePacket(GS_RINGTYPE_GSPACKET, (int)~0u, (int)size, (int)path);

	MTGS::s_CopyDataTally += -1;
	if (MTGS::s_CopyDataTally > 0x2000)
		MTGS::SetEvent();
}

