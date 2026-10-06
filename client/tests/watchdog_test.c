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

/* The watchdog that ends the client when its shutdown does not finish. It
 * ends the process that arms it, so the test runs itself again as that
 * process, which hangs, as a thread that is stuck in a driver does. */

#include "test.h"

#include "common/debug.h"
#include "common/watchdog.h"

#include <windows.h>

#include <string.h>

static int child(const char * mode)
{
  if (strcmp(mode, "hang") == 0)
  {
    CHECK(lgWatchdogArm(300, "The test"));
    Sleep(INFINITE);
    return 0;
  }

  if (strcmp(mode, "finish") == 0)
  {
    // the process ends when it is done, and not when the watchdog says
    CHECK(lgWatchdogArm(60000, "The test"));
    return 0;
  }

  if (strcmp(mode, "twice") == 0)
  {
    // the second does not move the time of the first
    CHECK(lgWatchdogArm(300, "The test"));
    CHECK(lgWatchdogArm(60000, "Another"));
    Sleep(INFINITE);
    return 0;
  }

  return 99;
}

// runs this program as the child, and returns its exit code and how long it
// lived. A child that does not end is ended by the test, with 98
static DWORD run(const char * mode, DWORD * lived)
{
  char path[MAX_PATH];
  CHECK(GetModuleFileNameA(NULL, path, sizeof(path)) > 0);

  char command[MAX_PATH + 32];
  CHECK(snprintf(command, sizeof(command), "\"%s\" child %s", path, mode) <
      (int)sizeof(command));

  STARTUPINFOA info = { .cb = sizeof(info) };
  PROCESS_INFORMATION process;
  CHECK(CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL,
        &info, &process));

  const DWORD start = GetTickCount();
  if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0)
    TerminateProcess(process.hProcess, 98);
  WaitForSingleObject(process.hProcess, INFINITE);
  *lived = GetTickCount() - start;

  DWORD code = 0;
  CHECK(GetExitCodeProcess(process.hProcess, &code));
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return code;
}

static void testHang(void)
{
  DWORD lived;
  CHECK(run("hang", &lived) == LG_WATCHDOG_EXIT_CODE);
  CHECK(lived >= 250);
  CHECK(lived < 8000);
}

static void testFinish(void)
{
  DWORD lived;
  CHECK(run("finish", &lived) == 0);
  CHECK(lived < 8000);
}

static void testTwice(void)
{
  DWORD lived;
  CHECK(run("twice", &lived) == LG_WATCHDOG_EXIT_CODE);
  CHECK(lived >= 250);
  CHECK(lived < 8000);
}

int main(int argc, char * argv[])
{
  debug_init();

  if (argc == 3 && strcmp(argv[1], "child") == 0)
    return child(argv[2]);

  testHang();
  testFinish();
  testTwice();
  puts("watchdog tests passed");
  return EXIT_SUCCESS;
}
