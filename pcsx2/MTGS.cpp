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
#include <rthreads/rthreads.h>

#include <libretro.h>

#include "GS.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "Elfheader.h"

#include "Host.h"

// Mask to apply to ring buffer indices to wrap the pointer from end to
// start (the wrapping is what makes it a ringbuffer, yo!)
static const unsigned int RINGBUFFERMASK = MTGS_RINGBUFFERSIZE - 1;

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

alignas(__cachelinesize) static u128 m_Ring[MTGS_RINGBUFFERSIZE];

extern struct retro_hw_render_callback hw_render;

namespace MTGS
{
	// note: when s_ReadPos == s_WritePos, the fifo is empty
	// Threading info: s_ReadPos is updated by the MTGS thread. s_WritePos is updated by the EE thread

	// s_WritePos and s_ReadPos sit on separate cache lines to avoid
	// false sharing between producer (cpu_thread writes WritePos, reads
	// ReadPos) and consumer (libretro thread writes ReadPos, reads
	// WritePos). Without the padding, every ring push from one side
	// invalidates the cached counter on the other side's core, forcing a
	// coherence transaction on the next access.
	alignas(__cachelinesize) static retro_atomic_int_t s_WritePos = RETRO_ATOMIC_INT_INITIALIZER(0); // cur pos ee thread is writing to
	alignas(__cachelinesize) static retro_atomic_int_t s_ReadPos  = RETRO_ATOMIC_INT_INITIALIZER(0); // cur pos gs is reading from

	static Threading::WorkSema s_sem_event;

	static uintptr_t s_thread;
	static retro_atomic_int_t s_open_flag = RETRO_ATOMIC_INT_INITIALIZER(0);
};

bool MTGS::IsOpen() { return retro_atomic_load_acquire_int(&s_open_flag); }

void MTGS::ResetGS(bool hardware_reset)
{
	// MTGS Reset process:
	//  * clear the ringbuffer.
	//  * Signal a reset.
	//  * clear the path and byRegs structs (used by GIFtagDummy)
	if (hardware_reset)
		s_ReadPos             = s_WritePos.load();

	const unsigned int writepos = retro_atomic_load_acquire_int(&s_WritePos);
	PacketTagType& tag          = (PacketTagType&)m_Ring[writepos];

	tag.command                 = GS_RINGTYPE_RESET;
	tag.data[0]                 = static_cast<int>(hardware_reset);
	tag.data[1]                 = 0;
	tag.data[2]                 = 0;

	retro_atomic_store_release_int(&s_WritePos, (writepos + 1) & RINGBUFFERMASK);

	if (hardware_reset)
		s_sem_event.NotifyOfWork();
}

void MTGS::PostVsyncStart()
{
	// Command qword: Low word is the command, and the high word is the packet
	// length in SIMDs (128 bits).
	const unsigned int writepos       = retro_atomic_load_acquire_int(&s_WritePos);
	PacketTagType& tag                = (PacketTagType&)m_Ring[writepos];
	tag.command                       = GS_RINGTYPE_VSYNC;
	tag.data[0]                       = 0;

	retro_atomic_store_release_int(&s_WritePos, (writepos + 1) & RINGBUFFERMASK);

	// Remove extra frame input lag. With VsyncQueueSize hard-locked to 0 in
	// the libretro topology, this WaitGS IS the frame-pacing mechanism: it
	// blocks cpu_thread until the libretro thread (= MTGS thread) drains
	// the ring through this VSYNC packet.
	//
	// (Letting the EE run 1-2 frames ahead of the GS was tried and measured: no
	// change in wall time, because the EE is not actually waiting here.)
	WaitGS(false);
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

	const unsigned int writepos = retro_atomic_load_acquire_int(&s_WritePos);
	PacketTagType& tag          = (PacketTagType&)m_Ring[writepos];

	tag.command                 = GS_RINGTYPE_INIT_AND_READ_FIFO;
	tag.data[0]                 = qwc;
	tag.pointer                 = (uptr)mem;

	retro_atomic_store_release_int(&s_WritePos, (writepos + 1) & RINGBUFFERMASK);
	WaitGS(false);
}

