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
#include "clipboard.h"
#include "clipboard_format.h"

#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/thread.h"
#include "common/windebug.h"

#include <windows.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* The messages of the clipboard go to a window, and Windows holds the program
 * that pastes until that window answers, so the window has a thread of its
 * own that does nothing else. It is not the window of the client: that thread
 * does not wait for anybody. */

#define WINDOW_CLASS L"LookingGlassClipboard"
#define WM_LG_CB_WAKE (WM_APP + 1)

// how many times the clipboard is tried when another program has it open
#define OPEN_TRIES 8

struct State
{
  LG_Lock           lock;        // the hooks and the free, for the pointer
  Win32Clipboard  * clipboard;
  LGThread        * thread;
  LGEvent         * ready;
  atomic_bool       initOk;
  _Atomic(HWND)     window;

  unsigned          formatPng;
  unsigned          formatJfif;
  unsigned          formatExclude;
  unsigned          formatHistory;
  unsigned          formatCloud;
};

static struct State g;

/* the table of Windows */

static void apiWake(void * opaque)
{
  HWND window = atomic_load(&g.window);
  if (window)
    PostMessageW(window, WM_LG_CB_WAKE, 0, 0);
}

static bool apiOpen(void * opaque)
{
  HWND window = atomic_load(&g.window);
  DWORD delay = 5;
  for (unsigned i = 0; i < OPEN_TRIES; ++i)
  {
    if (OpenClipboard(window))
      return true;
    Sleep(delay);
    if (delay < 160)
      delay *= 2;
  }
  return false;
}

static void apiClose(void * opaque)
{
  CloseClipboard();
}

static bool apiEmpty(void * opaque)
{
  return EmptyClipboard() != FALSE;
}

static uint32_t apiSequence(void * opaque)
{
  return GetClipboardSequenceNumber();
}

static bool apiIsOwner(void * opaque)
{
  HWND window = atomic_load(&g.window);
  return window && GetClipboardOwner() == window;
}

static bool apiHasFormat(void * opaque, unsigned format)
{
  return IsClipboardFormatAvailable(format) != FALSE;
}

static uint8_t * apiGet(void * opaque, unsigned format, size_t * size)
{
  HANDLE handle = GetClipboardData(format);
  if (!handle)
    return NULL;

  const SIZE_T length = GlobalSize(handle);
  if (!length || length > LG_CLIPBOARD_FORMAT_MAX)
    return NULL;

  const void * data = GlobalLock(handle);
  if (!data)
    return NULL;

  uint8_t * copy = malloc(length);
  if (copy)
  {
    memcpy(copy, data, length);
    *size = length;
  }
  GlobalUnlock(handle);
  return copy;
}

static bool apiSet(void * opaque, unsigned format, const void * data,
    size_t size)
{
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
  if (!memory)
    return false;

  void * target = GlobalLock(memory);
  if (!target)
  {
    GlobalFree(memory);
    return false;
  }
  if (size)
    memcpy(target, data, size);
  GlobalUnlock(memory);

  // Windows owns the memory once it takes it
  if (!SetClipboardData(format, memory))
  {
    GlobalFree(memory);
    return false;
  }
  return true;
}

static bool apiSetDelayed(void * opaque, unsigned format)
{
  // it returns NULL for a request that was made and for one that failed
  SetClipboardData(format, NULL);
  return IsClipboardFormatAvailable(format) != FALSE;
}

static bool apiSetPrivate(void * opaque)
{
  /* The guest may have copied a password, and nobody asked for it to be
   * kept in the history of the clipboard, or sent to the cloud. */
  const DWORD zero = 0;
  bool ok = true;
  if (g.formatExclude)
    ok &= apiSet(opaque, g.formatExclude, &zero, sizeof(zero));
  if (g.formatHistory)
    ok &= apiSet(opaque, g.formatHistory, &zero, sizeof(zero));
  if (g.formatCloud)
    ok &= apiSet(opaque, g.formatCloud, &zero, sizeof(zero));
  return ok;
}

/* the window */

static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam)
{
  // only this thread uses it, and the free joins the thread before it frees it
  Win32Clipboard * clipboard = g.clipboard;

  switch (message)
  {
    case WM_CLIPBOARDUPDATE:
      if (clipboard)
        win32Clipboard_changed(clipboard);
      return 0;

    case WM_RENDERFORMAT:
      if (clipboard)
        win32Clipboard_render(clipboard, (unsigned)wParam);
      return 0;

    case WM_RENDERALLFORMATS:
      if (clipboard)
        win32Clipboard_renderAll(clipboard);
      return 0;

    case WM_DESTROYCLIPBOARD:
      if (clipboard)
        win32Clipboard_lost(clipboard);
      return 0;

    case WM_LG_CB_WAKE:
      if (clipboard)
        win32Clipboard_process(clipboard);
      return 0;

    case WM_CLOSE:
      DestroyWindow(window);
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }

  return DefWindowProcW(window, message, wParam, lParam);
}

