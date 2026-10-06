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

/* PROCESS_POWER_THROTTLING_STATE and its flags are not in the headers that
 * the build uses, which ask for Windows 7, and the function that takes them
 * does not exist there: they are written out, and it is looked up */
struct PowerThrottling
{
  ULONG version;
  ULONG controlMask;
  ULONG stateMask;
};

#define POWER_THROTTLING_VERSION       1
#define POWER_THROTTLING_EXECUTION     0x1
#define POWER_THROTTLING_TIMER         0x4
#define PROCESS_POWER_THROTTLING_CLASS 4

bool windowsDisablePowerThrottling(void)
{
  typedef BOOL (WINAPI * SetProcessInformationFn)(HANDLE, int, LPVOID, DWORD);

  const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
  const SetProcessInformationFn set = kernel ?
    (SetProcessInformationFn)(void *)GetProcAddress(kernel,
        "SetProcessInformation") : NULL;
  if (!set)
    return false;

  /* A mask is control over the flags that it has, and a state of none is
   * the flags off. Windows 10 does not know the flag of the timer, and
   * refuses a request that has it. */
  struct PowerThrottling state =
  {
    .version     = POWER_THROTTLING_VERSION,
    .controlMask = POWER_THROTTLING_EXECUTION | POWER_THROTTLING_TIMER,
    .stateMask   = 0,
  };
  if (set(GetCurrentProcess(), PROCESS_POWER_THROTTLING_CLASS, &state,
        sizeof(state)))
    return true;

  state.controlMask = POWER_THROTTLING_EXECUTION;
  return set(GetCurrentProcess(), PROCESS_POWER_THROTTLING_CLASS, &state,
      sizeof(state)) != FALSE;
}