void MTGS::TryOpenGS(void)
{
	s_thread = sthread_get_current_thread_id();

	GSopen(EmuConfig.GS, EmuConfig.GS.Renderer, hw_render.context_type, PS2MEM_GS);

	retro_atomic_store_release_int(&s_open_flag, true);
}

void MTGS::MainLoop(bool flush_all)
{

	// Threading info: run in MTGS thread
	// s_ReadPos is only update by the MTGS thread so it is safe to load it with a relaxed atomic

	/* MTVU handoff needs no lock: the WaitGS(isMTVU) rendezvous this
	 * loop used to serve is now a real sleep on
	 * vu1Thread.semaP1Progress, posted once per PopGSPacketMTVU
	 * below.  The packet queue itself has always run on its own
	 * atomics + semaXGkick. */

	for (;;)
	{
		if (flush_all)
		{
			if(!s_sem_event.CheckForWork())
				return;
		}
		else
		{
			s_sem_event.WaitForWork();
		}

		if (!retro_atomic_load_acquire_int(&s_open_flag))
			break;

		// note: s_ReadPos is intentionally not volatile, because it should only
		// ever be modified by this thread.
		// Snapshot s_WritePos once per batch to avoid re-acquiring the EE's
		// cache line on every packet.  New packets added during processing
		// are picked up on the next outer-loop iteration.
		const int snapshot_WritePos = retro_atomic_load_acquire_int(&s_WritePos);
		while (retro_atomic_load_acquire_int(&s_ReadPos) != snapshot_WritePos)
		{
			const int local_ReadPos = retro_atomic_load_acquire_int(&s_ReadPos);
			const PacketTagType& tag = (PacketTagType&)m_Ring[local_ReadPos];

			switch (tag.command)
			{
				case GS_RINGTYPE_GSPACKET:
					{
						Gif_Path& path = gifUnit.gifPath[tag.data[2]];
						u32 offset     = tag.data[0];
						u32 size       = tag.data[1];
						if (offset != ~0u)
							GSgifTransfer((u8*)&path.buffer[offset], size / 16);
						retro_atomic_fetch_sub_int(&path.readAmount, size);
					}
					break;

				case GS_RINGTYPE_MTVU_GSPACKET:
				{
					// MTVU_GSPACKET only enqueued in MTVU mode.
					// One ring item = one VU1 program, but the program may
					// deliver MULTIPLE queue packets: PARTIAL flushes
					// (gsPack.cycles != 0, emitted when the worker's path1
					// buffer would fill mid-program -- continuous VU1
					// microprograms) followed by the final packet
					// (cycles == 0). Consume until the final one; each
					// semaXGkick post pairs with exactly one queue push.
					Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
					for (;;)
					{
						// Wait for MTVU to push a path1 packet.
						// Spin-try first (except on aarch64, whose
						// UserspaceSemaphore::Wait is already a
						// syscall-free WFE park): partial flushes and
						// short VU1 programs post microseconds apart,
						// and eating a kernel sleep/wake pair per
						// packet is the dominant per-program cost when
						// this thread outruns the worker.  A miss
						// falls through to the same Wait as before.
#if !defined(__aarch64__)
						{
							s32 spins = Threading::SpinBudget();
							while (!vu1Thread.semaXGkick.TryWait())
							{
								if (--spins <= 0)
								{
									vu1Thread.semaXGkick.Wait();
									break;
								}
								THREADING_CPU_RELAX();
							}
						}
#else
						vu1Thread.semaXGkick.Wait();
#endif
						GS_Packet gsPack = path.GetGSPacketMTVU(); // Get vu1 program's xgkick packet(s)
						if (gsPack.size)
							GSgifTransfer((u8*)&path.buffer[gsPack.offset], gsPack.size / 16);
						retro_atomic_fetch_sub_int(&path.readAmount, gsPack.size + gsPack.readAmount);
						const bool final_packet = gsPack.cycles == 0;
						path.PopGSPacketMTVU(); // Should be done last, for proper WaitGS(isMTVU)
						/* One post per pop: WaitGS(isMTVU) sleeps on
						 * this instead of the old lock rendezvous. */
						vu1Thread.semaP1Progress.Post();
						if (final_packet)
							break;
					}
				}
					break;
				case GS_RINGTYPE_VSYNC:
					// CSR & 0x2000; is the pageflip id.
					// flush_all skips GSvsync when multi-threaded (reset/pause drain
					// without rendering), but in single-threaded mode MainLoop(true)
					// IS the render path — call GSvsync.
					if(!flush_all || sthread_get_current_thread_id() == s_thread)
						GSvsync((gsCSRload() & GS_CSR_FIELD) ? 0 : 1,
						        (bool)retro_atomic_exchange_int(&s_GSRegistersWritten, 0));
					else
						retro_atomic_store_release_int(&s_GSRegistersWritten, 0);
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
				case GS_RINGTYPE_INIT_AND_READ_FIFO:
					GSInitAndReadFIFO((u8*)tag.pointer, tag.data[0]);
					break;
				// Optimized performance in non-Dev builds.
				default:
					break;
			}

			uint newringpos = (local_ReadPos + 1) & RINGBUFFERMASK;
			retro_atomic_store_release_int(&s_ReadPos, newringpos);

			if (!flush_all && tag.command == GS_RINGTYPE_VSYNC)
				return;
		}
	}

	// Unblock any threads in WaitGS in case MTGS gets cancelled while still processing work
	retro_atomic_store_release_int(&s_ReadPos, retro_atomic_load_acquire_int(&s_WritePos));
	/* Wake a WaitGS(isMTVU) sleeper too; its loop re-checks
	 * s_open_flag and exits.  The old rendezvous spin hung here
	 * with pending packets, so this path is strictly safer now. */
	vu1Thread.semaP1Progress.Post();
	s_sem_event.Kill();
}

