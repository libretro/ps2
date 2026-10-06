#include <cstdio>
#include "common/Pcsx2Defs.h"
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
#include "VMManager.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "WorkEventCount.h"
#include "MTGSOwner.h"
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "Elfheader.h"

#include "Host.h"

#include <retro_spsc.h>

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

/* The GS's view of the privileged registers: a copy of PS2MEM_GS taken
 * on this thread at each vsync, reset and freeze, while the EE is held
 * in WaitGS. The renderers read display registers at vsync and nothing
 * else, so a copy taken then is the frame's, and the EE is free to write
 * the next frame's the moment its vsync is consumed - before the scanout,
 * the frontend's present and its pacing wait, which the EE then overlaps
 * instead of idling through. CSR stays in PS2MEM_GS, where the EE keeps
 * it atomically; the field it carries at the vsync rides in the packet. */
alignas(64) static u8 s_gs_regs[PS2MEM_GS_REGS];

static __fi void mtgs_sync_regs(void)
{
	memcpy(s_gs_regs, PS2MEM_GS, sizeof(s_gs_regs));
}

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

/* Set by the frontend's thread around a context teardown, and read by
 * the drain that teardown runs on the same thread: mtgs_hold_present. */
static int s_present_held = 0;

/* See GS.h. NULL unless the renderer in use installed them. */
void (*gs_hw_context_begin)(void) = NULL;
void (*gs_hw_context_end)(void)   = NULL;

/* One read of the CPU count, cached. Spinning on a single-core host only
 * steals the producer's timeslice, so the budget is 0 there. Defined here
 * rather than in the header so the header pulls in no platform code. */
s32 WorkEventCount_SpinBudget(void)
{
	static s32 budget = -1;
	if (budget < 0)
	{
#if defined(_WIN32)
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		budget = (si.dwNumberOfProcessors >= 2) ? 2000 : 0;
#else
		budget = (sysconf(_SC_NPROCESSORS_ONLN) >= 2) ? 2000 : 0;
#endif
	}
	return budget;
}

namespace MTGS
{
	static WorkEventCount s_sem_event;


	static uintptr_t s_thread;
	static uintptr_t s_producer_thread;
	static retro_atomic_int_t s_open_flag = RETRO_ATOMIC_INT_INITIALIZER(0);
};

bool MTGS::IsOpen() { return retro_atomic_load_acquire_int(&s_open_flag); }

/* --- the GS on its own thread -------------------------------------------
 *
 * Without it, retro_run drains the ring: every GIF packet of the frame
 * is parsed and recorded on the frontend's thread between its input
 * poll and the frame's handover, and that is the time the frontend
 * reports as the core's (several ms for paraLLEl-GS). With it, a worker
 * drains the ring as the EE writes it, so a frame is rendered while the
 * EE runs it, and retro_run finds it done and only hands it over.
 *
 * The EE's pacing is unchanged. It is held at each vsync until that
 * vsync's record is committed (PostVsyncStart, WaitGS); the worker
 * renders the vsync, keeps the frame and parks on that record until
 * retro_run has handed the frame over, and commits it then - so the EE
 * is let on to the next frame when it always was, at the present, and
 * runs no further ahead than before.
 *
 * A drain from the frontend's side (WaitGS from a thread that does not
 * produce, cpu_thread_pause) does not run the ring itself: it hands over
 * or discards a parked frame by the rule in MTGSOwner.h, and waits for
 * the worker to empty the ring. A hold (mtgs_worker_hold) parks the
 * worker between records for a caller that touches the renderer
 * directly, as ApplySettings does.
 *
 * Vulkan only: its command buffers are recorded and submitted from any
 * thread, and set_image and the video callback stay in retro_run. */
extern retro_video_refresh_t video_cb;

