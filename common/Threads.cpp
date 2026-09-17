/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2014  PCSX2 Dev Team
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

#if !defined(_WIN32) && !defined(__APPLE__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#endif

#include "Threading.h"
#include <utility>

#ifdef _WIN32
#include "RedtapeWindows.h"
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <memory>

#ifdef _WIN32
#include <mmsystem.h>
#include <process.h>
#include <timeapi.h>
#else
#include <sched.h>
#include <pthread.h>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/types.h>
#include <sched.h>

// glibc < v2.30 doesn't define gettid...
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#endif

#elif defined(__APPLE__)
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/mach_port.h>
#elif defined(__unix__)
#include <pthread_np.h>
#endif
#endif


#ifdef _WIN32
#else
#endif













#ifdef _WIN32
void Threading::Timeslice()
{
	::SwitchToThread();
}
#else
void Threading::Timeslice()
{
	sched_yield();
}
#endif

#include <rthreads/rthreads.h>