void MTGS::CloseGS(void)
{
	GSclose();
	retro_atomic_store_release_int(&s_open_flag, false);
}

// Waits for the GS to empty out the entire ring buffer contents.
// This function is allowed to exit after MTGS finished a path1 packet.
// If isMTVU, then this implies this function is being called from the MTVU thread...
void MTGS::WaitGS(bool isMTVU)
{
	if(sthread_get_current_thread_id() == s_thread)
	{
		// Ensure MainLoop(true) doesn't bail immediately from
		// CheckForWork() — entries may have been written without
		// a prior NotifyOfWork (e.g. a frame with no completed
		// GIF packets between PostVsyncStart and WaitGS).
		s_sem_event.NotifyOfWork();
		MainLoop(true);
		return;
	}
	if (!IsOpen()) /* WaitGS issued on a closed thread! */
		return;

	s_sem_event.NotifyOfWork();
	if (isMTVU)
	{
		Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];

		// We will stop waiting on the MTGS thread if the
		// MTGS thread has processed a vu1 xgkick packet, or is pending on
		// its final vu1 xgkick packet (!curP1Packs)...
		// Note: s_WritePos doesn't seem to have proper atomic write
		// code, so reading it from the MTVU thread might be dangerous;
		// hence it has been avoided...
		u32 startP1Packs = path.GetPendingGSPackets();
		if (startP1Packs)
		{
			/* Sleep until MTGS consumes a path-1 packet.  MTGS posts
			 * semaP1Progress once per PopGSPacketMTVU, so this
			 * replaces the old slock rendezvous + Timeslice poll with
			 * a real block - the exit condition is unchanged.
			 *
			 * Drain stale credit first: posts accumulated from pops
			 * outside this wait window would otherwise turn Wait()
			 * into an immediate return and degrade this into a
			 * syscall spin.  A post racing the drain (a pop landing
			 * right now) at worst wakes the first Wait early; the
			 * loop re-checks the real condition.
			 *
			 * Liveness is the same premise the old poll relied on:
			 * progress requires MTGS to pop, and MTGS posts at every
			 * pop - including before it blocks in semaXGkick.Wait,
			 * which it only reaches after popping what was
			 * available. */
			while (vu1Thread.semaP1Progress.TryWait())
			{
			}
			for (;;)
			{
				if (path.GetPendingGSPackets() != startP1Packs)
				{
					/* ringbuffer_base::size() reads both cursors RELAXED, so
					 * on weak memory the changed count can be observed before
					 * MTGS's pop-side writes (the release store to read_index
					 * and the readAmount subtract).  Acquire-fence here to
					 * pair with that release before we act on the count.  The
					 * old code got this incidentally from the slock acquire
					 * it performed each iteration; with the lock gone the
					 * fence has to be explicit.  Costs nothing on x86 and one
					 * dmb ishld on arm64, once, on the exit path. */
					retro_atomic_thread_fence_acquire();
					break;
				}
				if (!retro_atomic_load_acquire_int(&s_open_flag))
					break; /* MTGS cancelled; see MainLoop exit tail */
				vu1Thread.semaP1Progress.Wait();
			}
		}
	}
	else
	{
		/* Blocks until the ring drains. Return value (false if the
		 * MTGS thread has died) is unused here, matching the other
		 * WaitForEmpty call sites in MTVU and GSRasterizer. */
		s_sem_event.WaitForEmpty();
	}
}

