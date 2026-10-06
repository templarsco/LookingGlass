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

#include "common/paths.h"
#include "common/debug.h"
#include "common/windebug.h"

#include <windows.h>
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

// UTF-8 can take up to three bytes for each UTF-16 unit of a MAX_PATH path
#define PATH_BUFFER (MAX_PATH * 3)

static char configDir[PATH_BUFFER];
static char dataDir[PATH_BUFFER];

/* The environment variable wins so that tests and portable setups can
 * redirect the directories, as XDG_CONFIG_HOME does on Linux. */
static bool baseDir(const wchar_t * env, REFKNOWNFOLDERID folder,
    wchar_t * out, size_t count)
{
  const wchar_t * value = _wgetenv(env);
  if (value && *value)
  {
    if ((size_t)_snwprintf(out, count, L"%ls", value) >= count)
      return false;
    out[count - 1] = L'\0';
    return true;
  }

  PWSTR known = NULL;
  HRESULT hr = SHGetKnownFolderPath(folder, KF_FLAG_CREATE, NULL, &known);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("SHGetKnownFolderPath failed", hr);
    return false;
  }

  const bool fits = (size_t)_snwprintf(out, count, L"%ls", known) < count;
  out[count - 1] = L'\0';
  CoTaskMemFree(known);
  return fits;
}

static void appDir(const wchar_t * env, REFKNOWNFOLDERID folder,
    const char * appName, char * out, size_t size)
{
  wchar_t base[MAX_PATH];
  wchar_t app[MAX_PATH];
  wchar_t path[MAX_PATH];

  if (!baseDir(env, folder, base, ARRAYSIZE(base)) ||
      !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, appName, -1, app,
        ARRAYSIZE(app)))
  {
    DEBUG_ERROR("Unable to resolve the %ls directory", env);
    exit(2);
  }

  const int len = _snwprintf(path, ARRAYSIZE(path), L"%ls\\%ls", base, app);
  if (len < 0 || len >= (int)ARRAYSIZE(path))
  {
    DEBUG_ERROR("The %ls directory path is too long", env);
    exit(2);
  }

  const int err = SHCreateDirectoryExW(NULL, path, NULL);
  if (err != ERROR_SUCCESS && err != ERROR_ALREADY_EXISTS &&
      err != ERROR_FILE_EXISTS)
    DEBUG_WINERROR("Failed to create the directory", err);

  const DWORD attrib = GetFileAttributesW(path);
  if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY))
  {
    DEBUG_ERROR("Expected to be a directory: %ls", path);
    exit(2);
  }

  if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, out, size, NULL, NULL))
  {
    DEBUG_WINERROR("Failed to convert the directory path", GetLastError());
    exit(2);
  }
}

void lgPathsInit(const char * appName)
{
  /* The paths are UTF-8, and the C library opens them by the code page of the
   * process. The manifest asks for UTF-8 as that, which Windows has honoured
   * since 10 version 1903: before it, a configuration in a folder with an
   * accent in it, as a user's name may have, is not found. */
  if (GetACP() != CP_UTF8)
    DEBUG_WARN("The code page of the process is %u and not UTF-8, which "
        "needs Windows 10 version 1903 or later: a path with a character "
        "that it lacks will not be found", (unsigned)GetACP());

  appDir(L"APPDATA"     , &FOLDERID_RoamingAppData, appName, configDir,
      sizeof(configDir));
  appDir(L"LOCALAPPDATA", &FOLDERID_LocalAppData  , appName, dataDir  ,
      sizeof(dataDir));
}

const char * lgConfigDir(void)
{
  return configDir;
}

const char * lgDataDir(void)
{
  return dataDir;
}
