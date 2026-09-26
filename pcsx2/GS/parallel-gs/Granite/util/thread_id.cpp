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

/* Index 0 belongs to the thread that set up the device (it registers
 * itself); the counter hands out 1 upward. */
static PGS::atomic_uint32_t next_thread_index(1);
static unsigned thread_index_count = 1;

void set_thread_index_count(unsigned count)
{
	thread_index_count = count ? count : 1;
}

unsigned get_current_thread_index()
{
	auto ret = thread_id_to_index;
	if (ret == ~0u)
	{
		unsigned idx = next_thread_index.fetch_add(1, PGS::memory_order_relaxed);
		if (idx >= thread_index_count)
		{
			/* More recording threads than the device has pools. Sharing
			 * index 0 races the owning thread on one VkCommandPool; the
			 * count in GSRendererPGS is what needs raising. */
			LOGE("Thread %llu needs a command pool but the device has only %u; sharing pool 0.\n",
			     (unsigned long long)sthread_get_current_thread_id(), thread_index_count);
			idx = 0;
		}
		else
			LOGI("Thread %llu takes command pool index %u.\n",
			     (unsigned long long)sthread_get_current_thread_id(), idx);
		thread_id_to_index = idx;
		ret = idx;
	}
	return ret;
}

void register_thread_index(unsigned index)
{
	thread_id_to_index = index;
}
}
