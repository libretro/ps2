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

#include "common/ScopedGuard.h"
#include "common/StringUtil.h"

#include "GS.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "Elfheader.h"

#include "Host.h"
#include "IconsFontAwesome5.h"
#include "VMManager.h"

using namespace Threading;

// =====================================================================================================
//  MTGS Threaded Class Implementation
// =====================================================================================================

alignas(32) MTGS_BufferedData RingBuffer;

SysMtgsThread::SysMtgsThread()
{
	m_ReadPos = 0;
	m_WritePos = 0;
	m_packet_size = 0;
	m_packet_writepos = 0;

	m_QueuedFrameCount = 0;
	m_VsyncSignalListener = false;
	m_SignalRingEnable = false;
	m_SignalRingPosition = 0;

	m_CopyDataTally = 0;
}

SysMtgsThread::~SysMtgsThread()
{
	ShutdownThread();
}

void SysMtgsThread::StartThread()
{
	if (m_thread != std::thread::id())
		return;

	m_sem_event.Reset();
	m_shutdown_flag.store(false, std::memory_order_release);
	GSinit();
}

void SysMtgsThread::ShutdownThread()
{
	if (m_thread == std::thread::id())
		return;

	// just go straight to shutdown, don't wait-for-open again
	m_shutdown_flag.store(true, std::memory_order_release);
	if (IsOpen())
		WaitForClose();

	// make sure the thread actually exits
	m_sem_event.NotifyOfWork();
	m_thread = {};
}

void SysMtgsThread::ThreadEntryPoint()
{
	if (GSinit() != 0)
	{
		Host::ReportErrorAsync("Error", "GSinit() failed.");
		m_open_or_close_done.Post();
		return;
	}

	m_thread_handle = Threading::ThreadHandle::GetForCallingThread();

	for (;;)
	{
		// wait until we're actually asked to initialize (and config has been loaded, etc)
		while (!m_open_flag.load(std::memory_order_acquire))
		{
			if (m_shutdown_flag.load(std::memory_order_acquire))
			{
				m_sem_event.Kill();
				m_thread_handle = {};
				return;
			}

			m_sem_event.WaitForWork();
		}

		// try initializing.. this could fail
		const bool opened = TryOpenGS();
		m_open_flag.store(opened, std::memory_order_release);

		// notify emu thread that we finished opening (or failed)
		m_open_or_close_done.Post();

		// are we open?
		if (!opened)
		{
			// wait until we're asked to try again...
			continue;
		}
		// when we come back here, it's because we closed (or shutdown)
		// that means the emu thread should be blocked, waiting for us to be done
		CloseGS();
		m_open_or_close_done.Post();

		// we need to reset sem_event here, because MainLoop() kills it.
		m_sem_event.Reset();
	}

	GSshutdown();
}

void SysMtgsThread::ResetGS(bool hardware_reset)
{
	// MTGS Reset process:
	//  * clear the ringbuffer.
	//  * Signal a reset.
	//  * clear the path and byRegs structs (used by GIFtagDummy)

	m_ReadPos = m_WritePos.load();
	m_QueuedFrameCount = 0;
	m_VsyncSignalListener = 0;

	SendSimplePacket(GS_RINGTYPE_RESET, static_cast<int>(hardware_reset), 0, 0);
	SetEvent();
}

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

void SysMtgsThread::PostVsyncStart(bool registers_written)
{
	// Optimization note: Typically regset1 isn't needed.  The regs in that area are typically
	// changed infrequently, usually during video mode changes.  However, on modern systems the
	// 256-byte copy is only a few dozen cycles -- executed 60 times a second -- so probably
	// not worth the effort or overhead of trying to selectively avoid it.

	uint packsize = sizeof(RingCmdPacket_Vsync) / 16;
	PrepDataPacket(GS_RINGTYPE_VSYNC, packsize);
	MemCopy_WrappedDest((u128*)PS2MEM_GS, RingBuffer.m_Ring, m_packet_writepos, RingBufferSize, 0xf);

	u32* remainder = (u32*)GetDataPacketPtr();
	remainder[0] = GSCSRr;
	remainder[1] = GSIMR._u32;
	(GSRegSIGBLID&)remainder[2] = GSSIGLBLID;
	remainder[4] = static_cast<u32>(registers_written);
	m_packet_writepos = (m_packet_writepos + 2) & RingBufferMask;

	SendDataPacket();

	// Vsyncs should always start the GS thread, regardless of how little has actually be queued.
	if (m_CopyDataTally != 0)
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

	if ((m_QueuedFrameCount.fetch_add(1) < EmuConfig.GS.VsyncQueueSize) /*|| (!EmuConfig.GS.VsyncEnable && !EmuConfig.GS.FrameLimitEnable)*/)
		return;

	m_VsyncSignalListener.store(true, std::memory_order_release);

	m_sem_Vsync.Wait();
}

