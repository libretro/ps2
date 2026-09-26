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

#pragma once

namespace Util
{
/* The Vulkan device keeps one command pool per thread index per frame,
 * because a VkCommandPool may only be used by one thread at a time.
 * A thread's index is handed out on its first request, from a counter
 * bounded by the count the device was created with
 * (Context::set_num_thread_indices). A thread that only ever compiles
 * pipelines and never records a command buffer may register index 0
 * explicitly and share it. */

/* This thread's index; assigned on first call. */
unsigned get_current_thread_index();

/* Pins the calling thread to an index, for the device-owning thread
 * (index 0) and for threads that never record. */
void register_thread_index(unsigned thread_index);

/* The number of indices the device was built with; indices are handed
 * out below it. Set once, before any thread asks. */
void set_thread_index_count(unsigned count);
}
