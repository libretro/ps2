// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "videodev.h"

#include <retro_atomic.h>

namespace usb_eyetoy
{
	// The largest picture the camera hands over, and so the largest encoded frame.
	static constexpr u32 MAX_FRAME_SIZE = 640 * 480 * 3;

	// Both resample to the dst size by nearest neighbour; mirror flips left and right.
	void XrgbToRgb(const u32* src, int src_w, int src_h, size_t src_pitch, u8* dst, int dst_w, int dst_h, bool mirror);
	void YuyvToRgb(const u8* src, int src_w, int src_h, u8* dst, int dst_w, int dst_h, bool mirror);

	// The scratch an encode needs, kept by whoever encodes rather than on the stack.
	struct FrameEncoder
	{
		float mpeg_work[256 * 3 + 64 * 2];
	};

	// rgb is width x height RGB24; returns the encoded size in out, which holds MAX_FRAME_SIZE.
	u32 EncodeFrame(FrameEncoder* enc, u8* out, const u8* rgb, int width, int height, FrameFormat format);

	// A gradient, for when no camera delivers a picture.
	u32 EncodeTestPattern(u8* out, int width, int height, FrameFormat format);

	// One encoded frame handed from the thread that captures to the thread that
	// runs the device, without a lock: three buffers, of which the producer owns
	// one, the consumer owns one, and the third sits in a slot both exchange.
	// A frame published while the consumer is away is overwritten by the next,
	// so the consumer always takes the newest. Single producer, single consumer.
	struct FrameMailbox
	{
		static constexpr int FRESH = 4;

		struct Buffer
		{
			u8* data;
			u32 size;
			u32 generation;
		};

		Buffer buf[3];
		retro_atomic_int_t slot;
		int work;
		int hold;

		// Three buffers of MAX_FRAME_SIZE from one allocation; false if that failed.
		bool Init();
		void Free();
		// Nothing in flight: the consumer's view starts empty. Only while no producer runs.
		void Reset();

		// Producer: where to encode into.
		u8* Work() const { return buf[work].data; }
		// Producer: what Work() holds is the frame, now visible to the consumer.
		void Publish(u32 size, u32 generation);
		// Consumer: the newest frame published since the last Take, or NULL.
		// The buffer stays the consumer's until the next Take.
		const Buffer* Take();
	};

	// 16-bit samples from the thread that captures to the thread that runs the
	// device, without a lock: a single-producer single-consumer ring. Full, the
	// producer drops what it has; the consumer trims a backlog itself.
	struct SampleRing
	{
		static constexpr u32 SIZE = 2048;
		static constexpr u32 MASK = SIZE - 1;

		s16 data[SIZE];
		retro_atomic_int_t head; // consumer's, next to read
		retro_atomic_int_t tail; // producer's, next to write

		void Reset();
		// Producer: room for how many more.
		u32 Space();
		// Producer: adds one; false and nothing added when full.
		bool Push(s16 sample);
		// Consumer: how many are waiting.
		u32 Count();
		// Consumer: drops all but the newest keep.
		void TrimTo(u32 keep);
		// Consumer: takes up to max; returns how many.
		u32 Pop(s16* samples, u32 max);
	};
} // namespace usb_eyetoy
