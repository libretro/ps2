/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#pragma once

#include "common/boost_spsc_queue.hpp"
#include "common/General.h"
#include "common/Threading.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "GSVertexSW.h"
#include "GSDrawScanline.h"
#include "../../GSAlignedClass.h"
#include "../../GSRingHeap.h"
#include "../../MultiISA.h"

template <class T, int CAPACITY>
class GSJobQueue final
{
private:
	std::thread m_thread;
	std::function<void()> m_startup;
	std::function<void(T&)> m_func;
	std::function<void()> m_shutdown;
	bool m_exit;
	ringbuffer_base<T, CAPACITY> m_queue;

	Threading::WorkSema m_sema;

	void ThreadProc()
	{
		if (m_startup)
			m_startup();

		for (;;)
		{
			m_sema.WaitForWork();
			if (m_exit)
				break;
			while (m_queue.consume_one(*this))
				;
		}

		if (m_shutdown)
			m_shutdown();
	}

public:
	GSJobQueue(std::function<void()> startup, std::function<void(T&)> func, std::function<void()> shutdown)
		: m_startup(std::move(startup))
		, m_func(std::move(func))
		, m_shutdown(std::move(shutdown))
		, m_exit(false)
	{
		m_thread = std::thread(&GSJobQueue::ThreadProc, this);
	}

	~GSJobQueue()
	{
		m_exit = true;
		m_sema.NotifyOfWork();
		m_thread.join();
	}

	bool IsEmpty()
	{
		return m_queue.empty();
	}

	void Push(const T& item)
	{
		while (!m_queue.push(item))
			std::this_thread::yield();
		m_sema.NotifyOfWork();
	}

	void Wait()
	{
		m_sema.WaitForEmpty();
	}

	void operator()(T& item)
	{
		m_func(item);
	}
};

MULTI_ISA_UNSHARED_START

class GSDrawScanline;

class alignas(32) GSRasterizerData : public GSAlignedClass<32>
{
public:
	GSVector4i scissor;
	GSVector4i bbox;
	GS_PRIM_CLASS primclass;
	u8* buff;
	GSVertexSW* vertex;
	int vertex_count;
	u16* index;
	int index_count;
	u64 start;
	int pixels;
	int counter;
	u8 scanmsk_value;

	GSScanlineGlobalData global;

	GSDrawScanline::SetupPrimPtr setup_prim;
	GSDrawScanline::DrawScanlinePtr draw_scanline;
	GSDrawScanline::DrawScanlinePtr draw_edge;

	GSRasterizerData()
		: scissor(GSVector4i::zero())
		, bbox(GSVector4i::zero())
		, primclass(GS_INVALID_CLASS)
		, buff(nullptr)
		, vertex(NULL)
		, vertex_count(0)
		, index(NULL)
		, index_count(0)
		, start(0)
		, pixels(0)
		, scanmsk_value(0)
	{
	}

	virtual ~GSRasterizerData()
	{
		if (buff != NULL)
			GSRingHeap::free(buff);
	}
};

class alignas(32) GSRasterizer final : public GSVirtualAlignedClass<32>
{
protected:
	GSDrawScanline* m_ds;
	int m_id;
	int m_threads;
	int m_thread_height;
	u8* m_scanline;
	u8 m_scanmsk_value;
	GSVector4i m_scissor;
	GSVector4 m_fscissor_x;
	GSVector4 m_fscissor_y;
	struct { GSVertexSW* buff; int count; } m_edge;
	struct { int sum, actual, total; } m_pixels;

	// For the current draw.
	GSScanlineLocalData m_local = {};
	GSDrawScanline::SetupPrimPtr m_setup_prim = nullptr;
	GSDrawScanline::DrawScanlinePtr m_draw_scanline = nullptr;
	GSDrawScanline::DrawScanlinePtr m_draw_edge = nullptr;

	__forceinline bool HasEdge() const { return (m_draw_edge != nullptr); }

	template <bool scissor_test>
	void DrawPoint(const GSVertexSW* vertex, int vertex_count, const u16* index, int index_count);
	void DrawLine(const GSVertexSW* vertex, const u16* index);
	void DrawTriangle(const GSVertexSW* vertex, const u16* index);
	void DrawSprite(const GSVertexSW* vertex, const u16* index);

#if _M_SSE >= 0x501
	__forceinline void DrawTriangleSection(int top, int bottom, GSVertexSW2& RESTRICT edge, const GSVertexSW2& RESTRICT dedge, const GSVertexSW2& RESTRICT dscan, const GSVector4& RESTRICT p0);
#else
	__forceinline void DrawTriangleSection(int top, int bottom, GSVertexSW& RESTRICT edge, const GSVertexSW& RESTRICT dedge, const GSVertexSW& RESTRICT dscan, const GSVector4& RESTRICT p0);
#endif

	void DrawEdge(const GSVertexSW& v0, const GSVertexSW& v1, const GSVertexSW& dv, int orientation, int side);

	__forceinline void AddScanline(GSVertexSW* e, int pixels, int left, int top, const GSVertexSW& scan);
	__forceinline void Flush(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, bool edge = false);

	__forceinline void DrawScanline(int pixels, int left, int top, const GSVertexSW& scan);
	__forceinline void DrawEdge(int pixels, int left, int top, const GSVertexSW& scan);

public:
	GSRasterizer(GSDrawScanline* ds, int id, int threads);
	~GSRasterizer();

	__forceinline bool IsOneOfMyScanlines(int top) const;
	__forceinline bool IsOneOfMyScanlines(int top, int bottom) const;
	__forceinline int FindMyNextScanline(int top) const;

	void Draw(GSRasterizerData& data);
	int GetPixels(bool reset);
};

class IRasterizer : public GSVirtualAlignedClass<32>
{
public:
	virtual ~IRasterizer() {}

	virtual void Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data) = 0;
	virtual void Sync() = 0;
	virtual bool IsSynced() const = 0;
	virtual int GetPixels(bool reset = true) = 0;
};

class GSSingleRasterizer final : public IRasterizer
{
public:
	GSSingleRasterizer();
	~GSSingleRasterizer() override;

	void Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data) override;
	void Sync() override;
	bool IsSynced() const override;
	int GetPixels(bool reset = true) override;

	void Draw(GSRasterizerData& data);

private:
	GSDrawScanline m_ds;
	GSRasterizer m_r;
};

class GSRasterizerList final : public IRasterizer
{
protected:
	using GSWorker = GSJobQueue<GSRingHeap::SharedPtr<GSRasterizerData>, 65536>;

	GSDrawScanline m_ds;

	// Worker threads depend on the rasterizers, so don't change the order.
	std::vector<std::unique_ptr<GSRasterizer>> m_r;
	std::vector<std::unique_ptr<GSWorker>> m_workers;
	u8* m_scanline;
	int m_thread_height;

	GSRasterizerList(int threads);

	static void OnWorkerStartup(int i);
	static void OnWorkerShutdown(int i);

public:
	~GSRasterizerList() override;

	static std::unique_ptr<IRasterizer> Create(int threads);

	// IRasterizer

	void Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data) override;
	void Sync() override;
	bool IsSynced() const override;
	int GetPixels(bool reset) override;
};

MULTI_ISA_UNSHARED_END
