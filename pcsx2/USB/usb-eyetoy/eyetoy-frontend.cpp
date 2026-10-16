// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "eyetoy-frontend.h"
#include "eyetoy-host.h"
#include "cam-frame.h"

#include <rthreads/rthreads.h>

#include <stdlib.h>
#include <string.h>

namespace usb_eyetoy
{
	namespace
	{
		constexpr u32 MIC_RATE = 16000;
		// About 128 ms; once full, the oldest samples go.
		constexpr u32 MIC_RING = 2048;
		// A game that starts or resumes listening hears the present: a backlog past 75 ms is cut to
		// 20 ms, which leaves room for a frame's worth of samples arriving at once.
		constexpr u32 MIC_TRIM_ABOVE = 1200;
		constexpr u32 MIC_TRIM_TO = 320;

		// Everything below is shared between the main thread and the emulator's, under s_lock,
		// except where noted.
		slock_t* s_lock;

		bool s_cam_available;
		retro_camera_callback s_cam;
		bool s_cam_wanted;
		int s_cam_width;
		int s_cam_height;
		FrameFormat s_cam_format;
		bool s_cam_mirror;
		// Bumped by every Open, so a frame encoded for the previous settings is not kept.
		u32 s_cam_generation;
		u8* s_frame;
		u32 s_frame_size;
		// Main thread only.
		bool s_cam_running;
		bool s_cam_failed;
		u8* s_work_frame;
		u8* s_work_rgb;

		bool s_mic_available;
		retro_microphone_interface s_mic_if;
		bool s_mic_wanted;
		s16 s_ring[MIC_RING];
		u32 s_ring_start;
		u32 s_ring_count;
		// Main thread only.
		retro_microphone_t* s_mic;
		unsigned s_mic_rate;
		bool s_mic_failed;
		// Resampling position in the current input chunk, in 1/MIC_RATE of a frontend sample.
		u64 s_mic_phase;

		void RETRO_CALLCONV OnCameraFrame(const uint32_t* buffer, unsigned width, unsigned height, size_t pitch)
		{
			slock_lock(s_lock);
			const bool wanted = s_cam_wanted;
			const int w = s_cam_width, h = s_cam_height;
			const FrameFormat format = s_cam_format;
			const bool mirror = s_cam_mirror;
			const u32 generation = s_cam_generation;
			slock_unlock(s_lock);
			if (!wanted || !buffer || !width || !height)
				return;

			XrgbToRgb(buffer, width, height, pitch, s_work_rgb, w, h, mirror);
			const u32 size = EncodeFrame(s_work_frame, s_work_rgb, w, h, format);

			slock_lock(s_lock);
			if (generation == s_cam_generation)
			{
				u8* const shared = s_frame;
				s_frame = s_work_frame;
				s_frame_size = size;
				s_work_frame = shared;
			}
			slock_unlock(s_lock);
		}

		class FrontendVideoDevice final : public VideoDevice
		{
		public:
			int Open(int width, int height, FrameFormat format, int mirror) override
			{
				slock_lock(s_lock);
				s_cam_width = width;
				s_cam_height = height;
				s_cam_format = format;
				s_cam_mirror = mirror;
				s_cam_generation++;
				s_frame_size = EncodeTestPattern(s_frame, width, height, format);
				s_cam_wanted = true;
				slock_unlock(s_lock);
				return 0;
			}

			int Close() override
			{
				slock_lock(s_lock);
				s_cam_wanted = false;
				slock_unlock(s_lock);
				return 0;
			}

			int GetImage(uint8_t* buf, size_t len) override
			{
				slock_lock(s_lock);
				const u32 size = s_frame_size < len ? s_frame_size : static_cast<u32>(len);
				memcpy(buf, s_frame, size);
				s_frame_size = 0;
				slock_unlock(s_lock);
				return static_cast<int>(size);
			}

			void SetMirroring(bool state) override
			{
				slock_lock(s_lock);
				s_cam_mirror = state;
				slock_unlock(s_lock);
			}
		};

		// With no camera at all the game still gets one picture, so it sees the device working.
		class PatternVideoDevice final : public VideoDevice
		{
		public:
			int Open(int width, int height, FrameFormat format, int mirror) override
			{
				if (!m_frame)
					m_frame = static_cast<u8*>(malloc(MAX_FRAME_SIZE));
				m_size = m_frame ? EncodeTestPattern(m_frame, width, height, format) : 0;
				return 0;
			}

			int Close() override { return 0; }

			int GetImage(uint8_t* buf, size_t len) override
			{
				const u32 size = m_size < len ? m_size : static_cast<u32>(len);
				if (size)
					memcpy(buf, m_frame, size);
				m_size = 0;
				return static_cast<int>(size);
			}

			void SetMirroring(bool state) override {}

			~PatternVideoDevice() override { free(m_frame); }

		private:
			u8* m_frame = nullptr;
			u32 m_size = 0;
		};

		class FrontendMicSource final : public MicSource
		{
		public:
			FrontendMicSource()
			{
				slock_lock(s_lock);
				s_mic_wanted = true;
				s_ring_count = 0;
				slock_unlock(s_lock);
			}

			~FrontendMicSource() override
			{
				slock_lock(s_lock);
				s_mic_wanted = false;
				slock_unlock(s_lock);
			}