namespace MTGS
{
	enum
	{
		FRAME_NONE = 0,  /* nothing parked */
		FRAME_READY,     /* rendered, waiting for retro_run */
		FRAME_RELEASED   /* handed over or discarded; the worker commits */
	};
	static bool s_own_thread;
	static sthread_t* s_worker;
	static retro_atomic_int_t s_worker_on   = RETRO_ATOMIC_INT_INITIALIZER(0);
	static retro_atomic_int_t s_frame_state = RETRO_ATOMIC_INT_INITIALIZER(0);
	static retro_atomic_int_t s_worker_hold = RETRO_ATOMIC_INT_INITIALIZER(0);
	static retro_atomic_int_t s_worker_held = RETRO_ATOMIC_INT_INITIALIZER(0);
	/* worker -> frontend: a frame is ready, the ring went empty, or the
	 * hold moved; frontend -> worker: the frame was released, or the
	 * hold moved. */
	static retro_eventcount_t s_frame_ec;
	static retro_eventcount_t s_release_ec;
	/* The parked frame's handover, written by the worker before
	 * FRAME_READY and run by the frontend after it; NULL is a dupe. */
	static void (*s_present_fn)(void* ctx);
	static void* s_present_ctx;

	static void WorkerEntry(void* unused);
}

static INLINE int mtgs_worker_on(void)
{
	return retro_atomic_load_acquire_int(&MTGS::s_worker_on);
}

/* The handover itself: on the thread that renders without a worker, in
 * retro_run with one. */
static void mtgs_present_now(void (*fn)(void* ctx), void* ctx)
{
	if (fn)
		fn(ctx);
	else
		video_cb(NULL, 0, 0, 0);
}

static void mtgs_present(void (*fn)(void* ctx), void* ctx)
{
	if (!mtgs_worker_on())
	{
		mtgs_present_now(fn, ctx);
		return;
	}
	MTGS::s_present_fn  = fn;
	MTGS::s_present_ctx = ctx;
}

void gs_present_vk(void (*handover)(void* ctx), void* ctx)
{
	mtgs_present(handover, ctx);
}

void gs_present_dupe(void)
{
	mtgs_present(NULL, NULL);
}

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
		work_eventcount_notify(&s_sem_event);
}

