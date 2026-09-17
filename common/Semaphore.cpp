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

#include "Threading.h"

#ifdef _WIN32
#include "RedtapeWindows.h"
#endif

#if defined(__APPLE__)
#include <pthread.h> // pthread_setcancelstate()
#include <sys/time.h> // gettimeofday()
#include <mach/mach.h>
#include <mach/task.h> // semaphore_create() and semaphore_destroy()
#include <mach/semaphore.h> // semaphore_*()
#include <mach/mach_error.h> // mach_error_string()
#include <mach/mach_time.h> // mach_absolute_time()
#endif

#include <stdint.h>

#ifndef _WIN32
#include <unistd.h> /* sysconf */
#endif


Threading::KernelSemaphore::KernelSemaphore()
{
#if defined(_WIN32)
	m_sema = CreateSemaphore(nullptr, 0, LONG_MAX, nullptr);
#elif defined(__APPLE__)
	semaphore_create(mach_task_self(), &m_sema, SYNC_POLICY_FIFO, 0);
#else
	sem_init(&m_sema, false, 0);
#endif
}

Threading::KernelSemaphore::~KernelSemaphore()
{
#if defined(_WIN32)
	CloseHandle(m_sema);
#elif defined(__APPLE__)
	semaphore_destroy(mach_task_self(), m_sema);
#else
	sem_destroy(&m_sema);
#endif
}

void Threading::KernelSemaphore::Post()
{
#if defined(_WIN32)
	ReleaseSemaphore(m_sema, 1, nullptr);
#elif defined(__APPLE__)
	semaphore_signal(m_sema);
#else
	sem_post(&m_sema);
#endif
}

void Threading::KernelSemaphore::Wait()
{
#if defined(_WIN32)
	WaitForSingleObject(m_sema, INFINITE);
#elif defined(__APPLE__)
	semaphore_wait(m_sema);
#else
	sem_wait(&m_sema);
#endif
}
