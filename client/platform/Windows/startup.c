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

/* The Windows client is a program of the GUI subsystem: it opens no console
 * window of its own when it is started from Explorer or a shortcut, and it
 * has to say where its messages go instead.
 *
 *  - Started with its standard error redirected (to a file or a pipe, as a
 *    test or another program does it), it writes there, as any program does.
 *  - Started from a terminal that is not redirected, it uses the terminal.
 *  - Started with neither, as Explorer starts it, it writes to a log file in
 *    %LOCALAPPDATA%\looking-glass, and if it stops with an error it says so
 *    in a message box with the last lines of the log: nobody is looking at a
 *    console that does not exist.
 *
 * main.c's main() is renamed to lgClientMain() on Windows, and this one wraps
 * it. */

#include <windows.h>
#include <shlobj.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// the entry point of main.c
int lgClientMain(int argc, char * argv[]);

#define LOG_NAME      L"client.log"
#define LOG_PREVIOUS  L"client.log.1"

// how much of the end of the log the box shows
#define TAIL_BYTES    2048
#define TAIL_LINES    10

static struct
{
  bool    output;    // standard error goes where somebody can read it
  bool    logFile;   // and it is the log file
  wchar_t logPath[MAX_PATH + 32];
}
g_startup;

static bool standardErrorUsable(void)
{
  const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
  return handle && handle != INVALID_HANDLE_VALUE &&
    GetFileType(handle) != FILE_TYPE_UNKNOWN;
}

static bool attachParentConsole(void)
{
  if (!AttachConsole(ATTACH_PARENT_PROCESS))
    return false;

  freopen("CONOUT$", "w", stdout);
  freopen("CONOUT$", "w", stderr);
  freopen("CONIN$", "r", stdin);
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  return true;
}

// %LOCALAPPDATA%\looking-glass, where the variable wins, as it does for the
// folders of the configuration
static bool logFolder(wchar_t * folder, size_t count)
{
  wchar_t         known[MAX_PATH];
  const wchar_t * base = _wgetenv(L"LOCALAPPDATA");
  if (!base || !*base)
  {
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL,
            SHGFP_TYPE_CURRENT, known)))
      return false;
    base = known;
  }

  // SHCreateDirectoryEx only makes a folder that has a full path, and a
  // variable that a test or a portable setup sets may not give one
  wchar_t relative[MAX_PATH];
  const int length = _snwprintf(relative, ARRAYSIZE(relative),
      L"%ls\\looking-glass", base);
  if (length < 0 || (size_t)length >= ARRAYSIZE(relative))
    return false;

  const DWORD full = GetFullPathNameW(relative, (DWORD)count, folder, NULL);
  if (!full || full >= count)
    return false;

  const int error = SHCreateDirectoryExW(NULL, folder, NULL);
  return error == ERROR_SUCCESS || error == ERROR_ALREADY_EXISTS ||
    error == ERROR_FILE_EXISTS;
}

// the log of this run starts empty, and is where both standard streams go. The
// streams append, so that the two of them do not write over each other
static bool openLogAt(const wchar_t * path)
{
  FILE * file = _wfopen(path, L"w");
  if (!file)
    return false;
  fclose(file);

  if (!_wfreopen(path, L"a", stderr))
    return false;
  _wfreopen(path, L"a", stdout);
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  return true;
}

static bool openLog(void)
{
  wchar_t folder[MAX_PATH];
  if (!logFolder(folder, ARRAYSIZE(folder)))
    return false;

  /* The log of the last run is kept as client.log.1. Another client that is
   * running has the log open, and the move fails: this one has a log of its
   * own then, and leaves that one alone. */
  wchar_t path[MAX_PATH + 32];
  wchar_t previous[MAX_PATH + 32];
  _snwprintf(path, ARRAYSIZE(path), L"%ls\\%ls", folder, LOG_NAME);
  _snwprintf(previous, ARRAYSIZE(previous), L"%ls\\%ls", folder, LOG_PREVIOUS);
  path[ARRAYSIZE(path) - 1]         = L'\0';
  previous[ARRAYSIZE(previous) - 1] = L'\0';

  if ((MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING) ||
        GetLastError() == ERROR_FILE_NOT_FOUND) &&
      openLogAt(path))
  {
    wcscpy(g_startup.logPath, path);
    return true;
  }

  _snwprintf(path, ARRAYSIZE(path), L"%ls\\client-%lu.log", folder,
      (unsigned long)GetCurrentProcessId());
  path[ARRAYSIZE(path) - 1] = L'\0';
  if (!openLogAt(path))
    return false;

  wcscpy(g_startup.logPath, path);
  return true;
}

static void startup(void)
{
  if (standardErrorUsable())
  {
    g_startup.output = true;
    return;
  }

  if (attachParentConsole())
  {
    g_startup.output = true;
    return;
  }

  g_startup.logFile = openLog();
}

/* The last lines of the log, as UTF-16 for the box, in what the caller
 * gives. The log is open for the streams, so this reads it through a handle of
 * its own. */
static void logTail(wchar_t * out, size_t count)
{
  out[0] = L'\0';

  FILE * file = _wfopen(g_startup.logPath, L"rb");
  if (!file)
    return;

  fseek(file, 0, SEEK_END);
  const long size  = ftell(file);
  const long start = size > TAIL_BYTES ? size - TAIL_BYTES : 0;
  fseek(file, start, SEEK_SET);

  char         bytes[TAIL_BYTES + 1];
  const size_t read = fread(bytes, 1, TAIL_BYTES, file);
  fclose(file);
  bytes[read] = '\0';

  // from the start of a line, when the read began in the middle of one
  char * text = bytes;
  if (start > 0)
  {
    char * newline = strchr(text, '\n');
    if (newline)
      text = newline + 1;
  }

  size_t length = strlen(text);
  while (length && (text[length - 1] == '\n' || text[length - 1] == '\r'))
    text[--length] = '\0';

  // and only the last lines of that
  unsigned lines = 0;
  for (char * p = text + length; p > text; --p)
    if (p[-1] == '\n' && ++lines == TAIL_LINES)
    {
      text = p;
      break;
    }

  if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, out, (int)count))
    out[0] = L'\0';
  out[count - 1] = L'\0';
}

static void showFailure(int code)
{
  wchar_t tail[TAIL_BYTES + 1];
  tail[0] = L'\0';
  if (g_startup.logFile)
    logTail(tail, ARRAYSIZE(tail));

  wchar_t message[TAIL_BYTES + MAX_PATH + 256];
  _snwprintf(message, ARRAYSIZE(message),
      L"Looking Glass stopped with an error (status %d).\n\n%ls%ls%ls", code,
      tail,
      g_startup.logFile ? L"\n\nThe whole log is in:\n" : L"",
      g_startup.logFile ? g_startup.logPath : L"");
  message[ARRAYSIZE(message) - 1] = L'\0';

  MessageBoxW(NULL, message, L"Looking Glass",
      MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

int main(int argc, char * argv[])
{
  startup();
  const int code = lgClientMain(argc, argv);

  // a console or a redirection has what the client said, and the exit code
  if (code != 0 && !g_startup.output)
    showFailure(code);
  return code;
}