			u32 Read(s16* samples, u32 max) override
			{
				slock_lock(s_lock);
				if (s_ring_count > MIC_TRIM_ABOVE)
				{
					s_ring_start = (s_ring_start + s_ring_count - MIC_TRIM_TO) % MIC_RING;
					s_ring_count = MIC_TRIM_TO;
				}
				const u32 n = s_ring_count < max ? s_ring_count : max;
				for (u32 i = 0; i < n; i++)
					samples[i] = s_ring[(s_ring_start + i) % MIC_RING];
				s_ring_start = (s_ring_start + n) % MIC_RING;
				s_ring_count -= n;
				slock_unlock(s_lock);
				return n;
			}
		};

		void RunCamera()
		{
			slock_lock(s_lock);
			const bool wanted = s_cam_wanted;
			slock_unlock(s_lock);

			if (wanted && !s_cam_running && !s_cam_failed)
			{
				s_cam_running = s_cam.start && s_cam.start();
				s_cam_failed = !s_cam_running;
				if (s_cam_failed)
					log_cb(RETRO_LOG_WARN, "EyeToy: the frontend could not start its camera\n");
			}
			else if (!wanted)
			{
				if (s_cam_running && s_cam.stop)
					s_cam.stop();
				s_cam_running = false;
				s_cam_failed = false;
			}
		}

		void RunMicrophone()
		{
			slock_lock(s_lock);
			const bool wanted = s_mic_wanted;
			slock_unlock(s_lock);

			if (wanted && !s_mic && !s_mic_failed)
			{
				retro_microphone_params_t params = {MIC_RATE};
				s_mic = s_mic_if.open_mic(&params);
				if (s_mic && s_mic_if.get_params(s_mic, &params) && params.rate && s_mic_if.set_mic_state(s_mic, true))
				{
					s_mic_rate = params.rate;
					s_mic_phase = 0;
				}
				else
				{
					log_cb(RETRO_LOG_WARN, "EyeToy: the frontend could not open its microphone\n");
					if (s_mic)
						s_mic_if.close_mic(s_mic);
					s_mic = nullptr;
					s_mic_failed = true;
				}
			}
			else if (!wanted)
			{
				if (s_mic)
					s_mic_if.close_mic(s_mic);
				s_mic = nullptr;
				s_mic_failed = false;
			}
			if (!s_mic)
				return;

			s16 in[1024];
			int n;
			while ((n = s_mic_if.read_mic(s_mic, in, C89_ARRAY_SIZE(in))) > 0)
			{
				slock_lock(s_lock);
				// Nearest-sample resampling from the frontend's rate to the EyeToy's.
				const u64 end = static_cast<u64>(n) * MIC_RATE;
				for (; s_mic_phase < end; s_mic_phase += s_mic_rate)
				{
					if (s_ring_count == MIC_RING)
					{
						s_ring_start = (s_ring_start + 1) % MIC_RING;
						s_ring_count--;
					}
					s_ring[(s_ring_start + s_ring_count) % MIC_RING] = in[s_mic_phase / MIC_RATE];
					s_ring_count++;
				}
				s_mic_phase -= end;
				slock_unlock(s_lock);
				if (n < static_cast<int>(C89_ARRAY_SIZE(in)))
					break;
			}
		}
	} // namespace

	void FrontendLoad(retro_environment_t environ_cb)
	{
		if (!s_lock)
			s_lock = slock_new();
		if (!s_frame)
		{
			s_frame = static_cast<u8*>(malloc(MAX_FRAME_SIZE));
			s_work_frame = static_cast<u8*>(malloc(MAX_FRAME_SIZE));
			s_work_rgb = static_cast<u8*>(malloc(MAX_FRAME_SIZE));
		}

		memset(&s_cam, 0, sizeof(s_cam));
		s_cam.caps = 1 << RETRO_CAMERA_BUFFER_RAW_FRAMEBUFFER;
		s_cam.width = 640;
		s_cam.height = 480;
		s_cam.frame_raw_framebuffer = OnCameraFrame;
		s_cam_available = s_lock && s_frame && s_work_frame && s_work_rgb &&
			environ_cb(RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE, &s_cam) && s_cam.start;

		memset(&s_mic_if, 0, sizeof(s_mic_if));
		s_mic_if.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;
		s_mic_available = s_lock && environ_cb(RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE, &s_mic_if) &&
			s_mic_if.interface_version && s_mic_if.open_mic && s_mic_if.read_mic;

		log_cb(RETRO_LOG_INFO, "EyeToy: camera from the %s, microphone from the %s\n",
			s_cam_available ? "frontend" : "host", s_mic_available ? "frontend" : "host");
	}

	void FrontendRun()
	{
		if (s_cam_available)
			RunCamera();
		if (s_mic_available)
			RunMicrophone();
	}

	void FrontendUnload()
	{
		if (s_cam_running && s_cam.stop)
			s_cam.stop();
		s_cam_running = false;
		s_cam_failed = false;
		s_cam_available = false;
		if (s_mic)
			s_mic_if.close_mic(s_mic);
		s_mic = nullptr;
		s_mic_failed = false;
		s_mic_available = false;
	}

	std::unique_ptr<VideoDevice> VideoDevice::CreateInstance()
	{
		if (s_cam_available)
			return std::make_unique<FrontendVideoDevice>();
		if (std::unique_ptr<VideoDevice> host = CreateHostVideoDevice())
			return host;
		return std::make_unique<PatternVideoDevice>();
	}

	std::unique_ptr<MicSource> MicSource::CreateInstance()
	{
		if (s_mic_available)
			return std::make_unique<FrontendMicSource>();
		return CreateHostMicSource();
	}
} // namespace usb_eyetoy
