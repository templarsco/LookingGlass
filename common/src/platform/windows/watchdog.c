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

#include "common/watchdog.h"
#include "common/debug.h"
#include "common/windebug.h"

#include <windows.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

struct Watchdog
{
  DWORD ms;
  char  reason[128];
};

static atomic_flag g_armed = ATOMIC_FLAG_INIT;

static DWORD WINAPI watchdogThread(LPVOID opaque)
{
  const struct Watchdog * watchdog = opaque;
  Sleep(watchdog->ms);

  DEBUG_ERROR("%s did not finish in %lu ms, ending the process",
      watchdog->reason, (unsigned long)watchdog->ms);

  /* ExitProcess and exit() would wait for the loader lock and run the exit
   * handlers of what is stuck, which is why this is here */
  TerminateProcess(GetCurrentProcess(), LG_WATCHDOG_EXIT_CODE);
  return 0;
}

bool lgWatchdogArm(unsigned ms, const char * reason)
{
  if (atomic_flag_test_and_set(&g_armed))
    return true;

  struct Watchdog * watchdog = malloc(sizeof(*watchdog));
  if (!watchdog)
  {
    atomic_flag_clear(&g_armed);
    return false;
  }

  watchdog->ms = ms;
  snprintf(watchdog->reason, sizeof(watchdog->reason), "%s",
      reason ? reason : "The program");

  // it is not freed: it lives until the process ends, which is its use
  HANDLE thread = CreateThread(NULL, 0, watchdogThread, watchdog, 0, NULL);
  if (!thread)
  {
    DEBUG_WINERROR("Failed to start the watchdog", GetLastError());
    free(watchdog);
    atomic_flag_clear(&g_armed);
    return false;
  }

  CloseHandle(thread);
  return true;
}
