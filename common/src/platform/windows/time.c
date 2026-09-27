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

#include "common/time.h"
#include "common/debug.h"

// decared by the platform
extern HWND MessageHWND;

struct LGTimer
{
  LGTimerFn   fn;
  void      * udata;
  UINT_PTR    handle;
  HANDLE      queueTimer;
  bool        running;
};

// used when there is no MessageHWND, the callback runs on the thread pool
static VOID CALLBACK QueueTimerProc(PVOID param, BOOLEAN fired)
{
  LGTimer * timer = (LGTimer *)param;
  if (!__atomic_load_n(&timer->running, __ATOMIC_ACQUIRE))
    return;

  if (!timer->fn(timer->udata))
    __atomic_store_n(&timer->running, false, __ATOMIC_RELEASE);
}

static void TimerProc(HWND Arg1, UINT Arg2, UINT_PTR Arg3, DWORD Arg4)
{
  LGTimer * timer = (LGTimer *)Arg3;
  if (!timer->fn(timer->udata))
  {
    KillTimer(Arg1, timer->handle);
    timer->running = false;
  }
}

bool lgCreateTimer(const unsigned int intervalMS, LGTimerFn fn,
    void * udata, LGTimer ** result)
{
  LGTimer * ret = malloc(sizeof(*ret));
  if (!ret)
  {
    DEBUG_ERROR("failed to malloc LGTimer struct");
    return false;
  }

  ret->fn         = fn;
  ret->udata      = udata;
  ret->running    = true;
  ret->handle     = 0;
  ret->queueTimer = NULL;

  /* SetTimer needs a window owned by the calling thread and a message loop,
   * applications without MessageHWND get a thread pool timer instead */
  if (!MessageHWND)
  {
    if (!CreateTimerQueueTimer(&ret->queueTimer, NULL, QueueTimerProc, ret,
          intervalMS, intervalMS, WT_EXECUTEDEFAULT))
    {
      DEBUG_ERROR("failed to create the timer");
      free(ret);
      return false;
    }
  }
  else
    ret->handle = SetTimer(MessageHWND, (UINT_PTR)ret, intervalMS, TimerProc);

  *result = ret;
  return true;
}

void lgTimerDestroy(LGTimer * timer)
{
  if (timer->queueTimer)
  {
    // waits for a running callback to finish
    if (!DeleteTimerQueueTimer(NULL, timer->queueTimer, INVALID_HANDLE_VALUE))
      DEBUG_ERROR("failed to destroy the timer");
  }
  else if (timer->running)
  {
    if (MessageHWND && !KillTimer(MessageHWND, timer->handle))
      DEBUG_ERROR("failed to destroy the timer");
  }

  free(timer);
}

NTSYSCALLAPI NTSTATUS NTAPI NtSetTimerResolution(
  _In_ ULONG DesiredTime,
  _In_ BOOLEAN SetResolution,
  _Out_ PULONG ActualTime
);

void windowsSetTimerResolution(void)
{
  ULONG actualResolution;
  NtSetTimerResolution(1, true, &actualResolution);
  DEBUG_INFO("System timer resolution: %.1f μs", actualResolution / 10.0);
}