void MTGS::PostVsyncStart()
{
	// Command qword: Low word is the command, and the high word is the packet
	// length in SIMDs (128 bits).
	PacketTagType* tag = RingWriteBegin();
	if (tag)
	{
		tag->command = GS_RINGTYPE_VSYNC;
		/* The field this vsync scans out, and whether the privileged
		 * registers changed since the last one: read here, on the thread
		 * that owns them, so the consumer needs nothing from the EE's
		 * state by the time it gets to them. */
		tag->data[0] = (gsCSRload() & GS_CSR_FIELD) ? 0 : 1;
		tag->data[1] = (u32)retro_atomic_exchange_int(&s_GSRegistersWritten, 0);
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

	/* This wait is the pacing between the two threads: the EE holds here
	 * until the libretro thread has consumed this vsync packet, which is
	 * where the GS takes its copy of the registers and lets the EE go on
	 * to the next frame while it scans this one out and hands it over. */
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
	/* Whoever opens the GS owns the ring until the frontend's thread
	 * says otherwise (mtgs_claim_ring, every retro_run). There is never a
	 * moment with the GS open and nobody owning it, which is the moment
	 * the EE used to fall into. */
	s_thread = sthread_get_current_thread_id();

	if (!s_RingOk)
	{
		static bool s_ec_inited = false;
		if (!s_ec_inited)
		{
			work_eventcount_init(&s_sem_event);
			s_ec_inited = true;
		}
		s_RingOk = retro_spsc_init(&s_Ring,
			(size_t)MTGS_RINGBUFFERSIZE * sizeof(PacketTagType));
		if (!s_RingOk)
			log_cb(RETRO_LOG_ERROR, "MTGS: command ring allocation failed; GS commands will be dropped\n");
	}

	/* The GS opens on an empty ring and a live eventcount, as it does in
	 * a freshly loaded image. A GS closed earlier in this image left the
	 * eventcount killed by MainLoop's exit tail, and the ring with
	 * whatever was written after that tail skipped it; a frontend that
	 * retires the core with dlclose may get the same image, statics and
	 * all, on the next load. The EE is not producing while the GS is
	 * closed, so both sides are quiescent here. */
	if (s_RingOk)
		retro_spsc_clear(&s_Ring);
	work_eventcount_reset(&s_sem_event);

	mtgs_sync_regs();
	GS_HW_CONTEXT_BEGIN();
	GSopen(EmuConfig.GS, EmuConfig.GS.Renderer, hw_render.context_type, s_gs_regs);
	GS_HW_CONTEXT_END();

	retro_atomic_store_release_int(&s_open_flag, true);

	/* The GS on its own thread: Vulkan, a hardware renderer, and a
	 * ring to drain. The thread takes its own Granite command pool
	 * index as any recording thread does. */
	if (s_own_thread && s_RingOk && GSRendererOpen()
	    && hw_render.context_type == RETRO_HW_CONTEXT_VULKAN
	    && GSConfig.Renderer != GSRendererType::SW)
	{
		static bool s_ec_inited = false;
		if (!s_ec_inited)
		{
			retro_eventcount_init(&s_frame_ec);
			retro_eventcount_init(&s_release_ec);
			s_ec_inited = true;
		}
		retro_atomic_store_release_int(&s_frame_state, FRAME_NONE);
		retro_atomic_store_release_int(&s_worker_hold, 0);
		retro_atomic_store_release_int(&s_worker_held, 0);
		s_worker = sthread_create_with_stack_size(WorkerEntry, NULL,
				VMManager::EMU_THREAD_STACK_SIZE);
		if (s_worker)
			retro_atomic_store_release_int(&s_worker_on, 1);
		else
			log_cb(RETRO_LOG_ERROR, "MTGS: GS thread creation failed; the GS runs in retro_run\n");
	}
}

void MTGS::SetOwnThread(bool on)
{
	s_own_thread = on;
}

/* --- the worker and the frontend's side of it ------------------------- */

static void mtgs_run_tag(const PacketTagType& tag);

/* Parks the worker while a hold is on. Between records only, and at
 * a parked vsync: nowhere inside the renderer. */
static void mtgs_worker_wait_hold(void)
{
	while (retro_atomic_load_acquire_int(&MTGS::s_worker_hold)
	    && retro_atomic_load_acquire_int(&MTGS::s_open_flag))
	{
		int key;
		if (!retro_atomic_load_acquire_int(&MTGS::s_worker_held))
		{
			retro_atomic_store_release_int(&MTGS::s_worker_held, 1);
			retro_eventcount_notify(&MTGS::s_frame_ec);
		}
		key = retro_eventcount_prepare_wait(&MTGS::s_release_ec);
		if (!retro_atomic_load_acquire_int(&MTGS::s_worker_hold)
		 || !retro_atomic_load_acquire_int(&MTGS::s_open_flag))
		{
			retro_eventcount_cancel_wait(&MTGS::s_release_ec);
			break;
		}
		retro_eventcount_commit_wait(&MTGS::s_release_ec, key);
	}
	if (retro_atomic_load_acquire_int(&MTGS::s_worker_held))
	{
		retro_atomic_store_release_int(&MTGS::s_worker_held, 0);
		retro_eventcount_notify(&MTGS::s_frame_ec);
	}
}

/* The vsync's scanout on the worker: render it, park the frame for
 * retro_run, and commit the record - which lets the EE go - once the
 * frame has been handed over or discarded. */
static void mtgs_worker_vsync(const PacketTagType& tag)
{
	const u32 field = tag.data[0];
	const bool registers_written = tag.data[1] != 0;
	mtgs_sync_regs();
	MTGS::s_present_fn  = NULL;
	MTGS::s_present_ctx = NULL;
	GSvsync(field, registers_written);

	retro_atomic_store_release_int(&MTGS::s_frame_state, MTGS::FRAME_READY);
	retro_eventcount_notify(&MTGS::s_frame_ec);
	for (;;)
	{
		int key;
		mtgs_worker_wait_hold();
		if (retro_atomic_load_acquire_int(&MTGS::s_frame_state) == MTGS::FRAME_RELEASED
		 || !retro_atomic_load_acquire_int(&MTGS::s_open_flag))
			break;
		key = retro_eventcount_prepare_wait(&MTGS::s_release_ec);
		if (retro_atomic_load_acquire_int(&MTGS::s_frame_state) == MTGS::FRAME_RELEASED
		 || !retro_atomic_load_acquire_int(&MTGS::s_open_flag)
		 || retro_atomic_load_acquire_int(&MTGS::s_worker_hold))
		{
			retro_eventcount_cancel_wait(&MTGS::s_release_ec);
			continue;
		}
		retro_eventcount_commit_wait(&MTGS::s_release_ec, key);
	}
}

void MTGS::WorkerEntry(void* unused)
{
	(void)unused;
	for (;;)
	{
		size_t span;
		const void* span_ptr;
		mtgs_worker_wait_hold();
		work_eventcount_wait(&s_sem_event);
		if (!retro_atomic_load_acquire_int(&s_open_flag))
			break;
		while (s_RingOk && (span = retro_spsc_read_begin(&s_Ring, &span_ptr)) >= sizeof(PacketTagType))
		{
			size_t consumed = 0;
			while (consumed < span)
			{
				const PacketTagType& tag = *(const PacketTagType*)((const u8*)span_ptr + consumed);
				if (tag.command == GS_RINGTYPE_VSYNC)
				{
					int epoch;
					mtgs_worker_vsync(tag);
					if (!retro_atomic_load_acquire_int(&s_open_flag))
						goto out;
					consumed += sizeof(PacketTagType);
					retro_spsc_read_end(&s_Ring, consumed);
					consumed = 0;
					/* As in MainLoop: idle at the current epoch lets the
					 * EE's wait go; anything behind the vsync re-arms. */
					epoch = work_eventcount_epoch(&s_sem_event);
					work_eventcount_drained(&s_sem_event, epoch,
							retro_spsc_read_avail(&s_Ring) != 0);
					retro_atomic_store_release_int(&s_frame_state, FRAME_NONE);
					retro_eventcount_notify(&s_frame_ec);
					/* The span is committed up to here; what follows it
					 * is taken as a new one. */
					break;
				}
				mtgs_run_tag(tag);
				consumed += sizeof(PacketTagType);
			}
			if (consumed)
				retro_spsc_read_end(&s_Ring, consumed);
		}
		/* Empty: a drain waiting on that may go. */
		retro_eventcount_notify(&s_frame_ec);
	}
out:
	/* As MainLoop's exit: nothing parks on a closed GS. */
	if (s_RingOk)
		retro_spsc_skip(&s_Ring, retro_spsc_read_avail(&s_Ring));
	retro_asym_eventcount_notify(&vu1Thread.ecP1Progress);
	work_eventcount_kill(&s_sem_event);
	retro_atomic_store_release_int(&s_frame_state, FRAME_NONE);
	retro_eventcount_notify(&s_frame_ec);
}

/* Hands the parked frame over, or discards it, and lets the worker
 * commit the vsync. */
static void mtgs_frontend_release(int present)
{
	if (present)
		mtgs_present_now(MTGS::s_present_fn, MTGS::s_present_ctx);
	retro_atomic_store_release_int(&MTGS::s_frame_state, MTGS::FRAME_RELEASED);
	retro_eventcount_notify(&MTGS::s_release_ec);
}

/* retro_run's frame: the one the worker has parked, or within 100 ms
 * the next it parks. False on the timeout, as MainLoop returns it. */
static bool mtgs_frontend_frame(void)
{
	for (;;)
	{
		int key;
		if (retro_atomic_load_acquire_int(&MTGS::s_frame_state) == MTGS::FRAME_READY)
		{
			mtgs_frontend_release(1);
			return true;
		}
		if (!retro_atomic_load_acquire_int(&MTGS::s_open_flag))
			return false;
		key = retro_eventcount_prepare_wait(&MTGS::s_frame_ec);
		if (retro_atomic_load_acquire_int(&MTGS::s_frame_state) == MTGS::FRAME_READY
		 || !retro_atomic_load_acquire_int(&MTGS::s_open_flag))
		{
			retro_eventcount_cancel_wait(&MTGS::s_frame_ec);
			continue;
		}
		if (!retro_eventcount_commit_wait_timeout(&MTGS::s_frame_ec, key, 100 * 1000))
		{
			if (retro_atomic_load_acquire_int(&MTGS::s_frame_state) != MTGS::FRAME_READY)
				return false;
		}
	}
}

/* A drain from the frontend's side: the ring is run by the worker, and
 * this waits for it to empty, handing over or discarding a parked frame
 * on the way by the rule in MTGSOwner.h. */
static void mtgs_frontend_drain(void)
{
	work_eventcount_notify(&MTGS::s_sem_event);
	for (;;)
	{
		int key;
		if (!retro_atomic_load_acquire_int(&MTGS::s_open_flag))
			return;
		if (retro_atomic_load_acquire_int(&MTGS::s_frame_state) == MTGS::FRAME_READY)
			mtgs_frontend_release(mtgs_vsync_presents(1,
					sthread_get_current_thread_id(), MTGS::s_thread, s_present_held));
		if (!s_RingOk || retro_spsc_read_avail(&s_Ring) == 0)
			return;
		key = retro_eventcount_prepare_wait(&MTGS::s_frame_ec);
		if (retro_atomic_load_acquire_int(&MTGS::s_frame_state) == MTGS::FRAME_READY
		 || retro_spsc_read_avail(&s_Ring) == 0
		 || !retro_atomic_load_acquire_int(&MTGS::s_open_flag))
		{
			retro_eventcount_cancel_wait(&MTGS::s_frame_ec);
			continue;
		}
		retro_eventcount_commit_wait(&MTGS::s_frame_ec, key);
	}
}

/* Parks the worker outside the renderer for a caller that is about to
 * touch it from another thread, and lets it go again. A count: the EE
 * applies settings at a game's start while the frontend may too. */
void mtgs_worker_hold(int on)
{
	if (!mtgs_worker_on())
		return;
	if (!on)
	{
		/* Returns once the worker has seen the last hold go: a hold
		 * taken straight after must not find the acknowledgement of
		 * this one still standing. */
		if (retro_atomic_fetch_sub_int(&MTGS::s_worker_hold, 1) != 1)
			return;
		retro_eventcount_notify(&MTGS::s_release_ec);
		for (;;)
		{
			int key;
			if (!retro_atomic_load_acquire_int(&MTGS::s_worker_held)
			 || !retro_atomic_load_acquire_int(&MTGS::s_open_flag))
				return;
			key = retro_eventcount_prepare_wait(&MTGS::s_frame_ec);
			if (!retro_atomic_load_acquire_int(&MTGS::s_worker_held)
			 || !retro_atomic_load_acquire_int(&MTGS::s_open_flag))
			{
				retro_eventcount_cancel_wait(&MTGS::s_frame_ec);
				return;
			}
			retro_eventcount_commit_wait(&MTGS::s_frame_ec, key);
		}
	}
	retro_atomic_fetch_add_int(&MTGS::s_worker_hold, 1);
	work_eventcount_notify(&MTGS::s_sem_event);
	retro_eventcount_notify(&MTGS::s_release_ec);
	for (;;)
	{
		int key;
		if (retro_atomic_load_acquire_int(&MTGS::s_worker_held)
		 || !retro_atomic_load_acquire_int(&MTGS::s_open_flag))
			return;
		key = retro_eventcount_prepare_wait(&MTGS::s_frame_ec);
		if (retro_atomic_load_acquire_int(&MTGS::s_worker_held)
		 || !retro_atomic_load_acquire_int(&MTGS::s_open_flag))
		{
			retro_eventcount_cancel_wait(&MTGS::s_frame_ec);
			return;
		}
		retro_eventcount_commit_wait(&MTGS::s_frame_ec, key);
	}
}

/* Runs one record of the ring, on whichever thread drains it. A vsync
 * is not run here: its commit and scanout are the drain's own, see the
 * two drains below. */
static void mtgs_run_tag(const PacketTagType& tag)
{
	switch (tag.command)
	{
		case GS_RINGTYPE_GSPACKET:
			{
				Gif_Path& path = gifUnit.gifPath[tag.data[2]];
				u32 offset     = tag.data[0];
				u32 size       = tag.data[1];
				if (offset != ~0u)
				{
					GS_HW_CONTEXT_BEGIN();
					GSgifTransfer((u8*)&path.buffer[offset], size / 16);
					GS_HW_CONTEXT_END();
				}
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
			// Each ecXGkick notify follows exactly one queue push.
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
				/* Wait for a path-1 packet. The queue's occupancy is
				 * the condition -- there is no separate credit to take,
				 * and PopGSPacketMTVU below is the consume. Spin the
				 * budget first, then register, re-check, park. */
				if (!path.GetPendingGSPackets())
				{
					s32 spins = WorkEventCount_SpinBudget();
					while (!path.GetPendingGSPackets() && spins-- > 0)
						WORK_EVENTCOUNT_RELAX();
					while (!path.GetPendingGSPackets())
					{
						int key = retro_asym_eventcount_prepare_wait(&vu1Thread.ecXGkick);
						if (path.GetPendingGSPackets())
						{
							retro_asym_eventcount_cancel_wait(&vu1Thread.ecXGkick);
							break;
						}
						retro_asym_eventcount_commit_wait(&vu1Thread.ecXGkick, key);
					}
				}
				GS_Packet gsPack = path.GetGSPacketMTVU(); // Get vu1 program's xgkick packet(s)
				if (gsPack.size)
				{
					GS_HW_CONTEXT_BEGIN();
					GSgifTransfer((u8*)&path.buffer[gsPack.offset], gsPack.size / 16);
					GS_HW_CONTEXT_END();
				}
				retro_atomic_fetch_sub_int(&path.readAmount, gsPack.size + gsPack.readAmount);
				const bool final_packet = gsPack.cycles == 0;
				path.PopGSPacketMTVU(); // Should be done last, for proper WaitGS(isMTVU)
				/* One post per pop: WaitGS(isMTVU) sleeps on
				 * this instead of the old lock rendezvous. */
				retro_asym_eventcount_notify(&vu1Thread.ecP1Progress);
				if (final_packet)
					break;
			}
		}
			break;
		case GS_RINGTYPE_FREEZE:
			{
				MTGS_FreezeData* data = (MTGS_FreezeData*)tag.pointer;
				int mode = tag.data[0];
				mtgs_sync_regs();
				GS_HW_CONTEXT_BEGIN();
				GSfreeze((FreezeAction)mode, (freezeData*)data->fdata);
				GS_HW_CONTEXT_END();
			}
			break;
		case GS_RINGTYPE_RESET:
			mtgs_sync_regs();
			GS_HW_CONTEXT_BEGIN();
			GSreset(tag.data[0] != 0);
			GS_HW_CONTEXT_END();
			break;
		case GS_RINGTYPE_INIT_AND_READ_FIFO:
			GS_HW_CONTEXT_BEGIN();
			GSInitAndReadFIFO((u8*)tag.pointer, tag.data[0]);
			GS_HW_CONTEXT_END();
			break;
		// Optimized performance in non-Dev builds.
		default:
			break;
	}
}

