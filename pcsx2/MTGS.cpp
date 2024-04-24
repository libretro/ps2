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

#include "PrecompiledHeader.h"
#include "Common.h"

#include <cstring>
#include <list>

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

struct RingCmdPacket_Vsync
{
	u8 regset1[0x0f0];
	u32 csr;
	u32 imr;
	GSRegSIGBLID siglblid;

	// must be 16 byte aligned
	u32 registers_written;
	u32 pad[3];
};

struct MTGS_BufferedData
{
	u128 m_Ring[RingBufferSize];
	u8 Regs[Ps2MemSize::GSregs];
};

// =====================================================================================================
//  MTGS Threaded Class Implementation
// =====================================================================================================

alignas(32) MTGS_BufferedData RingBuffer;

namespace MTGS
{
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

	// These vars maintain instance data for sending Data Packets.
	// Only one data packet can be constructed and uploaded at a time.

	static uint s_packet_startpos   = 0; // size of the packet (data only, ie. not including the 16 byte command!)
	static uint s_packet_size       = 0; // size of the packet (data only, ie. not including the 16 byte command!)
	static uint s_packet_writepos   = 0; // index of the data location in the ringbuffer.

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

	s_ReadPos = s_WritePos.load();
	s_QueuedFrameCount = 0;
	s_VsyncSignalListener = 0;

	SendSimplePacket(GS_RINGTYPE_RESET, static_cast<int>(hardware_reset), 0, 0);
	SetEvent();
}

void MTGS::PostVsyncStart(bool registers_written)
{
	// Optimization note: Typically regset1 isn't needed.  The regs in that area are typically
	// changed infrequently, usually during video mode changes.  However, on modern systems the
	// 256-byte copy is only a few dozen cycles -- executed 60 times a second -- so probably
	// not worth the effort or overhead of trying to selectively avoid it.

	uint packsize = sizeof(RingCmdPacket_Vsync) / 16;
	PrepDataPacket(GS_RINGTYPE_VSYNC, packsize);
	MemCopy_WrappedDest((u128*)PS2MEM_GS, RingBuffer.m_Ring, s_packet_writepos, RingBufferSize, 0xf);

	u32* remainder              = (u32*)(u8*)&RingBuffer.m_Ring[s_packet_writepos & RingBufferMask];
	remainder[0]                = GSCSRr;
	remainder[1]                = GSIMR._u32;
	(GSRegSIGBLID&)remainder[2] = GSSIGLBLID;
	remainder[4]                = static_cast<u32>(registers_written);
	s_packet_writepos           = (s_packet_writepos + 2) & RingBufferMask;

	SendDataPacket();

	// Vsyncs should always start the GS thread, regardless of how little has actually be queued.
	if (s_CopyDataTally != 0)
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

	SendPointerPacket(GS_RINGTYPE_INIT_AND_READ_FIFO, qwc, mem);
	WaitGS(false, false, false);
}