void MTGS::WaitForClose()
{
	// and kick the thread if it's sleeping
	s_sem_event.NotifyOfWork();

	s_thread = 0;
}

void MTGS::Freeze(FreezeAction mode, MTGS_FreezeData& data)
{
	const unsigned int writepos = retro_atomic_load_acquire_int(&s_WritePos);
	PacketTagType& tag          = (PacketTagType&)m_Ring[writepos];

	tag.command                 = GS_RINGTYPE_FREEZE;
	tag.data[0]                 = (int)mode;
	tag.pointer                 = (uptr)&data;

	retro_atomic_store_release_int(&s_WritePos, (writepos + 1) & RINGBUFFERMASK);
	WaitGS(false);
}

void MTGS::GameChanged()
{
	GSGameChanged();
}

void MTGS::ApplySettings()
{
	GSUpdateConfig(EmuConfig.GS, hw_render.context_type);
	// We need to synchronize the thread when changing any settings when the download mode
	// is unsynchronized, because otherwise we might potentially read in the middle of
	// the GS renderer being reopened.
	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false);
}

void MTGS::SwitchRenderer(GSRendererType renderer, GSInterlaceMode interlace)
{
	GSSwitchRenderer(renderer, hw_render.context_type, interlace);
	// See note in ApplySettings() for reasoning here.
	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false);
}

// Adds a finished GS Packet to the MTGS ring buffer
void Gif_AddCompletedGSPacket(GS_Packet& _gsPack, GIF_PATH _path)
{
	const unsigned int writepos = retro_atomic_load_acquire_int(&MTGS::s_WritePos);
	PacketTagType& tag          = (PacketTagType&)m_Ring[writepos];
	if (_gsPack.size == ~0u)
	{
		// Used in MTVU mode... MTVU will later complete a real packet
		tag.command                 = GS_RINGTYPE_MTVU_GSPACKET;
		tag.data[0]                 = 0;
		tag.data[1]                 = (int)0;
	}
	else
	{
		tag.command                 = GS_RINGTYPE_GSPACKET;
		tag.data[0]                 = (int)_gsPack.offset;
		tag.data[1]                 = (int)_gsPack.size;

		retro_atomic_fetch_add_int(&gifUnit.gifPath[_path].readAmount, _gsPack.size);
	}
	tag.data[2]                         = (int)_path;
	retro_atomic_store_release_int(&MTGS::s_WritePos, (writepos + 1) & RINGBUFFERMASK);
	MTGS::s_sem_event.NotifyOfWorkIfRunning();
}

void Gif_AddBlankGSPacket(u32 _size, GIF_PATH _path)
{
	// If we're running on the same thread (single-threaded libretro
	// topology), readAmount tracking via blank packets is unnecessary:
	// there is no concurrent GS thread observing readAmount between
	// the fetch_add here and the fetch_sub in MainLoop.  Skipping
	// the ringbuffer entry removes ~88% of MainLoop entries.
	if (sthread_get_current_thread_id() == MTGS::s_thread)
		return;

	retro_atomic_fetch_add_int(&gifUnit.gifPath[_path].readAmount, _size);
	const unsigned int writepos = retro_atomic_load_acquire_int(&MTGS::s_WritePos);
	PacketTagType& tag          = (PacketTagType&)m_Ring[writepos];

	tag.command                 = GS_RINGTYPE_GSPACKET;
	tag.data[0]                 = (int)~0u;
	tag.data[1]                 = (int)_size;
	tag.data[2]                 = (int)_path;

	retro_atomic_store_release_int(&MTGS::s_WritePos, (writepos + 1) & RINGBUFFERMASK);
	MTGS::s_sem_event.NotifyOfWorkIfRunning();
}