void SysMtgsThread::InitAndReadFIFO(u8* mem, u32 qwc)
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

bool SysMtgsThread::TryOpenGS()
{
	if(IsOpen())
		return true;

	m_thread = std::this_thread::get_id();

	memcpy(RingBuffer.Regs, PS2MEM_GS, sizeof(PS2MEM_GS));

	if (!GSopen(EmuConfig.GS, EmuConfig.GS.Renderer, RingBuffer.Regs))
		return false;

	GSSetGameCRC(ElfCRC);
	m_open_flag.store(true, std::memory_order_release);
	// notify emu thread that we finished opening (or failed)
	m_open_or_close_done.Post();
	return true;
}

void SysMtgsThread::MainLoop(bool flush_all)
{
	// Threading info: run in MTGS thread
	// m_ReadPos is only update by the MTGS thread so it is safe to load it with a relaxed atomic

	std::unique_lock mtvu_lock(m_mtx_RingBufferBusy2);

	for (;;)
	{
		if (flush_all)
		{
			if(!m_sem_event.CheckForWork())
				return;
		}
		else
		if (m_run_idle_flag.load(std::memory_order_acquire) && VMManager::GetState() != VMState::Running)
		{
			if (!m_sem_event.CheckForWork())
				GSPresentCurrentFrame();
		}
		else
		{
			mtvu_lock.unlock();
			m_sem_event.WaitForWork();
			mtvu_lock.lock();
		}

		if (!m_open_flag.load(std::memory_order_acquire))
			break;

		// note: m_ReadPos is intentionally not volatile, because it should only
		// ever be modified by this thread.
		while (m_ReadPos.load(std::memory_order_relaxed) != m_WritePos.load(std::memory_order_acquire))
		{
			const unsigned int local_ReadPos = m_ReadPos.load(std::memory_order_relaxed);
			const PacketTagType& tag = (PacketTagType&)RingBuffer[local_ReadPos];
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

							u32* remainder = (u32*)&RingBuffer[datapos];
							((u32&)RingBuffer.Regs[0x1000]) = remainder[0];
							((u32&)RingBuffer.Regs[0x1010]) = remainder[1];
							((GSRegSIGBLID&)RingBuffer.Regs[0x1080]) = (GSRegSIGBLID&)remainder[2];

							// CSR & 0x2000; is the pageflip id.
							if(!flush_all)
								GSvsync((((u32&)RingBuffer.Regs[0x1000]) & 0x2000) ? 0 : 1, remainder[4] != 0);

							m_QueuedFrameCount.fetch_sub(1);
							if (m_VsyncSignalListener.exchange(false))
								m_sem_Vsync.Post();

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
						{
							int mask = tag.data[0];
							GSgifSoftReset(mask);
						}
						break;

						case GS_RINGTYPE_CRC:
							GSSetGameCRC(tag.data[0]);
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

			uint newringpos = (m_ReadPos.load(std::memory_order_relaxed) + ringposinc) & RingBufferMask;

			m_ReadPos.store(newringpos, std::memory_order_release);

			if(!flush_all && tag.command == GS_RINGTYPE_VSYNC) {
				m_sem_event.NotifyOfWork();
				return;
			}

			if (m_SignalRingEnable.load(std::memory_order_acquire))
			{
				// The EEcore has requested a signal after some amount of processed data.
				if (m_SignalRingPosition.fetch_sub(ringposinc) <= 0)
				{
					// Make sure to post the signal after the m_ReadPos has been updated...
					m_SignalRingEnable.store(false, std::memory_order_release);
					m_sem_OnRingReset.Post();
					continue;
				}
			}
		}

		// TODO: With the new race-free WorkSema do we still need these?

		// Safety valve in case standard signals fail for some reason -- this ensures the EEcore
		// won't sleep the eternity, even if SignalRingPosition didn't reach 0 for some reason.
		// Important: Need to unlock the MTGS busy signal PRIOR, so that EEcore SetEvent() calls
		// parallel to this handler aren't accidentally blocked.
		if (m_SignalRingEnable.exchange(false))
		{
			//Console.Warning( "(MTGS Thread) Dangling RingSignal on empty buffer!  signalpos=0x%06x", m_SignalRingPosition.exchange(0) ) );
			m_SignalRingPosition.store(0, std::memory_order_release);
			m_sem_OnRingReset.Post();
		}

		if (m_VsyncSignalListener.exchange(false))
			m_sem_Vsync.Post();
	}

	// Unblock any threads in WaitGS in case MTGS gets cancelled while still processing work
	m_ReadPos.store(m_WritePos.load(std::memory_order_acquire), std::memory_order_relaxed);
	m_sem_event.Kill();
}

