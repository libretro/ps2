#include <cstdio>
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

#include <retro_spsc.h>
#include "common/Console.h"

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

/* The command ring is a byte SPSC of 16-byte PacketTagType records: the
 * EE thread produces, the libretro (= MTGS) thread consumes, and the
 * libretro-thread writers (ResetGS, Freeze, InitAndReadFIFO on its
 * synchronous paths) only run with the EE quiesced, so the single-producer
 * contract holds serially.  The capacity is a whole number of records, so
 * a record never straddles the wrap and the framing is sizeof(PacketTagType).
 *
 * Allocated once in TryOpenGS and kept for the life of the process, which
 * is the same lifetime the static array it replaces had.  retro_spsc pads
 * its own cursors onto separate cache lines, so the alignas gymnastics the
 * old cursor pair needed live inside the queue now. */
static retro_spsc_t s_Ring;
static bool s_RingOk = false;

static_assert(sizeof(PacketTagType) == 16, "command ring framing is one 16-byte record per packet");

/* Reserve one record.  Backpressure on a full ring: the consumer is the
 * libretro thread, which the frontend keeps pumping through retro_run, so
 * a full ring drains and progress is guaranteed.  The old open-coded ring
 * had no full check at all - a lapped writer silently dropped an entire
 * ring of commands - so waiting is strictly safer.  Occupancy was measured
 * at ~22k records worst-case against 65536 capacity (see GS.h), so the
 * wait should never fire outside a wedged consumer.
 *
 * Returns NULL only when the ring never allocated (TryOpenGS failed and
 * logged); the caller drops the command, which is the degraded mode the
 * MTVU packet queue already uses for the same impossible allocation. */
static __fi PacketTagType* RingWriteBegin(void)
{
	void* dst;
	if (!s_RingOk)
		return NULL;
	while (retro_spsc_write_begin(&s_Ring, &dst) < sizeof(PacketTagType))
		;
	return (PacketTagType*)dst;
}

static __fi void RingWriteEnd(void)
{
	retro_spsc_write_end(&s_Ring, sizeof(PacketTagType));
}

#ifdef ENABLE_PCSX2_PROFILER
/* Which side of the handoff is waiting? Each counter is written by exactly
 * one thread -- idle by the GS/frontend thread, wait by the EE thread -- so
 * no synchronisation is needed; they are only read together at the report.
 *
 * GS idle high  => the GS is starved and the EE is the limit.
 * EE wait high  => the GS is the limit and the EE has headroom.
 * Both low      => both saturated, genuinely overlapped.
 * Both high     => neither is saturated; the frame is paced elsewhere. */
alignas(64) static u64 g_gs_idle_ticks;
alignas(64) static u64 g_ee_wait_ticks;
#endif

extern struct retro_hw_render_callback hw_render;

namespace MTGS
{
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
	/* Discarding pending entries requires both sides quiesced: the EE is
	 * paused across a reset, and the consumer is this very thread. */
	if (hardware_reset && s_RingOk)
		retro_spsc_clear(&s_Ring);

	PacketTagType* tag = RingWriteBegin();
	if (tag)
	{
		tag->command = GS_RINGTYPE_RESET;
		tag->data[0] = static_cast<int>(hardware_reset);
		tag->data[1] = 0;
		tag->data[2] = 0;
		RingWriteEnd();
	}

	if (hardware_reset)
		s_sem_event.NotifyOfWork();
}

