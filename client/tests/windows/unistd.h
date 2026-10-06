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

/* MinGW's unistd.h, plus alarm() for the unit tests that use it as a watchdog.
 * The Linux one ends the process with SIGALRM, this one with exit status 142,
 * which is what a shell reports for a process that SIGALRM ended. */

#ifndef LG_CLIENT_TESTS_WINDOWS_UNISTD_H
#define LG_CLIENT_TESTS_WINDOWS_UNISTD_H

#ifndef _WIN32
#error "this is the Windows wrapper for unistd.h"
#endif

#include_next <unistd.h>

#include <process.h>
#include <stdlib.h>
#include <windows.h>

static inline unsigned __stdcall lgTestAlarmThread(void * param)
{
  Sleep((DWORD)(uintptr_t)param * 1000);
  _exit(128 + 14);
  return 0;
}

static inline unsigned int alarm(unsigned int seconds)
{
  if (seconds)
  {
    const uintptr_t thread = _beginthreadex(NULL, 0, lgTestAlarmThread,
        (void *)(uintptr_t)seconds, 0, NULL);
    if (thread)
      CloseHandle((HANDLE)thread);
  }
  return 0;
}

#endif