void SysMtgsThread::StepFrame()
{
	MainLoop(false);
}

void SysMtgsThread::Flush()
{
	MainLoop(true);
}

void SysMtgsThread::SignalVsync()
{
	if (m_VsyncSignalListener.exchange(false))
		m_sem_Vsync.Post();
}

void SysMtgsThread::CloseGS()
{
	if( m_SignalRingEnable.exchange(false) )
	{
		//Console.Warning( "(MTGS Thread) Dangling RingSignal on empty buffer!  signalpos=0x%06x", m_SignalRingPosition.exchange(0) ) );
		m_SignalRingPosition.store(0, std::memory_order_release);
		m_sem_OnRingReset.Post();
	}
	if (m_VsyncSignalListener.exchange(false))
		m_sem_Vsync.Post();
	GSclose();
	m_open_flag.store(false, std::memory_order_release);
	m_open_or_close_done.Post();
}

// Waits for the GS to empty out the entire ring buffer contents.
// If syncRegs, then writes pcsx2's gs regs to MTGS's internal copy
// If weakWait, then this function is allowed to exit after MTGS finished a path1 packet
// If isMTVU, then this implies this function is being called from the MTVU thread...
void SysMtgsThread::WaitGS(bool syncRegs, bool weakWait, bool isMTVU)
{
	if(std::this_thread::get_id() == m_thread)
	{
		GetMTGS().Flush();
		return;
	}
	if (!IsOpen())
	{
		Console.Error("MTGS Warning!  WaitGS issued on a closed thread.");
		return;
	}

	Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];

	// Both m_ReadPos and m_WritePos can be relaxed as we only want to test if the queue is empty but
	// we don't want to access the content of the queue

	SetEvent();
	if (weakWait && isMTVU)
	{
		// On weakWait we will stop waiting on the MTGS thread if the
		// MTGS thread has processed a vu1 xgkick packet, or is pending on
		// its final vu1 xgkick packet (!curP1Packs)...
		// Note: m_WritePos doesn't seem to have proper atomic write
		// code, so reading it from the MTVU thread might be dangerous;
		// hence it has been avoided...
		u32 startP1Packs = path.GetPendingGSPackets();
		if (startP1Packs)
		{
			for (;;)
			{
				m_mtx_RingBufferBusy2.lock();
				m_mtx_RingBufferBusy2.unlock();
				if (path.GetPendingGSPackets() != startP1Packs)
					break;
			}
		}
	}
	else
	{
		if (!m_sem_event.WaitForEmpty())
			Console.Error("MTGS Thread Died");
	}

	assert(!(weakWait && syncRegs) && "No synchronization for this!");

	if (syncRegs)
	{
		// Completely synchronize GS and MTGS register states.
		memcpy(RingBuffer.Regs, PS2MEM_GS, sizeof(RingBuffer.Regs));
	}
}

