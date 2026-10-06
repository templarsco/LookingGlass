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

/* The client's request that Windows does not throttle it as a program in the
 * background. The test turns the throttling on, which it reads back, then
 * makes the request, and reads back that it is off, so that what it checks
 * is what the request did, and not what Windows started with. */

#include "test.h"

#include "common/debug.h"
#include "common/time.h"

#include <windows.h>

#include <string.h>

struct State
{
  ULONG version;
  ULONG controlMask;
  ULONG stateMask;
};

#define EXECUTION_SPEED 0x1
#define CLASS           4

typedef BOOL (WINAPI * SetFn)(HANDLE, int, LPVOID, DWORD);
typedef BOOL (WINAPI * GetFn)(HANDLE, int, LPVOID, DWORD);

int main(void)
{
  debug_init();

  const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
  const SetFn set = (SetFn)(void *)GetProcAddress(kernel,
      "SetProcessInformation");
  const GetFn get = (GetFn)(void *)GetProcAddress(kernel,
      "GetProcessInformation");
  if (!set || !get)
  {
    // Windows 7: there is nothing to throttle with, and nothing to ask
    CHECK(!windowsDisablePowerThrottling());
    puts("power tests skipped: this Windows has no power throttling");
    return EXIT_SUCCESS;
  }

  struct State state =
  {
    .version     = 1,
    .controlMask = EXECUTION_SPEED,
    .stateMask   = EXECUTION_SPEED,
  };
  CHECK(set(GetCurrentProcess(), CLASS, &state, sizeof(state)));

  struct State read = { .version = 1 };
  CHECK(get(GetCurrentProcess(), CLASS, &read, sizeof(read)));
  CHECK(read.controlMask & EXECUTION_SPEED);
  CHECK(read.stateMask   & EXECUTION_SPEED);

  CHECK(windowsDisablePowerThrottling());

  memset(&read, 0, sizeof(read));
  read.version = 1;
  CHECK(get(GetCurrentProcess(), CLASS, &read, sizeof(read)));
  CHECK(read.controlMask & EXECUTION_SPEED);
  CHECK(!(read.stateMask & EXECUTION_SPEED));

  // and again, which changes nothing
  CHECK(windowsDisablePowerThrottling());

  puts("power tests passed");
  return EXIT_SUCCESS;
}
