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

/* What the client's own build provides to the code that the unit tests link:
 * the version that the crash handler, which debug_init() installs, reports,
 * and the message window of the timers, which the tests do not have. Without
 * one, the timers run on the thread pool. */

#include <windows.h>

char * BUILD_VERSION = "unit tests";
HWND   MessageHWND   = NULL;

/* The client asks for the finest timer resolution when it starts, as
 * windowsSetTimerResolution() in common does. Without it a sleep of a
 * millisecond takes up to 15.6 ms, and the tests, which are timed for Linux,
 * wait far longer than they mean to. */
__attribute__((constructor))
static void lgTestTimerResolution(void)
{
  typedef LONG (NTAPI * SetTimerResolutionFn)(ULONG, BOOLEAN, PULONG);

  const HMODULE ntdll = GetModuleHandleA("ntdll.dll");
  SetTimerResolutionFn setResolution = ntdll ?
    (SetTimerResolutionFn)(void *)GetProcAddress(ntdll,
        "NtSetTimerResolution") : NULL;

  ULONG actual;
  if (setResolution)
    setResolution(1, TRUE, &actual);
}