static int clipboardThread(void * opaque)
{
  HINSTANCE instance = GetModuleHandleW(NULL);

  WNDCLASSEXW windowClass =
  {
    .cbSize        = sizeof(windowClass),
    .lpfnWndProc   = windowProc,
    .hInstance     = instance,
    .lpszClassName = WINDOW_CLASS,
  };

  HWND window = NULL;
  if (RegisterClassExW(&windowClass) ||
      GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
    window = CreateWindowExW(0, WINDOW_CLASS, L"Looking Glass clipboard", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, instance, NULL);

  if (!window)
  {
    DEBUG_WINERROR("The clipboard's window could not be made", GetLastError());
    lgSignalEvent(g.ready);
    return 1;
  }

  atomic_store(&g.window, window);
  if (!AddClipboardFormatListener(window))
  {
    DEBUG_WINERROR("The clipboard could not be listened to", GetLastError());
    atomic_store(&g.window, NULL);
    DestroyWindow(window);
    lgSignalEvent(g.ready);
    return 1;
  }

  atomic_store(&g.initOk, true);
  lgSignalEvent(g.ready);

  // what is on the clipboard now, and what the core asked for since
  win32Clipboard_start(g.clipboard);
  win32Clipboard_process(g.clipboard);

  MSG message;
  while (GetMessageW(&message, NULL, 0, 0) > 0)
  {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  RemoveClipboardFormatListener(window);
  atomic_store(&g.window, NULL);
  UnregisterClassW(WINDOW_CLASS, instance);
  return 0;
}

/* the hooks */

bool win32CBStart(const Win32ClipboardCore * core)
{
  if (g.clipboard)
    return true;

  g.formatPng     = RegisterClipboardFormatW(L"PNG");
  g.formatJfif    = RegisterClipboardFormatW(L"JFIF");
  g.formatExclude = RegisterClipboardFormatW(
      L"ExcludeClipboardContentFromMonitorProcessing");
  g.formatHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
  g.formatCloud   = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");

  const Win32ClipboardApi api =
  {
    .formatPng  = g.formatPng,
    .formatJfif = g.formatJfif,
    .wake       = apiWake,
    .open       = apiOpen,
    .close      = apiClose,
    .empty      = apiEmpty,
    .sequence   = apiSequence,
    .isOwner    = apiIsOwner,
    .hasFormat  = apiHasFormat,
    .get        = apiGet,
    .set        = apiSet,
    .setDelayed = apiSetDelayed,
    .setPrivate = apiSetPrivate,
  };

  Win32Clipboard * clipboard = win32Clipboard_create(&api, core, 0);
  if (!clipboard)
    return false;

  LG_LOCK_INIT(g.lock);
  g.ready = lgCreateEvent(false, 0);
  atomic_store(&g.initOk, false);
  if (!g.ready)
  {
    win32Clipboard_destroy(clipboard);
    return false;
  }

  g.clipboard = clipboard;
  if (!lgCreateThread("Win32Clipboard", clipboardThread, NULL, &g.thread))
  {
    g.clipboard = NULL;
    win32Clipboard_destroy(clipboard);
    lgFreeEvent(g.ready);
    g.ready = NULL;
    return false;
  }

  lgWaitEvent(g.ready, 5000);
  if (!atomic_load(&g.initOk))
  {
    DEBUG_WARN("The clipboard is not available");
    lgJoinThread(g.thread, NULL);
    g.thread    = NULL;
    g.clipboard = NULL;
    win32Clipboard_destroy(clipboard);
    lgFreeEvent(g.ready);
    g.ready = NULL;
    return false;
  }

  return true;
}

void win32CBStop(void)
{
  HWND window = atomic_load(&g.window);
  if (g.thread)
  {
    if (window)
      PostMessageW(window, WM_CLOSE, 0, 0);
    lgJoinThread(g.thread, NULL);
    g.thread = NULL;
  }

  LG_LOCK(g.lock);
  Win32Clipboard * clipboard = g.clipboard;
  g.clipboard = NULL;
  LG_UNLOCK(g.lock);
  win32Clipboard_destroy(clipboard);

  if (g.ready)
  {
    lgFreeEvent(g.ready);
    g.ready = NULL;
  }
}

void win32CBNotice(LG_ClipboardData type)
{
  LG_LOCK(g.lock);
  if (g.clipboard)
    win32Clipboard_notice(g.clipboard, type);
  LG_UNLOCK(g.lock);
}

void win32CBRelease(void)
{
  LG_LOCK(g.lock);
  if (g.clipboard)
    win32Clipboard_release(g.clipboard);
  LG_UNLOCK(g.lock);
}

void win32CBRequest(LG_ClipboardRequest request, LG_ClipboardData type)
{
  LG_LOCK(g.lock);
  if (g.clipboard)
    win32Clipboard_request(g.clipboard, request, type);
  LG_UNLOCK(g.lock);
}

void win32CBRequestReady(LG_ClipboardRequest request)
{
  // the data is sent whole, and is never blocked
  (void)request;
}

void win32CBRequestCancel(LG_ClipboardRequest request,
    LG_ClipboardCancelReason reason)
{
  (void)reason;
  LG_LOCK(g.lock);
  if (g.clipboard)
    win32Clipboard_requestCancel(g.clipboard, request);
  LG_UNLOCK(g.lock);
}
