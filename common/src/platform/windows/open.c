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

#include "common/open.h"
#include "common/debug.h"

#include <windows.h>
#include <shellapi.h>

#include <stdint.h>

bool lgOpenURL(const char * url)
{
  const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1,
      NULL, 0);
  if (len <= 0)
  {
    DEBUG_ERROR("Invalid URL: %s", url);
    return false;
  }

  wchar_t * wurl = malloc(len * sizeof(*wurl));
  if (!wurl)
  {
    DEBUG_ERROR("out of memory");
    return false;
  }

  MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, len);
  const INT_PTR result = (INT_PTR)ShellExecuteW(NULL, L"open", wurl, NULL, NULL,
      SW_SHOWNORMAL);
  free(wurl);

  // ShellExecute returns a value greater than 32 on success
  if (result <= 32)
  {
    DEBUG_ERROR("Failed to open %s (error %d)", url, (int)result);
    return false;
  }

  return true;
}
