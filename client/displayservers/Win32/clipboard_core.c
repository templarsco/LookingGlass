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

#include "clipboard_win32.h"

#include "core/clipboard.h"

/* The clipboard of the Windows display server, told to the clipboard of the
 * client. It is a file of its own so that a test of Windows' side can give
 * that side another core. */

static void coreNotifyTypes(void * opaque, const LG_ClipboardData types[],
    size_t count)
{
  clipboard_notifyTypes(types, count);
}

static void coreRelease(void * opaque)
{
  clipboard_release();
}

static void coreData(void * opaque, LG_ClipboardRequest request,
    LG_ClipboardData type, const void * data, size_t size)
{
  clipboard_data(request, type, data, size);
}

static void coreAbort(void * opaque, LG_ClipboardRequest request)
{
  clipboard_abort(request);
}

static bool coreRequest(void * opaque, LG_ClipboardData type,
    LG_ClipboardReplyFn replyFn, void * replyOpaque)
{
  return clipboard_request(type, replyFn, replyOpaque);
}

bool win32CBInit(void)
{
  static const Win32ClipboardCore core =
  {
    .notifyTypes = coreNotifyTypes,
    .release     = coreRelease,
    .data        = coreData,
    .abort       = coreAbort,
    .request     = coreRequest,
  };

  return win32CBStart(&core);
}

void win32CBFree(void)
{
  win32CBStop();
}