bool MTGS::MainLoop(bool flush_all)
{
	/* With the GS on its own thread this is the frontend's side of it:
	 * retro_run's handover, or a drain. */
	if (mtgs_worker_on())
	{
		if (flush_all)
		{
			mtgs_frontend_drain();
			return true;
		}
		return mtgs_frontend_frame();
	}

	// Threading info: run in MTGS thread

	/* MTVU handoff needs no lock: the WaitGS(isMTVU) rendezvous this
	 * loop used to serve is now a real sleep on
	 * vu1Thread.ecP1Progress, notified once per PopGSPacketMTVU
	 * below.  The packet queue itself has always run on its own
	 * atomics + ecXGkick. */

	for (;;)
	{
		if (flush_all)
		{
			if(!work_eventcount_check(&s_sem_event))
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
				const bool got = work_eventcount_wait_timed(&s_sem_event, 100);
				g_gs_idle_ticks += __builtin_ia32_rdtsc() - t0;
				if (!got)
					return false;
			}
#else
			if (!work_eventcount_wait_timed(&s_sem_event, 100))
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
		 * and the WaitGS emptiness contract lives entirely in the work eventcount,
		 * so batching the commit changes nothing anyone can observe.
		 * The vsync early-return commits before leaving.
		 *
		 * The inner loop runs spans until the queue reports empty:
		 * eventcount wakes are not 1:1 with records (a soft-reset tag is
		 * written with no notify and rides along on the next one), so
		 * exiting to the sema with buffered records - as a wrap split
		 * could otherwise cause - would strand them.  That is also why
		 * the old loop compared cursors instead of trusting the sema. */
		size_t span;
		const void* span_ptr;
		while (s_RingOk && (span = retro_spsc_read_begin(&s_Ring, &span_ptr)) >= sizeof(PacketTagType))
		{
		size_t consumed = 0;
		/* The hardware context, where it has to be taken, is taken around
		 * each call into the GS below and nothing else. Not around a span:
		 * a span can be a whole frame, and it includes the waits for the
		 * VU1 worker, and for all of that the frontend's thread could not
		 * start its frame. */
		while (consumed < span)
		{
			const PacketTagType& tag = *(const PacketTagType*)((const u8*)span_ptr + consumed);

			if (tag.command == GS_RINGTYPE_VSYNC)
			{
				const u32 field = tag.data[0];
				const bool registers_written = tag.data[1] != 0;
				/* The EE is held in WaitGS behind this packet, so the
				 * registers are this frame's: take them, and if this is
				 * the per-frame exit, commit the packet and let the EE go
				 * before the scanout. The tag is not read past here. */
				mtgs_sync_regs();
				if (!flush_all)
				{
					consumed += sizeof(PacketTagType);
					retro_spsc_read_end(&s_Ring, consumed);
					consumed = 0;
					/* Idle at the current epoch releases WaitGS's
					 * empty-wait, including when the vsync's own notify
					 * landed after this drain began. Entries behind the
					 * vsync (a soft-reset tag rides along with no notify
					 * of its own) re-arm the work count so the next call
					 * drains them. */
					const int epoch = work_eventcount_epoch(&s_sem_event);
					work_eventcount_drained(&s_sem_event, epoch,
							retro_spsc_read_avail(&s_Ring) != 0);
				}
				/* Whether this vsync is scanned out: see
				 * mtgs_vsync_presents (MTGSOwner.h). A drain on the
				 * thread that renders presents the frame the EE had
				 * ready, unless the frontend is taking the context
				 * away. */
				if (mtgs_vsync_presents(flush_all,
							sthread_get_current_thread_id(), s_thread,
							s_present_held))
				{
					GS_HW_CONTEXT_BEGIN();
					GSvsync(field, registers_written);
					GS_HW_CONTEXT_END();
				}
				if (!flush_all)
					return true;
			}
			else
				mtgs_run_tag(tag);

			consumed += sizeof(PacketTagType);
		}
		retro_spsc_read_end(&s_Ring, consumed);
		}
	}

	// Unblock any threads in WaitGS in case MTGS gets cancelled while still processing work
	if (s_RingOk)
		retro_spsc_skip(&s_Ring, retro_spsc_read_avail(&s_Ring));
	/* Wake a WaitGS(isMTVU) sleeper too; its loop re-checks
	 * s_open_flag and exits. */
	retro_asym_eventcount_notify(&vu1Thread.ecP1Progress);
	work_eventcount_kill(&s_sem_event);
	return true;
}