void MTGS::PostVsyncStart()
{
	// Command qword: Low word is the command, and the high word is the packet
	// length in SIMDs (128 bits).
	PacketTagType* tag = RingWriteBegin();
	if (tag)
	{
		tag->command = GS_RINGTYPE_VSYNC;
		tag->data[0] = 0;
		RingWriteEnd();
	}

#ifdef ENABLE_PCSX2_PROFILER
	{
		static u64 last_wall, last_idle, last_wait;
		static unsigned long frames;
		const u64 now = __builtin_ia32_rdtsc();
		if ((++frames % 60) == 0)
		{
			const double wall = (double)(now - last_wall);
			fprintf(stderr, "[overlap] 60 frames: GS idle %.1f%% of wall, EE blocked in WaitGS %.1f%%\n",
			        wall > 0 ? 100.0 * (double)(g_gs_idle_ticks - last_idle) / wall : 0.0,
			        wall > 0 ? 100.0 * (double)(g_ee_wait_ticks - last_wait) / wall : 0.0);
			last_wall = now; last_idle = g_gs_idle_ticks; last_wait = g_ee_wait_ticks;
		}
		else if (frames == 1)
			last_wall = now;
	}
#endif

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

	PacketTagType* tag = RingWriteBegin();
	if (tag)
	{
		tag->command = GS_RINGTYPE_INIT_AND_READ_FIFO;
		tag->data[0] = qwc;
		tag->pointer = (uptr)mem;
		RingWriteEnd();
	}
	WaitGS(false);
}

void MTGS::TryOpenGS(void)
{
	s_thread = sthread_get_current_thread_id();

	if (!s_RingOk)
	{
		s_RingOk = retro_spsc_init(&s_Ring,
			(size_t)MTGS_RINGBUFFERSIZE * sizeof(PacketTagType));
		if (!s_RingOk)
			Console.Error("MTGS: command ring allocation failed; GS commands will be dropped");
	}

	GSopen(EmuConfig.GS, EmuConfig.GS.Renderer, hw_render.context_type, PS2MEM_GS);

	retro_atomic_store_release_int(&s_open_flag, true);
}

bool MTGS::MainLoop(bool flush_all)
{

	// Threading info: run in MTGS thread

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
				return true;
		}
		else
		{
			/* The frontend thread must never park unboundedly: a wedged
			 * producer degrades to duped frames with a live frontend,
			 * never a frozen process.  100ms only fires when the EE has
			 * genuinely stopped delivering vsyncs. */
#ifdef ENABLE_PCSX2_PROFILER
			{
				const u64 t0 = __builtin_ia32_rdtsc();
				const bool got = s_sem_event.WaitForWorkTimed(100);
				g_gs_idle_ticks += __builtin_ia32_rdtsc() - t0;
				if (!got)
					return false;
			}
#else
			if (!s_sem_event.WaitForWorkTimed(100))
				return false;
#endif
		}

		if (!retro_atomic_load_acquire_int(&s_open_flag))
			break;

		/* Drain in contiguous spans.  read_begin acquires the head once
		 * per span - the same single-acquire-per-batch the old snapshot
		 * of s_WritePos bought - and hands back a stable pointer into
		 * the ring.  Consumed records are committed in one read_end per
		 * span instead of one cursor store per record: the only
		 * cross-thread reader of the tail is the producer's full check,
		 * and the WaitGS emptiness contract lives entirely in WorkSema,
		 * so batching the commit changes nothing anyone can observe.
		 * The vsync early-return commits before leaving.
		 *
		 * The inner loop runs spans until the queue reports empty:
		 * WorkSema wakes are not 1:1 with records (a soft-reset tag is
		 * written with no notify and rides along on the next one), so
		 * exiting to the sema with buffered records - as a wrap split
		 * could otherwise cause - would strand them.  That is also why
		 * the old loop compared cursors instead of trusting the sema. */
		size_t span;
		const void* span_ptr;
		while (s_RingOk && (span = retro_spsc_read_begin(&s_Ring, &span_ptr)) >= sizeof(PacketTagType))
		{
		size_t consumed = 0;
		while (consumed < span)
		{
			const PacketTagType& tag = *(const PacketTagType*)((const u8*)span_ptr + consumed);

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

			consumed += sizeof(PacketTagType);

			if (!flush_all && tag.command == GS_RINGTYPE_VSYNC)
			{
				retro_spsc_read_end(&s_Ring, consumed);
				/* Returning mid-ring: the batch acknowledge at the top of
				 * the outer loop already consumed the notifies for any
				 * entries still queued behind this vsync.  WorkSema's
				 * contract -- relied on by WaitForEmpty and by the
				 * pre-park empty post in WaitForWorkTimed -- is that a
				 * consumer at RUNNING_0 has drained the ring, and the
				 * per-frame early exit is the one place this consumer
				 * breaks it.  Re-arm the sema when entries remain so the
				 * next WaitForWorkTimed drains them instead of parking
				 * and waking a producer whose WaitGS(false) rendezvous
				 * (readbacks, freezes) has not actually completed: a
				 * producer released early reads a readback buffer the GS
				 * side never filled, and the stale entry is processed
				 * late into memory the EE may have reused.  A notify
				 * racing a concurrent enqueue at worst doubles up; the
				 * state machine absorbs spurious wakes by design. */
				if (retro_spsc_read_avail(&s_Ring) != 0)
					s_sem_event.NotifyOfWork();
				return true;
			}
		}
		retro_spsc_read_end(&s_Ring, consumed);
		}
	}

	// Unblock any threads in WaitGS in case MTGS gets cancelled while still processing work
	if (s_RingOk)
		retro_spsc_skip(&s_Ring, retro_spsc_read_avail(&s_Ring));
	/* Wake a WaitGS(isMTVU) sleeper too; its loop re-checks
	 * s_open_flag and exits.  The old rendezvous spin hung here
	 * with pending packets, so this path is strictly safer now. */
	vu1Thread.semaP1Progress.Post();
	s_sem_event.Kill();
	return true;
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

