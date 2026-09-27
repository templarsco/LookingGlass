/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "common/event.h"
#include "common/windebug.h"
#include "common/time.h"

#include <windows.h>
#include <stdatomic.h>

LGEvent * lgCreateEvent(bool autoReset, unsigned int msSpinTime)
{
  HANDLE handle = CreateEvent(NULL, autoReset ? FALSE : TRUE, FALSE, NULL);
  if (!handle)
  {
    DEBUG_WINERROR("Failed to create the event", GetLastError());
    return NULL;
  }

  return (LGEvent *)handle;
}

LGEvent * lgWrapEvent(void * handle)
{
  return (LGEvent *)handle;
}

void lgFreeEvent(LGEvent * event)
{
  CloseHandle((HANDLE)event);
}

bool lgWaitEvent(LGEvent * event, unsigned int timeout)
{
  const DWORD to = (timeout == TIMEOUT_INFINITE) ? INFINITE : (DWORD)timeout;
  do
  {
    switch(WaitForSingleObject((HANDLE)event, to))
    {
      case WAIT_OBJECT_0:
        return true;

      case WAIT_ABANDONED:
        continue;

      case WAIT_TIMEOUT:
        if (timeout == TIMEOUT_INFINITE)
          continue;
        return false;

      case WAIT_FAILED:
        DEBUG_WINERROR("Wait for event failed", GetLastError());
        return false;

      default:
        DEBUG_ERROR("Unknown wait event return code");
        return false;
    }
  }
  while(true);
}

bool lgSignalEvent(LGEvent * event)
{
  return SetEvent((HANDLE)event);
}

bool lgResetEvent(LGEvent * event)
{
  return ResetEvent((HANDLE)event);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static bool waitEventNS(LGEvent * event, int64_t timeout)
{
  if (timeout <= 0)
    return WaitForSingleObject((HANDLE)event, 0) == WAIT_OBJECT_0;

  // high resolution timers need Windows 10 1803, older versions round the
  // wait up to the system timer resolution
  HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer)
    timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);

  LARGE_INTEGER due = { .QuadPart = -((timeout + 99) / 100) };
  if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
  {
    DEBUG_WINERROR("Failed to create the wait timer", GetLastError());
    if (timer)
      CloseHandle(timer);
    return lgWaitEvent(event, (unsigned int)((timeout + 999999) / 1000000));
  }

  HANDLE handles[] = { (HANDLE)event, timer };
  const DWORD result = WaitForMultipleObjects(ARRAYSIZE(handles), handles,
      FALSE, INFINITE);
  CloseHandle(timer);

  switch(result)
  {
    case WAIT_OBJECT_0:
      return true;

    case WAIT_OBJECT_0 + 1:
      return false;

    case WAIT_FAILED:
      DEBUG_WINERROR("Wait for event failed", GetLastError());
      return false;

    default:
      DEBUG_ERROR("Unknown wait event return code");
      return false;
  }
}

// ts is an absolute CLOCK_MONOTONIC time. MinGW implements that clock with
// QueryPerformanceCounter, the same source as nanotime.
bool lgWaitEventAbs(LGEvent * event, struct timespec * ts)
{
  if (!ts)
    return lgWaitEvent(event, TIMEOUT_INFINITE);

  const int64_t deadline = (int64_t)ts->tv_sec * 1000000000LL + ts->tv_nsec;
  return waitEventNS(event, deadline - (int64_t)nanotime());
}

bool lgWaitEventNS(LGEvent * event, unsigned int timeout)
{
  if (timeout == TIMEOUT_INFINITE)
    return lgWaitEvent(event, TIMEOUT_INFINITE);

  return waitEventNS(event, timeout);
}
