// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "eyetoy-host.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include "cam-frame.h"

#include <rthreads/rthreads.h>
#include <retro_atomic.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace usb_eyetoy
{
	namespace
	{
		int xioctl(int fd, unsigned long request, void* arg)
		{
			int r;
			do
				r = ioctl(fd, request, arg);
			while (r == -1 && errno == EINTR);
			return r;
		}

		// The first node that streams video capture: a UVC camera's metadata node, or a codec, does not.
		int OpenCaptureDevice()
		{
			for (int index = 0; index < 64; index++)
			{
				char name[32];
				snprintf(name, sizeof(name), "/dev/video%d", index);
				const int fd = open(name, O_RDWR | O_NONBLOCK, 0);
				if (fd < 0)
					continue;
				v4l2_capability cap = {};
				if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
				{
					const u32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
					if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING))
					{
						log_cb(RETRO_LOG_INFO, "EyeToy: camera %s (%s)\n", name, reinterpret_cast<const char*>(cap.card));
						return fd;
					}
				}
				close(fd);
			}
			return -1;
		}

		class V4L2VideoDevice final : public VideoDevice
		{
		public:
			V4L2VideoDevice() {}

			~V4L2VideoDevice() override
			{
				Close();
				m_frames.Free();
				free(m_rgb);
				free(m_pattern);
			}

			int Open(int width, int height, FrameFormat format, int mirror) override
			{
				Close();
				if (!m_frames.buf[0].data && !m_frames.Init())
					return -1;
				if (!m_rgb)
					m_rgb = static_cast<u8*>(malloc(MAX_FRAME_SIZE + sizeof(FrameEncoder)));
				if (!m_pattern)
					m_pattern = static_cast<u8*>(malloc(MAX_FRAME_SIZE));
				if (!m_rgb || !m_pattern)
					return -1;

				m_width = width;
				m_height = height;
				m_format = format;
				retro_atomic_store_release_int(&m_mirror, mirror);
				// No capture thread runs here, so the mailbox is the device's to clear.
				m_frames.Reset();
				// Until the camera delivers, the game gets one picture so it sees the device working.
				m_pattern_size = EncodeTestPattern(m_pattern, width, height, format);

				if (!StartCapture())
				{
					StopCapture();
					return -1;
				}
				retro_atomic_store_release_int(&m_running, 1);
				m_thread = sthread_create(CaptureThread, this);
				if (!m_thread)
				{
					retro_atomic_store_release_int(&m_running, 0);
					StopCapture();
					return -1;
				}
				return 0;
			}

			int Close() override
			{
				if (m_thread)
				{
					retro_atomic_store_release_int(&m_running, 0);
					sthread_join(m_thread);
					m_thread = nullptr;
				}
				StopCapture();
				return 0;
			}

			int GetImage(uint8_t* buf, size_t len) override
			{
				const FrameMailbox::Buffer* frame = m_frames.Take();
				u32 size = 0;
				if (frame)
				{
					size = frame->size < len ? frame->size : static_cast<u32>(len);
					memcpy(buf, frame->data, size);
					m_pattern_size = 0;
				}
				else if (m_pattern_size)
				{
					size = m_pattern_size < len ? m_pattern_size : static_cast<u32>(len);
					memcpy(buf, m_pattern, size);
					m_pattern_size = 0;
				}
				return static_cast<int>(size);
			}

			void SetMirroring(bool state) override { retro_atomic_store_release_int(&m_mirror, state); }

		private:
			bool StartCapture()
			{
				m_fd = OpenCaptureDevice();
				if (m_fd < 0)
				{
					log_cb(RETRO_LOG_WARN, "EyeToy: no camera found\n");
					return false;
				}

				v4l2_format fmt = {};
				fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				fmt.fmt.pix.width = m_width;
				fmt.fmt.pix.height = m_height;
				fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
				if (xioctl(m_fd, VIDIOC_S_FMT, &fmt) == -1 || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
				{
					log_cb(RETRO_LOG_WARN, "EyeToy: the camera does not offer YUYV\n");
					return false;
				}
				// Cameras that cannot capture the game's size give another; frames are scaled to fit.
				m_capture_width = fmt.fmt.pix.width;
				m_capture_height = fmt.fmt.pix.height;

				v4l2_requestbuffers req = {};
				req.count = 4;
				req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				req.memory = V4L2_MEMORY_MMAP;
				if (xioctl(m_fd, VIDIOC_REQBUFS, &req) == -1 || req.count < 2)
					return false;
				for (u32 i = 0; i < req.count; i++)
				{
					v4l2_buffer buf = {};
					buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
					buf.memory = V4L2_MEMORY_MMAP;
					buf.index = i;
					if (xioctl(m_fd, VIDIOC_QUERYBUF, &buf) == -1)
						return false;
					void* start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, buf.m.offset);
					if (start == MAP_FAILED)
						return false;
					m_buffers.push_back({start, buf.length});
					if (xioctl(m_fd, VIDIOC_QBUF, &buf) == -1)
						return false;
				}
				v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				return xioctl(m_fd, VIDIOC_STREAMON, &type) == 0;
			}

			void StopCapture()
			{
				if (m_fd < 0)
					return;
				v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				xioctl(m_fd, VIDIOC_STREAMOFF, &type);
				for (const Buffer& b : m_buffers)
					munmap(b.start, b.length);
				m_buffers.clear();
				close(m_fd);
				m_fd = -1;
			}

			static void CaptureThread(void* arg)
			{
				V4L2VideoDevice* self = static_cast<V4L2VideoDevice*>(arg);
				FrameEncoder* const enc = reinterpret_cast<FrameEncoder*>(self->m_rgb + MAX_FRAME_SIZE);
				while (retro_atomic_load_acquire_int(&self->m_running))
				{
					fd_set fds;
					FD_ZERO(&fds);
					FD_SET(self->m_fd, &fds);
					timeval timeout = {0, 100000};
					const int ret = select(self->m_fd + 1, &fds, nullptr, nullptr, &timeout);
					if (ret < 0 && errno != EINTR)
						break;
					if (ret <= 0)
						continue;

					v4l2_buffer buf = {};
					buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
					buf.memory = V4L2_MEMORY_MMAP;
					if (xioctl(self->m_fd, VIDIOC_DQBUF, &buf) == -1)
					{
						if (errno == EAGAIN)
							continue;
						log_cb(RETRO_LOG_WARN, "EyeToy: the camera stopped (%s)\n", strerror(errno));
						break;
					}
					if (buf.bytesused >= static_cast<u32>(self->m_capture_width * self->m_capture_height * 2))
					{
						YuyvToRgb(static_cast<const u8*>(self->m_buffers[buf.index].start), self->m_capture_width,
							self->m_capture_height, self->m_rgb, self->m_width, self->m_height,
							retro_atomic_load_acquire_int(&self->m_mirror));
						const u32 size = EncodeFrame(enc, self->m_frames.Work(), self->m_rgb, self->m_width, self->m_height, self->m_format);
						self->m_frames.Publish(size, 0);
					}
					xioctl(self->m_fd, VIDIOC_QBUF, &buf);
				}
			}

			struct Buffer
			{
				void* start;
				size_t length;
			};

			sthread_t* m_thread = nullptr;
			retro_atomic_int_t m_running = 0;
			retro_atomic_int_t m_mirror = 0;
			int m_fd = -1;
			std::vector<Buffer> m_buffers;
			int m_width = 0, m_height = 0;
			int m_capture_width = 0, m_capture_height = 0;
			FrameFormat m_format = format_mpeg;
			// The RGB conversion of a capture, with the encoder's scratch behind it.
			u8* m_rgb = nullptr;
			u8* m_pattern = nullptr;
			u32 m_pattern_size = 0;
			FrameMailbox m_frames = {};
		};

		// libasound is loaded when a microphone is wanted, so the core does not need it to run.
		struct snd_pcm_t;
		struct Alsa
		{
			int (*pcm_open)(snd_pcm_t**, const char*, int, int);
			int (*pcm_set_params)(snd_pcm_t*, int, int, unsigned int, unsigned int, int, unsigned int);
			int (*pcm_start)(snd_pcm_t*);
			long (*pcm_readi)(snd_pcm_t*, void*, unsigned long);
			int (*pcm_recover)(snd_pcm_t*, int, int);
			int (*pcm_close)(snd_pcm_t*);
			const char* (*strerror)(int);
		};

		// Stable ALSA ABI values.
		constexpr int SND_PCM_STREAM_CAPTURE = 1;
		constexpr int SND_PCM_NONBLOCK = 1;
		constexpr int SND_PCM_ACCESS_RW_INTERLEAVED = 3;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
		constexpr int SND_PCM_FORMAT_S16 = 3;
