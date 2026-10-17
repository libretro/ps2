// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "eyetoy-frontend.h"
#include "eyetoy-host.h"
#include "cam-frame.h"

#include <retro_atomic.h>

#include <stdlib.h>
#include <string.h>

namespace usb_eyetoy
{
	namespace
	{
		constexpr u32 MIC_RATE = 16000;
		// A game that starts or resumes listening hears the present: a backlog past 75 ms is cut to
		// 20 ms, which leaves room for a frame's worth of samples arriving at once.
		constexpr u32 MIC_TRIM_ABOVE = 1200;
		constexpr u32 MIC_TRIM_TO = 320;

		// What the device asked of the camera, in one word the main thread reads
		// and the emulator thread writes, so the two never need a lock:
		//   bit 0      wanted
		//   bits 1-2   FrameFormat
		//   bit 3      mirror
		//   bits 4-9   width / 16   (the OV519 takes the width as a count of 16)
		//   bits 10-15 height / 8   (and the height as a count of 8)
		//   bits 16-30 generation, bumped by every Open so an older frame is told apart
		constexpr int CAM_WANTED = 1;
		constexpr int CAM_FORMAT_SHIFT = 1;
		constexpr int CAM_MIRROR = 1 << 3;
		constexpr int CAM_W_SHIFT = 4;
		constexpr int CAM_H_SHIFT = 10;
		constexpr int CAM_GEN_SHIFT = 16;
		constexpr int CAM_GEN_MASK = 0x7FFF;

		int CamConfig(bool wanted, FrameFormat format, bool mirror, int width, int height, u32 generation)
		{
			return (wanted ? CAM_WANTED : 0) | (static_cast<int>(format) << CAM_FORMAT_SHIFT) | (mirror ? CAM_MIRROR : 0) |
				((width / 16) << CAM_W_SHIFT) | ((height / 8) << CAM_H_SHIFT) |
				(static_cast<int>(generation & CAM_GEN_MASK) << CAM_GEN_SHIFT);
		}

		// Shared between the main thread and the emulator's: the atomics, the
		// mailbox and the ring, each built for exactly that.
		retro_atomic_int_t s_cam_config;
		FrameMailbox s_frames;
		retro_atomic_int_t s_mic_wanted;
		SampleRing s_ring;

		// Main thread only.
		bool s_cam_available;
		retro_camera_callback s_cam;
		bool s_cam_running;
		bool s_cam_failed;
		u8* s_work_rgb;
		FrameEncoder* s_encoder;

		bool s_mic_available;
		retro_microphone_interface s_mic_if;
		retro_microphone_t* s_mic;
		unsigned s_mic_rate;
		bool s_mic_failed;
		// Resampling position in the current input chunk, in 1/MIC_RATE of a frontend sample.
		u64 s_mic_phase;

		void RETRO_CALLCONV OnCameraFrame(const uint32_t* buffer, unsigned width, unsigned height, size_t pitch)
		{
			const int config = retro_atomic_load_acquire_int(&s_cam_config);
			if (!(config & CAM_WANTED) || !buffer || !width || !height)
				return;

			const int w = ((config >> CAM_W_SHIFT) & 0x3F) * 16;
			const int h = ((config >> CAM_H_SHIFT) & 0x3F) * 8;
			const FrameFormat format = static_cast<FrameFormat>((config >> CAM_FORMAT_SHIFT) & 3);
			const u32 generation = static_cast<u32>(config >> CAM_GEN_SHIFT) & CAM_GEN_MASK;

			XrgbToRgb(buffer, width, height, pitch, s_work_rgb, w, h, (config & CAM_MIRROR) != 0);
			const u32 size = EncodeFrame(s_encoder, s_frames.Work(), s_work_rgb, w, h, format);
			s_frames.Publish(size, generation);
		}

		class FrontendVideoDevice final : public VideoDevice
		{
		public:
			FrontendVideoDevice()
			{
				m_pattern = static_cast<u8*>(malloc(MAX_FRAME_SIZE));
				// Past whatever a previous device published, so its frames are told apart.
				m_generation = static_cast<u32>(retro_atomic_load_relaxed_int(&s_cam_config) >> CAM_GEN_SHIFT) & CAM_GEN_MASK;
			}

			~FrontendVideoDevice() override
			{
				Close();
				free(m_pattern);
			}

