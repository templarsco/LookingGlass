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

#ifndef _H_LG_CLIENT_DISPLAYSERVER_WIN32_CLIPBOARD_
#define _H_LG_CLIENT_DISPLAYSERVER_WIN32_CLIPBOARD_

/* What the Windows display server does with the clipboard: text and images
 * that are copied on the PC are offered to the guest, which asks for them
 * when it pastes, and what the guest copies is put on the clipboard of the PC
 * as an offer that is not filled in until something pastes it.
 *
 * The logic is here, and nothing in it calls Windows: the operations on the
 * clipboard are a table (Win32ClipboardApi), and what it tells the rest of
 * the client is another (Win32ClipboardCore), so that a test can give it a
 * clipboard of its own and the core as a recording. clipboard_win32.c is the
 * table of Windows, and the thread that owns the window which Windows
 * sends the clipboard's messages to.
 *
 * Every function that is called by Windows, and win32Clipboard_process, are
 * for one thread, the one that has the window. The hooks that the core calls
 * (notice, release, request and requestCancel) may be called from any
 * thread, with the core's lock held: they do not call Windows and do not wait,
 * they leave a note and wake that thread. */

#include "interface/clipboard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// the numbers of the formats that every PC has
enum
{
  WIN32_CF_TIFF        = 6,
  WIN32_CF_DIB         = 8,
  WIN32_CF_UNICODETEXT = 13,
  WIN32_CF_DIBV5       = 17,
};

typedef struct Win32ClipboardApi
{
  void * opaque;

  // the formats that are registered by name: "PNG" and "JFIF"
  unsigned formatPng;
  unsigned formatJfif;

  /* Makes the thread that has the window call win32Clipboard_process(). Any
   * thread may call it, and it does not wait. */
  void (*wake)(void * opaque);

  /* Opens the clipboard for the window, trying again for a while when
   * another program has it. */
  bool (*open)(void * opaque);
  void (*close)(void * opaque);

  /* Makes the window the owner, which is what an offer from the guest needs,
   * and takes away what was there. */
  bool (*empty)(void * opaque);

  // The number that Windows counts the clipboard's changes with
  uint32_t (*sequence)(void * opaque);

  // Is the window the owner of what is on the clipboard?
  bool (*isOwner)(void * opaque);

  bool (*hasFormat)(void * opaque, unsigned format);

  /* A copy of what the clipboard holds in a format, as large as Windows says
   * it is, which free() takes. The clipboard is open. */
  uint8_t * (*get)(void * opaque, unsigned format, size_t * size);

  /* Puts data on the clipboard in a format. It is open, or the window is
   * answering a request to render. */
  bool (*set)(void * opaque, unsigned format, const void * data, size_t size);

  // Says that a format is on the clipboard but not filled in
  bool (*setDelayed)(void * opaque, unsigned format);

  /* Says that what the guest copied is to stay out of the clipboard's history
   * and the cloud's: it may be a password. */
  bool (*setPrivate)(void * opaque);
}
Win32ClipboardApi;

typedef struct Win32ClipboardCore
{
  void * opaque;

  void (*notifyTypes)(void * opaque, const LG_ClipboardData types[],
      size_t count);
  void (*release)(void * opaque);
  void (*data)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardData type, const void * data, size_t size);
  void (*abort)(void * opaque, LG_ClipboardRequest request);
  bool (*request)(void * opaque, LG_ClipboardData type,
      LG_ClipboardReplyFn replyFn, void * replyOpaque);
}
Win32ClipboardCore;

typedef struct Win32Clipboard Win32Clipboard;

/* How long a program that pastes what the guest copied waits for it, in
 * milliseconds, unless the creator says another. Whoever pastes is stopped for
 * as long, with the clipboard open, so it is not long. */
#define WIN32_CLIPBOARD_RENDER_TIMEOUT_MS 5000

Win32Clipboard * win32Clipboard_create(const Win32ClipboardApi * api,
    const Win32ClipboardCore * core, unsigned renderTimeoutMs);

/* Stops answering, and frees what it made. The hooks are not called any more
 * by then. */
void win32Clipboard_destroy(Win32Clipboard * clipboard);

/* The hooks of the core. */
void win32Clipboard_notice(Win32Clipboard * clipboard, LG_ClipboardData type);
void win32Clipboard_release(Win32Clipboard * clipboard);
void win32Clipboard_request(Win32Clipboard * clipboard,
    LG_ClipboardRequest request, LG_ClipboardData type);
void win32Clipboard_requestCancel(Win32Clipboard * clipboard,
    LG_ClipboardRequest request);

/* For the thread of the window. process() does what the hooks left. start()
 * offers what is on the clipboard already. */
void win32Clipboard_process(Win32Clipboard * clipboard);
void win32Clipboard_start(Win32Clipboard * clipboard);

/* What Windows says to the window. changed() is WM_CLIPBOARDUPDATE and lost()
 * is WM_DESTROYCLIPBOARD. render() is WM_RENDERFORMAT: it puts the data of
 * the format on the clipboard with the table's set(), and returns whether it
 * did. renderAll() is WM_RENDERALLFORMATS, for a window that is going away
 * while it still has an offer that nothing filled in. */
void win32Clipboard_changed(Win32Clipboard * clipboard);
void win32Clipboard_lost(Win32Clipboard * clipboard);
bool win32Clipboard_render(Win32Clipboard * clipboard, unsigned format);
void win32Clipboard_renderAll(Win32Clipboard * clipboard);

#endif