#else
		constexpr int SND_PCM_FORMAT_S16 = 2;
#endif

		const Alsa* LoadAlsa()
		{
			static Alsa alsa;
			static bool tried, loaded;
			if (tried)
				return loaded ? &alsa : nullptr;
			tried = true;
			void* lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
			if (!lib)
				return nullptr;
			alsa.pcm_open = reinterpret_cast<decltype(alsa.pcm_open)>(dlsym(lib, "snd_pcm_open"));
			alsa.pcm_set_params = reinterpret_cast<decltype(alsa.pcm_set_params)>(dlsym(lib, "snd_pcm_set_params"));
			alsa.pcm_start = reinterpret_cast<decltype(alsa.pcm_start)>(dlsym(lib, "snd_pcm_start"));
			alsa.pcm_readi = reinterpret_cast<decltype(alsa.pcm_readi)>(dlsym(lib, "snd_pcm_readi"));
			alsa.pcm_recover = reinterpret_cast<decltype(alsa.pcm_recover)>(dlsym(lib, "snd_pcm_recover"));
			alsa.pcm_close = reinterpret_cast<decltype(alsa.pcm_close)>(dlsym(lib, "snd_pcm_close"));
			alsa.strerror = reinterpret_cast<decltype(alsa.strerror)>(dlsym(lib, "snd_strerror"));
			loaded = alsa.pcm_open && alsa.pcm_set_params && alsa.pcm_start && alsa.pcm_readi && alsa.pcm_recover &&
				alsa.pcm_close && alsa.strerror;
			if (!loaded)
				dlclose(lib);
			return loaded ? &alsa : nullptr;
		}

		// realpath allocates, so no PATH_MAX buffer sits on the stack.
		std::string RealPath(const std::string& path)
		{
			char* real = realpath(path.c_str(), nullptr);
			if (!real)
				return {};
			std::string s(real);
			free(real);
			return s;
		}

		std::string UsbDeviceOf(const std::string& sysfs_link)
		{
			std::string real = RealPath(sysfs_link);
			if (real.empty())
				return {};
			// The link names a USB interface; its parent is the device.
			return dirname(&real[0]);
		}

		bool Exists(const std::string& path)
		{
			struct stat st;
			return stat(path.c_str(), &st) == 0;
		}

		// The microphone on the same USB device as a camera, else any USB microphone.
		std::string FindCaptureDevice(const std::string& sys, const std::string& proc)
		{
			std::vector<std::string> cameras;
			if (DIR* d = opendir((sys + "/class/video4linux").c_str()))
			{
				while (dirent* e = readdir(d))
				{
					if (e->d_name[0] == '.')
						continue;
					const std::string usb = UsbDeviceOf(sys + "/class/video4linux/" + e->d_name + "/device");
					if (!usb.empty())
						cameras.push_back(usb);
				}
				closedir(d);
			}

			std::string usb_microphone;
			for (int card = 0; card < 32; card++)
			{
				int pcm = 0;
				while (pcm < 32 && !Exists(proc + "/asound/card" + std::to_string(card) + "/pcm" + std::to_string(pcm) + "c"))
					pcm++;
				if (pcm == 32)
					continue;

				const std::string link = sys + "/class/sound/card" + std::to_string(card) + "/device";
				const std::string real = RealPath(link);
				if (real.empty())
					continue;
				const std::string name = "plughw:" + std::to_string(card) + "," + std::to_string(pcm);
				const std::string usb = UsbDeviceOf(link);
				for (const std::string& camera : cameras)
				{
					if (camera == usb)
						return name;
				}
				if (usb_microphone.empty() && real.find("/usb") != std::string::npos)
					usb_microphone = name;
			}
			return usb_microphone;
		}

		class AlsaMicSource final : public MicSource
		{
		public:
			AlsaMicSource(const Alsa* alsa, snd_pcm_t* pcm)
				: m_alsa(alsa)
				, m_pcm(pcm)
			{
			}

			~AlsaMicSource() override { m_alsa->pcm_close(m_pcm); }

			// Non-blocking reads as the game asks; an overrun from a game that stopped listening
			// restarts the stream rather than playing back what it missed.
			u32 Read(s16* samples, u32 max) override
			{
				const long n = m_alsa->pcm_readi(m_pcm, samples, max);
				if (n >= 0)
					return static_cast<u32>(n);
				if (n != -EAGAIN && m_alsa->pcm_recover(m_pcm, static_cast<int>(n), 1) == 0)
					m_alsa->pcm_start(m_pcm);
				return 0;
			}

		private:
			const Alsa* m_alsa;
			snd_pcm_t* m_pcm;
		};
	} // namespace

	std::unique_ptr<VideoDevice> CreateHostVideoDevice()
	{
		return std::make_unique<V4L2VideoDevice>();
	}

	std::unique_ptr<MicSource> CreateHostMicSource()
	{
		const std::string name = FindCaptureDevice("/sys", "/proc");
		if (name.empty())
		{
			log_cb(RETRO_LOG_INFO, "EyeToy: no microphone found\n");
			return nullptr;
		}
		const Alsa* alsa = LoadAlsa();
		if (!alsa)
		{
			log_cb(RETRO_LOG_WARN, "EyeToy: libasound.so.2 is not available\n");
			return nullptr;
		}
		snd_pcm_t* pcm = nullptr;
		int err = alsa->pcm_open(&pcm, name.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
		if (err == 0)
		{
			err = alsa->pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1, 100000);
			if (err == 0)
				err = alsa->pcm_start(pcm);
			if (err != 0)
				alsa->pcm_close(pcm);
		}
		if (err != 0)
		{
			log_cb(RETRO_LOG_WARN, "EyeToy: cannot capture from %s: %s\n", name.c_str(), alsa->strerror(err));
			return nullptr;
		}
		log_cb(RETRO_LOG_INFO, "EyeToy: microphone %s\n", name.c_str());
		return std::make_unique<AlsaMicSource>(alsa, pcm);
	}
} // namespace usb_eyetoy

#else

namespace usb_eyetoy
{
	std::unique_ptr<VideoDevice> CreateHostVideoDevice()
	{
		return nullptr;
	}

	std::unique_ptr<MicSource> CreateHostMicSource()
	{
		return nullptr;
	}
} // namespace usb_eyetoy

#endif