bool MTGS::TryOpenGS()
{
	s_thread = std::this_thread::get_id();

	memcpy(RingBuffer.Regs, PS2MEM_GS, sizeof(PS2MEM_GS));

	if (!GSopen(EmuConfig.GS, EmuConfig.GS.Renderer, RingBuffer.Regs))
		return false;

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
			u32 ringposinc = 1;

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
					break;
				}

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
					break;
				}

				default:
				{
					switch (tag.command)
					{
						case GS_RINGTYPE_VSYNC:
						{
							const int qsize = tag.data[0];
							ringposinc += qsize;

							// Mail in the important GS registers.
							// This seemingly obtuse system is needed in order to handle cases where the vsync data wraps
							// around the edge of the ringbuffer.  If not for that I'd just use a struct. >_<

							uint datapos = (local_ReadPos + 1) & RingBufferMask;
							MemCopy_WrappedSrc(RingBuffer.m_Ring, datapos, RingBufferSize, (u128*)RingBuffer.Regs, 0xf);

							u32* remainder = (u32*)&RingBuffer.m_Ring[datapos];
							((u32&)RingBuffer.Regs[0x1000]) = remainder[0];
							((u32&)RingBuffer.Regs[0x1010]) = remainder[1];
							((GSRegSIGBLID&)RingBuffer.Regs[0x1080]) = (GSRegSIGBLID&)remainder[2];

							// CSR & 0x2000; is the pageflip id.
							if(!flush_all)
								GSvsync((((u32&)RingBuffer.Regs[0x1000]) & 0x2000) ? 0 : 1, remainder[4] != 0);

							s_QueuedFrameCount.fetch_sub(1);
							if (s_VsyncSignalListener.exchange(false))
								s_sem_Vsync.Post();

							// Do not StateCheckInThread() here
							// Otherwise we could pause while there's still data in the queue
							// Which could make the MTVU thread wait forever for it to empty
						}
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
							data->retval = GSfreeze((FreezeAction)mode, (freezeData*)data->fdata);
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
				}
			}

			uint newringpos = (s_ReadPos.load(std::memory_order_relaxed) + ringposinc) & RingBufferMask;

			s_ReadPos.store(newringpos, std::memory_order_release);

			if(!flush_all && tag.command == GS_RINGTYPE_VSYNC) {
				s_sem_event.NotifyOfWork();
				return;
			}

			if (s_SignalRingEnable.load(std::memory_order_acquire))
			{
				// The EEcore has requested a signal after some amount of processed data.
				if (s_SignalRingPosition.fetch_sub(ringposinc) <= 0)
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
			//Console.Warning( "(MTGS Thread) Dangling RingSignal on empty buffer!  signalpos=0x%06x", s_SignalRingPosition.exchange(0) ) );
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

void MTGS::CloseGS()
{
	if( s_SignalRingEnable.exchange(false) )
	{
		//Console.Warning( "(MTGS Thread) Dangling RingSignal on empty buffer!  signalpos=0x%06x", s_SignalRingPosition.exchange(0) ) );
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
// If syncRegs, then writes pcsx2's gs regs to MTGS's internal copy
// If weakWait, then this function is allowed to exit after MTGS finished a path1 packet
// If isMTVU, then this implies this function is being called from the MTVU thread...
void MTGS::WaitGS(bool syncRegs, bool weakWait, bool isMTVU)
{
	if(std::this_thread::get_id() == s_thread)
	{
		MainLoop(true);
		return;
	}
	if (!IsOpen())
	{
		Console.Error("MTGS Warning!  WaitGS issued on a closed thread.");
		return;
	}

	Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];

	// Both s_ReadPos and s_WritePos can be relaxed as we only want to test if the queue is empty but
	// we don't want to access the content of the queue

	SetEvent();
	if (weakWait && isMTVU)
	{
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
		if (!s_sem_event.WaitForEmpty())
			Console.Error("MTGS Thread Died");
	}

	// Completely synchronize GS and MTGS register states.
	if (syncRegs)
		memcpy(RingBuffer.Regs, PS2MEM_GS, sizeof(RingBuffer.Regs));
}

// Sets the gsEvent flag and releases a timeslice.
// For use in loops that wait on the GS thread to do certain things.
void MTGS::SetEvent()
{
	s_sem_event.NotifyOfWork();
	s_CopyDataTally = 0;
}

// Closes the data packet send command, and initiates the gs thread (if needed).
void MTGS::SendDataPacket()
{
	uint actualSize    = ((s_packet_writepos - s_packet_startpos) & RingBufferMask) - 1;
	PacketTagType& tag = (PacketTagType&)RingBuffer.m_Ring[s_packet_startpos];
	tag.data[0]        = actualSize;

	s_WritePos.store(s_packet_writepos, std::memory_order_release);

	s_CopyDataTally += s_packet_size;
	if (s_CopyDataTally > 0x2000)
		SetEvent();

	s_packet_size = 0;
}

void MTGS::GenericStall(uint size)
{
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
		freeroom = RingBufferSize - (writepos - readpos);

	if (freeroom <= size)
	{
		// writepos will overlap readpos if we commit the data, so we need to wait until
		// readpos is out past the end of the future write pos, or until it wraps around
		// (in which case writepos will be >= readpos).

		// Ideally though we want to wait longer, because if we just toss in this packet
		// the next packet will likely stall up too.  So lets set a condition for the MTGS
		// thread to wake up the EE once there's a sizable chunk of the ringbuffer emptied.

		uint somedone = (RingBufferSize - freeroom) / 4;
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
					freeroom = RingBufferSize - (writepos - readpos);

				if (freeroom > size)
					break;
			}
		}
		else
		{
			SetEvent();
			for (;;)
			{
				Threading::SpinWait();
				readpos = s_ReadPos.load(std::memory_order_acquire);

				if (writepos < readpos)
					freeroom = readpos - writepos;
				else
					freeroom = RingBufferSize - (writepos - readpos);

				if (freeroom > size)
					break;
			}
		}
	}
}