// Sets the gsEvent flag and releases a timeslice.
// For use in loops that wait on the GS thread to do certain things.
void SysMtgsThread::SetEvent()
{
	m_sem_event.NotifyOfWork();
	m_CopyDataTally = 0;
}

u8* SysMtgsThread::GetDataPacketPtr() const
{
	return (u8*)&RingBuffer[m_packet_writepos & RingBufferMask];
}

// Closes the data packet send command, and initiates the gs thread (if needed).
void SysMtgsThread::SendDataPacket()
{
	uint actualSize = ((m_packet_writepos - m_packet_startpos) & RingBufferMask) - 1;
	PacketTagType& tag = (PacketTagType&)RingBuffer[m_packet_startpos];
	tag.data[0] = actualSize;

	m_WritePos.store(m_packet_writepos, std::memory_order_release);

	if (EmuConfig.GS.SynchronousMTGS)
	{
		WaitGS();
	}
	else
	{
		m_CopyDataTally += m_packet_size;
		if (m_CopyDataTally > 0x2000)
			SetEvent();
	}

	m_packet_size = 0;
}

void SysMtgsThread::GenericStall(uint size)
{
	// Note on volatiles: m_WritePos is not modified by the GS thread, so there's no need
	// to use volatile reads here.  We do cache it though, since we know it never changes,
	// except for calls to RingbufferRestert() -- handled below.
	const uint writepos = m_WritePos.load(std::memory_order_relaxed);

	// generic gs wait/stall.
	// if the writepos is past the readpos then we're safe.
	// But if not then we need to make sure the readpos is outside the scope of
	// the block about to be written (writepos + size)

	uint readpos = m_ReadPos.load(std::memory_order_acquire);
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
			m_SignalRingPosition.store(somedone, std::memory_order_release);

			for (;;)
			{
				m_SignalRingEnable.store(true, std::memory_order_release);
				SetEvent();
				m_sem_OnRingReset.Wait();
				readpos = m_ReadPos.load(std::memory_order_acquire);
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
				SpinWait();
				readpos = m_ReadPos.load(std::memory_order_acquire);

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

void SysMtgsThread::PrepDataPacket(MTGS_RingCommand cmd, u32 size)
{
	m_packet_size = size;
	++size; // takes into account our RingCommand QWC.
	GenericStall(size);

	// Command qword: Low word is the command, and the high word is the packet
	// length in SIMDs (128 bits).
	const unsigned int local_WritePos = m_WritePos.load(std::memory_order_relaxed);

	PacketTagType& tag = (PacketTagType&)RingBuffer[local_WritePos];
	tag.command = cmd;
	tag.data[0] = m_packet_size;
	m_packet_startpos = local_WritePos;
	m_packet_writepos = (local_WritePos + 1) & RingBufferMask;
}

// Returns the amount of giftag data processed (in simd128 values).
// Return value is used by VU1's XGKICK instruction to wrap the data
// around VU memory instead of having buffer overflow...
// Parameters:
//  size - size of the packet data, in smd128's
void SysMtgsThread::PrepDataPacket(GIF_PATH pathidx, u32 size)
{
	PrepDataPacket((MTGS_RingCommand)pathidx, size);
}

__fi void SysMtgsThread::_FinishSimplePacket()
{
	uint future_writepos = (m_WritePos.load(std::memory_order_relaxed) + 1) & RingBufferMask;
	m_WritePos.store(future_writepos, std::memory_order_release);

	if (EmuConfig.GS.SynchronousMTGS)
		WaitGS();
	else
		++m_CopyDataTally;
}

void SysMtgsThread::SendSimplePacket(MTGS_RingCommand type, int data0, int data1, int data2)
{
	GenericStall(1);
	PacketTagType& tag = (PacketTagType&)RingBuffer[m_WritePos.load(std::memory_order_relaxed)];

	tag.command = type;
	tag.data[0] = data0;
	tag.data[1] = data1;
	tag.data[2] = data2;

	_FinishSimplePacket();
}

void SysMtgsThread::SendSimpleGSPacket(MTGS_RingCommand type, u32 offset, u32 size, GIF_PATH path)
{
	SendSimplePacket(type, (int)offset, (int)size, (int)path);

	if (!EmuConfig.GS.SynchronousMTGS)
	{
		m_CopyDataTally += size / 16;
		if (m_CopyDataTally > 0x2000)
			SetEvent();
	}
}

void SysMtgsThread::SendPointerPacket(MTGS_RingCommand type, u32 data0, void* data1)
{
	GenericStall(1);
	PacketTagType& tag = (PacketTagType&)RingBuffer[m_WritePos.load(std::memory_order_relaxed)];

	tag.command = type;
	tag.data[0] = data0;
	tag.pointer = (uptr)data1;

	_FinishSimplePacket();
}

void SysMtgsThread::SendGameCRC(u32 crc)
{
	SendSimplePacket(GS_RINGTYPE_CRC, crc, 0, 0);
}

bool SysMtgsThread::WaitForOpen()
{
	if (IsOpen())
		return true;

	StartThread();
	m_sem_event.NotifyOfWork();

	// wait for it to finish its stuff
	m_open_or_close_done.Wait();

	// did we succeed?
	const bool result = m_open_flag.load(std::memory_order_acquire);
	if (!result)
		Console.Error("GS failed to open.");

	return result;
}

void SysMtgsThread::WaitForClose()
{
	// and kick the thread if it's sleeping
	m_sem_event.NotifyOfWork();

	// and wait for it to finish up..
	m_open_or_close_done.Wait();

	m_thread = {};
}

void SysMtgsThread::Freeze(FreezeAction mode, MTGS_FreezeData& data)
{
	SendPointerPacket(GS_RINGTYPE_FREEZE, (int)mode, &data);
	WaitGS();
}

void SysMtgsThread::RunOnGSThread(AsyncCallType func)
{
	SendPointerPacket(GS_RINGTYPE_ASYNC_CALL, 0, new AsyncCallType(std::move(func)));

	// wake the gs thread in case it's sleeping
	SetEvent();
}

void SysMtgsThread::ApplySettings()
{
	RunOnGSThread([opts = EmuConfig.GS]() {
		GSUpdateConfig(opts);
		GSSetVSyncMode(Host::GetEffectiveVSyncMode());
	});

	// We need to synchronize the thread when changing any settings when the download mode
	// is unsynchronized, because otherwise we might potentially read in the middle of
	// the GS renderer being reopened.
	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false, false, false);
}

void SysMtgsThread::SetVSyncMode(VsyncMode mode)
{
	RunOnGSThread([mode]() {
		GSSetVSyncMode(mode);
	});
}

void SysMtgsThread::UpdateVSyncMode()
{
	SetVSyncMode(Host::GetEffectiveVSyncMode());
}

void SysMtgsThread::SwitchRenderer(GSRendererType renderer, bool display_message /* = true */)
{
	if (display_message)
	{
		Host::AddIconOSDMessage("SwitchRenderer", ICON_FA_MAGIC, fmt::format("Switching to {} renderer...",
			Pcsx2Config::GSOptions::GetRendererName(renderer)), Host::OSD_INFO_DURATION);
	}

	RunOnGSThread([renderer]() {
		GSSwitchRenderer(renderer);
	});

	// See note in ApplySettings() for reasoning here.
	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false, false, false);
}

void SysMtgsThread::SetSoftwareRendering(bool software, bool display_message /* = true */)
{
	// for hardware, use the chosen api in the base config, or auto if base is set to sw
	GSRendererType new_renderer;
	if (!software)
		new_renderer = EmuConfig.GS.UseHardwareRenderer() ? EmuConfig.GS.Renderer : GSRendererType::Auto;
	else
		new_renderer = GSRendererType::SW;

	SwitchRenderer(new_renderer, display_message);
}

void SysMtgsThread::ToggleSoftwareRendering()
{
	// reading from the GS thread.. but should be okay here
	SetSoftwareRendering(GSConfig.Renderer != GSRendererType::SW);
}

void SysMtgsThread::PresentCurrentFrame()
{
	// If we're running idle, we're going to re-present anyway.
	if (m_run_idle_flag.load(std::memory_order_relaxed))
		return;

	RunOnGSThread([]() {
		GSPresentCurrentFrame();
	});
}