void MTGS::CloseGS(void)
{
	/* The worker leaves first: everything it parks on is woken, and it
	 * runs its exit tail on the way out. The renderer is closed on this
	 * thread once it is gone. */
	if (s_worker)
	{
		retro_atomic_store_release_int(&s_open_flag, false);
		work_eventcount_kill(&s_sem_event);
		retro_eventcount_notify(&s_release_ec);
		sthread_join(s_worker);
		s_worker = NULL;
		retro_atomic_store_release_int(&s_worker_on, 0);
		retro_atomic_store_release_int(&s_frame_state, FRAME_NONE);
		retro_atomic_store_release_int(&s_worker_hold, 0);
		retro_atomic_store_release_int(&s_worker_held, 0);
	}
	GS_HW_CONTEXT_BEGIN();
	GSclose();
	GS_HW_CONTEXT_END();
	retro_atomic_store_release_int(&s_open_flag, false);
}

// Waits for the GS to empty out the entire ring buffer contents.
// This function is allowed to exit after MTGS finished a path1 packet.
// If isMTVU, then this implies this function is being called from the MTVU thread...
/* The consumer commits a record only once it has processed it, and the
 * vsync before its scanout, so an empty ring is a drained one. */
static int mtgs_ring_drained(void* ctx)
{
	(void)ctx;
	return retro_spsc_read_avail(&s_Ring) == 0;
}