void MTGS::PrepDataPacket(MTGS_RingCommand cmd, u32 size)
{
	s_packet_size = size;
	++size; // takes into account our RingCommand QWC.
	GenericStall(size);

	// Command qword: Low word is the command, and the high word is the packet
	// length in SIMDs (128 bits).
	const unsigned int local_WritePos = s_WritePos.load(std::memory_order_relaxed);

	PacketTagType& tag = (PacketTagType&)RingBuffer.m_Ring[local_WritePos];
	tag.command        = cmd;
	tag.data[0]        = s_packet_size;
	s_packet_startpos  = local_WritePos;
	s_packet_writepos  = (local_WritePos + 1) & RingBufferMask;
}

void MTGS::SendSimplePacket(MTGS_RingCommand type, int data0, int data1, int data2)
{
	GenericStall(1);
	PacketTagType& tag   = (PacketTagType&)RingBuffer.m_Ring[s_WritePos.load(std::memory_order_relaxed)];

	tag.command          = type;
	tag.data[0]          = data0;
	tag.data[1]          = data1;
	tag.data[2]          = data2;

	uint future_writepos = (s_WritePos.load(std::memory_order_relaxed) + 1) & RingBufferMask;
	s_WritePos.store(future_writepos, std::memory_order_release);

	++s_CopyDataTally;
}

void MTGS::SendSimpleGSPacket(MTGS_RingCommand type, u32 offset, u32 size, GIF_PATH path)
{
	SendSimplePacket(type, (int)offset, (int)size, (int)path);

	s_CopyDataTally += size / 16;
	if (s_CopyDataTally > 0x2000)
		SetEvent();
}

void MTGS::SendPointerPacket(MTGS_RingCommand type, u32 data0, void* data1)
{
	GenericStall(1);
	PacketTagType& tag   = (PacketTagType&)RingBuffer.m_Ring[s_WritePos.load(std::memory_order_relaxed)];

	tag.command          = type;
	tag.data[0]          = data0;
	tag.pointer          = (uptr)data1;

	uint future_writepos = (s_WritePos.load(std::memory_order_relaxed) + 1) & RingBufferMask;
	s_WritePos.store(future_writepos, std::memory_order_release);

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
	// synchronize regs before loading
	if (mode == FreezeAction::Load)
		WaitGS(true);

	SendPointerPacket(GS_RINGTYPE_FREEZE, (int)mode, &data);
	WaitGS(false);
}

void MTGS::RunOnGSThread(AsyncCallType func)
{
	SendPointerPacket(GS_RINGTYPE_ASYNC_CALL, 0, new AsyncCallType(std::move(func)));

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
		GSUpdateConfig(opts);
	});

	// We need to synchronize the thread when changing any settings when the download mode
	// is unsynchronized, because otherwise we might potentially read in the middle of
	// the GS renderer being reopened.
	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false, false, false);
}

void MTGS::SwitchRenderer(GSRendererType renderer, bool display_message /* = true */)
{
	RunOnGSThread([renderer]() {
		GSSwitchRenderer(renderer);
	});

	// See note in ApplySettings() for reasoning here.
	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false, false, false);
}

void MTGS::SetSoftwareRendering(bool software, bool display_message /* = true */)
{
	// for hardware, use the chosen api in the base config, or auto if base is set to sw
	GSRendererType new_renderer;
	if (!software)
		new_renderer = EmuConfig.GS.UseHardwareRenderer() ? EmuConfig.GS.Renderer : GSRendererType::Auto;
	else
		new_renderer = GSRendererType::SW;

	SwitchRenderer(new_renderer, display_message);
}

void MTGS::ToggleSoftwareRendering()
{
	// reading from the GS thread.. but should be okay here
	SetSoftwareRendering(GSConfig.Renderer != GSRendererType::SW);
}

// Used in MTVU mode... MTVU will later complete a real packet
void Gif_AddGSPacketMTVU(GS_Packet& gsPack, GIF_PATH path)
{
	MTGS::SendSimpleGSPacket(GS_RINGTYPE_MTVU_GSPACKET, 0, 0, path);
}

void Gif_AddCompletedGSPacket(GS_Packet& gsPack, GIF_PATH path)
{
	gifUnit.gifPath[path].readAmount.fetch_add(gsPack.size);
	MTGS::SendSimpleGSPacket(GS_RINGTYPE_GSPACKET, gsPack.offset, gsPack.size, path);
}

void Gif_AddBlankGSPacket(u32 size, GIF_PATH path)
{
	gifUnit.gifPath[path].readAmount.fetch_add(size);
	MTGS::SendSimpleGSPacket(GS_RINGTYPE_GSPACKET, ~0u, size, path);
}

