/* Copyright (c) 2017-2022 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "thread_id.hpp"
#include "logging.hpp"
#include "thread_prims.hpp"
#include <rthreads/rthreads.h>

namespace Util
{
static thread_local unsigned thread_id_to_index = ~0u;
/* The device generation thread_id_to_index was handed out under. Each
 * device starts a generation with its own pools, so an index from an
 * earlier one is given up and the thread asks again. */
static thread_local unsigned thread_id_generation = 0;

/* Index 0 belongs to the thread that set up the device (it registers
 * itself); the counter hands out 1 upward, afresh for every device. */
static PGS::atomic_uint32_t next_thread_index(1);
static PGS::atomic_uint32_t thread_index_count(1);
static PGS::atomic_uint32_t thread_index_generation(1);

void set_thread_index_count(unsigned count)
{
	thread_index_count.store(count ? count : 1);
	next_thread_index.store(1);
	thread_index_generation.fetch_add(1);
}

unsigned get_current_thread_index()
{
	const unsigned gen = thread_index_generation.load();
	auto ret = thread_id_to_index;
	if (ret == ~0u || thread_id_generation != gen)
	{
		const unsigned count = thread_index_count.load();
		unsigned idx = next_thread_index.fetch_add(1, PGS::memory_order_relaxed);
		if (idx >= count)
		{
			/* More recording threads than the device has pools. Sharing
			 * index 0 races the owning thread on one VkCommandPool; the
			 * count in GSRendererPGS is what needs raising. */
			LOGE("Thread %llu needs a command pool but the device has only %u; sharing pool 0.\n",
			     (unsigned long long)sthread_get_current_thread_id(), count);
			idx = 0;
		}
		else
			LOGI("Thread %llu takes command pool index %u.\n",
			     (unsigned long long)sthread_get_current_thread_id(), idx);
		thread_id_to_index = idx;
		thread_id_generation = gen;
		ret = idx;
	}
	return ret;
}

void register_thread_index(unsigned index)
{
	thread_id_to_index = index;
	thread_id_generation = thread_index_generation.load();
}
}