void MTGS::WaitGS(bool isMTVU)
{
	/* See MTGSOwner.h for who drains and why it is decided by who
	 * produces. A producer's wait parks until the frontend's side has
	 * drained the ring, which every retro_run does; anyone else's wait
	 * drains it here. */
	if (!isMTVU)
	{
		if (mtgs_wait_drains(sthread_get_current_thread_id(), s_producer_thread, 0))
		{
			/* Entries may have been written without a notify (a frame
			 * with no completed GIF packets between PostVsyncStart and
			 * here); without this MainLoop(true) returns at once. */
			work_eventcount_notify(&s_sem_event);
			MainLoop(true);   /* the worker's drain when there is one */
			return;
		}
		if (!IsOpen())
			return;
		work_eventcount_notify(&s_sem_event);
		/* Blocks until the ring drains: until MainLoop has committed
		 * every record written so far, the vsync among them. The return
		 * value (false if the ring was killed) is unused, as at the
		 * other wait_empty sites. */
		if (s_RingOk)
			work_eventcount_wait_drained(&s_sem_event, mtgs_ring_drained, NULL);
		else
			work_eventcount_wait_empty(&s_sem_event);
		return;
	}
	if (!IsOpen()) /* WaitGS issued on a closed thread! */
		return;

#ifdef ENABLE_PCSX2_PROFILER
	const u64 t_wait0 = __builtin_ia32_rdtsc();
	struct WaitTimer { u64 t; ~WaitTimer() { g_ee_wait_ticks += __builtin_ia32_rdtsc() - t; } } wait_timer{t_wait0};
#endif

	work_eventcount_notify(&s_sem_event);
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
			/* Park until MTGS consumes a path-1 packet. MTGS notifies
			 * ecP1Progress once per PopGSPacketMTVU; the exit condition
			 * is the packet count moving. An eventcount rather than a
			 * semaphore because the count of pops is not what is wanted,
			 * only that one happened: the semaphore this replaced had to
			 * drain stale credit before each wait, and a post racing the
			 * drain could still turn the wait into a syscall spin.
			 *
			 * Liveness is unchanged: progress requires MTGS to pop, and
			 * MTGS notifies at every pop -- including before it blocks in
			 * the ecXGkick park, which it only reaches after popping what
			 * was available. */
			for (;;)
			{
				int key;
				if (path.GetPendingGSPackets() != startP1Packs)
				{
					/* The packet count derives from the queue's cursors, so
					 * on weak memory the changed count can be observed before
					 * MTGS's pop-side writes (the release store on the tail
					 * and the readAmount subtract).  Acquire-fence here to
					 * pair with that release before we act on the count. */
					retro_atomic_thread_fence_acquire();
					break;
				}
				if (!retro_atomic_load_acquire_int(&s_open_flag))
					break; /* MTGS cancelled; see MainLoop exit tail */
				/* Register, then re-check under the eventcount's own
				 * ordering: after prepare_wait either a pop already landed
				 * and the re-check sees it, or MTGS sees us registered and
				 * its notify wakes us. There is no credit to drain, so a
				 * stale post cannot turn this into a spin. */
				key = retro_asym_eventcount_prepare_wait(&vu1Thread.ecP1Progress);
				if (path.GetPendingGSPackets() != startP1Packs
				 || !retro_atomic_load_acquire_int(&s_open_flag))
				{
					retro_asym_eventcount_cancel_wait(&vu1Thread.ecP1Progress);
					continue;
				}
				retro_asym_eventcount_commit_wait(&vu1Thread.ecP1Progress, key);
			}
		}
	}
}