#ifdef ENABLE_PCSX2_PROFILER
	const u64 t_wait0 = __builtin_ia32_rdtsc();
	struct WaitTimer { u64 t; ~WaitTimer() { g_ee_wait_ticks += __builtin_ia32_rdtsc() - t; } } wait_timer{t_wait0};
#endif

	s_sem_event.NotifyOfWork();
	if (isMTVU)
	{
		Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];

		// We will stop waiting on the MTGS thread if the
		// MTGS thread has processed a vu1 xgkick packet, or is pending on
		// its final vu1 xgkick packet (!curP1Packs)...
		// Note: the command ring's cursors belong to the EE and MTGS
		// threads; this MTVU-thread path deliberately never reads them
		// and keys off the packet queue instead.
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
					/* The packet count derives from the queue's cursors, so
					 * on weak memory the changed count can be observed before
					 * MTGS's pop-side writes (the release store on the tail
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
	PacketTagType* tag = RingWriteBegin();
	if (tag)
	{
		tag->command = GS_RINGTYPE_FREEZE;
		tag->data[0] = (int)mode;
		tag->pointer = (uptr)&data;
		RingWriteEnd();
	}
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
	PacketTagType* tag = RingWriteBegin();
	if (!tag)
		return;
	if (_gsPack.size == ~0u)
	{
		// Used in MTVU mode... MTVU will later complete a real packet
		tag->command = GS_RINGTYPE_MTVU_GSPACKET;
		tag->data[0] = 0;
		tag->data[1] = (int)0;
	}
	else
	{
		tag->command = GS_RINGTYPE_GSPACKET;
		tag->data[0] = (int)_gsPack.offset;
		tag->data[1] = (int)_gsPack.size;

		retro_atomic_fetch_add_int(&gifUnit.gifPath[_path].readAmount, _gsPack.size);
	}
	tag->data[2] = (int)_path;
	RingWriteEnd();
	MTGS::s_sem_event.NotifyOfWork();
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
	PacketTagType* tag = RingWriteBegin();
	if (!tag)
		return;

	tag->command = GS_RINGTYPE_GSPACKET;
	tag->data[0] = (int)~0u;
	tag->data[1] = (int)_size;
	tag->data[2] = (int)_path;

	RingWriteEnd();
	MTGS::s_sem_event.NotifyOfWork();
}