			int Open(int width, int height, FrameFormat format, int mirror) override
			{
				m_mirror = mirror != 0;
				m_generation = (m_generation + 1) & CAM_GEN_MASK;
				retro_atomic_store_release_int(&s_cam_config, CamConfig(true, format, m_mirror, width, height, m_generation));
				// Until the frontend's camera delivers, the game gets one picture so it sees the device working.
				m_pattern_size = m_pattern ? EncodeTestPattern(m_pattern, width, height, format) : 0;
				return 0;
			}

			int Close() override
			{
				retro_atomic_store_release_int(&s_cam_config, CamConfig(false, format_mpeg, m_mirror, 16, 8, m_generation));
				return 0;
			}

			int GetImage(uint8_t* buf, size_t len) override
			{
				const FrameMailbox::Buffer* frame = s_frames.Take();
				u32 size = 0;
				if (frame && frame->generation == m_generation)
				{
					size = frame->size;
					m_pattern_size = 0;
					if (size > len)
						size = static_cast<u32>(len);
					memcpy(buf, frame->data, size);
				}
				else if (m_pattern_size)
				{
					size = m_pattern_size < len ? m_pattern_size : static_cast<u32>(len);
					memcpy(buf, m_pattern, size);
					m_pattern_size = 0;
				}
				return static_cast<int>(size);
			}

			void SetMirroring(bool state) override
			{
				const int config = retro_atomic_load_relaxed_int(&s_cam_config);
				m_mirror = state;
				retro_atomic_store_release_int(&s_cam_config, state ? (config | CAM_MIRROR) : (config & ~CAM_MIRROR));
			}

		private:
			u8* m_pattern = nullptr;
			u32 m_pattern_size = 0;
			u32 m_generation = 0;
			bool m_mirror = false;
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
				retro_atomic_store_release_int(&s_mic_wanted, 1);
			}

			~FrontendMicSource() override
			{
				retro_atomic_store_release_int(&s_mic_wanted, 0);
			}

			u32 Read(s16* samples, u32 max) override
			{
				if (s_ring.Count() > MIC_TRIM_ABOVE)
					s_ring.TrimTo(MIC_TRIM_TO);
				return s_ring.Pop(samples, max);
			}
		};

		void RunCamera()
		{
			const bool wanted = (retro_atomic_load_acquire_int(&s_cam_config) & CAM_WANTED) != 0;

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
			const bool wanted = retro_atomic_load_acquire_int(&s_mic_wanted) != 0;

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
				// Nearest-sample resampling from the frontend's rate to the EyeToy's. A full
				// ring drops the rest of the chunk: the game is not listening, and when it
				// does again it trims to the newest anyway.
				const u64 end = static_cast<u64>(n) * MIC_RATE;
				for (; s_mic_phase < end; s_mic_phase += s_mic_rate)
				{
					if (!s_ring.Push(in[s_mic_phase / MIC_RATE]))
					{
						s_mic_phase = end;
						break;
					}
				}
				s_mic_phase -= end;
				if (n < static_cast<int>(C89_ARRAY_SIZE(in)))
					break;
			}
		}
	} // namespace

	void FrontendLoad(retro_environment_t environ_cb)
	{
		if (!s_work_rgb)
		{
			s_work_rgb = static_cast<u8*>(malloc(MAX_FRAME_SIZE));
			s_encoder = static_cast<FrameEncoder*>(malloc(sizeof(FrameEncoder)));
			if (!s_frames.Init())
				s_frames.Free();
		}
		retro_atomic_int_init(&s_cam_config, 0);
		retro_atomic_int_init(&s_mic_wanted, 0);
		s_frames.Reset();
		s_ring.Reset();

		memset(&s_cam, 0, sizeof(s_cam));
		s_cam.caps = 1 << RETRO_CAMERA_BUFFER_RAW_FRAMEBUFFER;
		s_cam.width = 640;
		s_cam.height = 480;
		s_cam.frame_raw_framebuffer = OnCameraFrame;
		s_cam_available = s_work_rgb && s_encoder && s_frames.buf[0].data &&
			environ_cb(RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE, &s_cam) && s_cam.start;

		memset(&s_mic_if, 0, sizeof(s_mic_if));
		s_mic_if.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;
		s_mic_available = environ_cb(RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE, &s_mic_if) &&
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