void MTGS::WaitForClose()
{
	// and kick the thread if it's sleeping
	work_eventcount_notify(&s_sem_event);

	/* Ownership of the ring goes back to nobody; the GS stays open
	 * until CloseGS(), and a WaitGS in between pumps the ring itself
	 * (see WaitGS). */
	s_thread = 0;
}

/* The frontend's thread, first thing in every retro_run. s_thread is
 * what the vsync and blank-packet tests compare against -- "is this the
 * thread that renders" -- and no longer decides who drains a wait. */
void mtgs_claim_ring(void)
{
	MTGS::s_thread = sthread_get_current_thread_id();
}

/* The frontend's thread, around context_destroy and unload: the drain
 * inside consumes the frame the EE had ready without presenting it. See
 * mtgs_vsync_presents (MTGSOwner.h). */
void mtgs_hold_present(int on)
{
	s_present_held = on;
}

/* The EE thread, as it starts and as it ends. See MTGSOwner.h. */
void mtgs_set_producer_thread(int on)
{
	MTGS::s_producer_thread = on ? sthread_get_current_thread_id() : 0;
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
	mtgs_worker_hold(1);
	GSGameChanged();
	mtgs_worker_hold(0);
}

void MTGS::ApplySettings()
{
	// We need to synchronize the thread when changing any settings when the download mode
	// is unsynchronized, because otherwise we might potentially read in the middle of
	// the GS renderer being reopened.
	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false);
	/* The worker is parked outside the renderer while its config moves. */
	mtgs_worker_hold(1);
	GSUpdateConfig(EmuConfig.GS, hw_render.context_type);
	mtgs_worker_hold(0);
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
	work_eventcount_notify(&MTGS::s_sem_event);
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
	work_eventcount_notify(&MTGS::s_sem_event);
}

