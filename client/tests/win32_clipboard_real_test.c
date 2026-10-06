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

/* The clipboard of the Windows display server, on Windows' own clipboard, with
 * a core that the test makes. The clipboard is Windows', so the test does not
 * use the one of the PC: the clipboard belongs to a window station, and the
 * test makes one of its own, with a desktop in it, and moves the process there
 * before it has a window, a hook or the clipboard open. Then the threads that
 * the clipboard starts are in it too. It looks at where it is before it
 * touches the clipboard, and does not if that is the station of the PC. */

#include "test.h"

#include "common/debug.h"
#include "../displayservers/Win32/clipboard_win32.h"
#include "../displayservers/Win32/clipboard_format.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* the window station */

static HWINSTA g_station;
static HDESK   g_desktop;

// returns false if the process is not in a window station of its own, which is
// when the test does not go on
static bool enterOwnStation(void)
{
  HWINSTA own = GetProcessWindowStation();

  g_station = CreateWindowStationW(NULL, 0, WINSTA_ALL_ACCESS, NULL);
  if (!g_station)
    return false;

  if (!SetProcessWindowStation(g_station))
  {
    CloseWindowStation(g_station);
    return false;
  }

  g_desktop = CreateDesktopW(L"Default", NULL, NULL, 0, GENERIC_ALL, NULL);
  if (!g_desktop || !SetThreadDesktop(g_desktop))
  {
    SetProcessWindowStation(own);
    return false;
  }

  // not the station of the PC, by the handle and by the name
  wchar_t name[128] = { 0 };
  DWORD needed;
  if (GetProcessWindowStation() != g_station || g_station == own ||
      !GetUserObjectInformationW(g_station, UOI_NAME, name, sizeof(name),
        &needed) || _wcsicmp(name, L"WinSta0") == 0)
  {
    SetProcessWindowStation(own);
    return false;
  }

  return true;
}

/* what the core of the test sees */

static struct
{
  HANDLE      notified;
  HANDLE      dataEvent;
  LONG        notifies;
  LONG        releases;
  LONG        datas;
  LONG        aborts;
  LONG        requests;
  LG_ClipboardData types[8];
  size_t      typeCount;
  LG_ClipboardRequest dataRequest;
  LG_ClipboardData dataType;
  uint8_t     dataBytes[4096];
  size_t      dataSize;

  // what the guest answers
  const void * replyData;
  size_t       replySize;
}
g_core;

static void coreNotifyTypes(void * o, const LG_ClipboardData types[],
    size_t count)
{
  CHECK(count <= 8);
  memcpy(g_core.types, types, count * sizeof(*types));
  g_core.typeCount = count;
  InterlockedIncrement(&g_core.notifies);
  SetEvent(g_core.notified);
}

static void coreRelease(void * o)
{
  InterlockedIncrement(&g_core.releases);
}

static void coreData(void * o, LG_ClipboardRequest request,
    LG_ClipboardData type, const void * data, size_t size)
{
  CHECK(size <= sizeof(g_core.dataBytes));
  memcpy(g_core.dataBytes, data, size);
  g_core.dataSize    = size;
  g_core.dataRequest = request;
  g_core.dataType    = type;
  InterlockedIncrement(&g_core.datas);
  SetEvent(g_core.dataEvent);
}

static void coreAbort(void * o, LG_ClipboardRequest request)
{
  InterlockedIncrement(&g_core.aborts);
}

static bool coreRequest(void * o, LG_ClipboardData type,
    LG_ClipboardReplyFn replyFn, void * replyOpaque)
{
  InterlockedIncrement(&g_core.requests);
  replyFn(replyOpaque, type, g_core.replyData, (uint32_t)g_core.replySize);
  return true;
}

/* another program: the test's own thread, which uses the clipboard as one
 * does */

static bool openForOtherProgram(void)
{
  for (unsigned i = 0; i < 100; ++i)
  {
    if (OpenClipboard(NULL))
      return true;
    Sleep(20);
  }
  return false;
}

static void copyAs(UINT format, const void * data, size_t size)
{
  CHECK(openForOtherProgram());
  CHECK(EmptyClipboard());
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
  CHECK(memory);
  void * target = GlobalLock(memory);
  CHECK(target);
  memcpy(target, data, size);
  GlobalUnlock(memory);
  CHECK(SetClipboardData(format, memory));
  CloseClipboard();
}

static uint8_t * pasteAs(UINT format, size_t * size)
{
  CHECK(openForOtherProgram());
  uint8_t * copy = NULL;
  HANDLE handle  = GetClipboardData(format);
  if (handle)
  {
    const SIZE_T length = GlobalSize(handle);
    const void * data   = GlobalLock(handle);
    if (data)
    {
      copy = malloc(length ? length : 1);
      CHECK(copy);
      memcpy(copy, data, length);
      *size = length;
      GlobalUnlock(handle);
    }
  }
  CloseClipboard();
  return copy;
}

static bool waitEvent(HANDLE event, DWORD ms)
{
  return WaitForSingleObject(event, ms) == WAIT_OBJECT_0;
}

// waits for a condition that another thread makes true
static bool eventually(bool (*check)(void))
{
  for (unsigned i = 0; i < 300; ++i)
  {
    if (check())
      return true;
    Sleep(10);
  }
  return false;
}

static bool textOnClipboard(void)
{
  return IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
}

static bool noTextOnClipboard(void)
{
  return !IsClipboardFormatAvailable(CF_UNICODETEXT);
}

static bool bitmapOnClipboard(void)
{
  return IsClipboardFormatAvailable(CF_DIB) != FALSE;
}

static bool ownedByUs(void)
{
  return GetClipboardOwner() != NULL;
}

/* the child */

static void testText(void)
{
  // copied by another program: offered, and given when the guest asks
  static const wchar_t text[] = L"café\r\nbar";
  copyAs(CF_UNICODETEXT, text, sizeof(text));
  CHECK(waitEvent(g_core.notified, 3000));
  CHECK(g_core.typeCount == 1 && g_core.types[0] == LG_CLIPBOARD_DATA_TEXT);

  win32CBRequest(7, LG_CLIPBOARD_DATA_TEXT);
  CHECK(waitEvent(g_core.dataEvent, 3000));
  CHECK(g_core.dataRequest == 7 && g_core.dataType == LG_CLIPBOARD_DATA_TEXT);
  CHECK(g_core.dataSize == 9);
  CHECK(memcmp(g_core.dataBytes, "caf\xc3\xa9\nbar", 9) == 0);
  CHECK(g_core.aborts == 0);
}

static void testGuestText(void)
{
  const LONG notifies = g_core.notifies;

  // what the guest copies is on the clipboard, and no program has been
  // given it yet
  static const char reply[] = "from the guest\nline 2";
  g_core.replyData = reply;
  g_core.replySize = sizeof(reply) - 1;
  const LONG requests = g_core.requests;

  win32CBNotice(LG_CLIPBOARD_DATA_TEXT);
  CHECK(eventually(ownedByUs));
  CHECK(eventually(textOnClipboard));
  Sleep(200);
  CHECK(g_core.requests == requests);

  // and not for the history of the clipboard, or the cloud's
  CHECK(IsClipboardFormatAvailable(RegisterClipboardFormatW(
          L"CanIncludeInClipboardHistory")));
  CHECK(IsClipboardFormatAvailable(RegisterClipboardFormatW(
          L"CanUploadToCloudClipboard")));
  CHECK(IsClipboardFormatAvailable(RegisterClipboardFormatW(
          L"ExcludeClipboardContentFromMonitorProcessing")));

  // a program pastes, and Windows asks the window to fill it in
  size_t size = 0;
  uint8_t * pasted = pasteAs(CF_UNICODETEXT, &size);
  CHECK(pasted);
  static const wchar_t expected[] = L"from the guest\r\nline 2";
  CHECK(size >= sizeof(expected));
  CHECK(memcmp(pasted, expected, sizeof(expected)) == 0);
  free(pasted);
  CHECK(g_core.requests == requests + 1);

  // the text of other formats is made from it, and is not asked for again
  pasted = pasteAs(CF_TEXT, &size);
  CHECK(pasted);
  CHECK(memcmp(pasted, "from the guest\r\nline 2", 22) == 0);
  free(pasted);
  CHECK(g_core.requests == requests + 1);

  // none of that was a copy that the guest should be told of
  Sleep(300);
  CHECK(g_core.notifies == notifies);
  CHECK(g_core.releases == 0);

  // the guest takes it back: it goes off the clipboard, and is not a copy
  win32CBRelease();
  CHECK(eventually(noTextOnClipboard));
  Sleep(200);
  CHECK(g_core.notifies == notifies);
  CHECK(g_core.releases == 0);
}

static void makeBitmap(uint8_t dib[56])
{
  memset(dib, 0, 56);
  dib[0] = 40; dib[4] = 2; dib[8] = 2; dib[12] = 1; dib[14] = 24;
  for (size_t i = 40; i < 56; ++i)
    dib[i] = (uint8_t)(i * 3);
}

static void testImages(void)
{
  uint8_t dib[56];
  makeBitmap(dib);

  // a bitmap copied by another program is offered, and sent as a BMP file
  ResetEvent(g_core.notified);
  ResetEvent(g_core.dataEvent);
  copyAs(CF_DIB, dib, sizeof(dib));
  CHECK(waitEvent(g_core.notified, 3000));
  CHECK(g_core.typeCount == 1 && g_core.types[0] == LG_CLIPBOARD_DATA_BMP);

  win32CBRequest(8, LG_CLIPBOARD_DATA_BMP);
  CHECK(waitEvent(g_core.dataEvent, 3000));
  CHECK(g_core.dataRequest == 8 && g_core.dataType == LG_CLIPBOARD_DATA_BMP);

  // a BMP file of the bitmap: the version 5 that Windows makes of it, which
  // has the header that carries an alpha channel, or the bitmap itself. Either
  // is a bitmap of 2 by 2 that the format module takes back
  CHECK(g_core.dataBytes[0] == 'B' && g_core.dataBytes[1] == 'M');
  CHECK(g_core.dataSize > 14 + 40);
  CHECK(g_core.dataBytes[18] == 2 && g_core.dataBytes[22] == 2);
  size_t dibSize = 0;
  uint8_t * back = lgClipboardDibFromBmp(g_core.dataBytes, g_core.dataSize,
      &dibSize, NULL);
  CHECK(back && dibSize > 40);
  free(back);

  // and one that the guest copied is put on the clipboard as a bitmap
  uint8_t bmp[14 + 56] = { 'B', 'M' };
  bmp[2]  = sizeof(bmp);
  bmp[10] = 54;
  memcpy(bmp + 14, dib, sizeof(dib));
  g_core.replyData = bmp;
  g_core.replySize = sizeof(bmp);

  win32CBNotice(LG_CLIPBOARD_DATA_BMP);
  CHECK(eventually(bitmapOnClipboard));
  CHECK(IsClipboardFormatAvailable(CF_BITMAP));   // which Windows makes

  size_t size = 0;
  uint8_t * pasted = pasteAs(CF_DIB, &size);
  CHECK(pasted);
  CHECK(size >= sizeof(dib));
  CHECK(memcmp(pasted, dib, sizeof(dib)) == 0);
  free(pasted);
}

static void testKeptWhenTheClientEnds(void)
{
  static const char reply[] = "kept\nafter";
  g_core.replyData = reply;
  g_core.replySize = sizeof(reply) - 1;

  win32CBNotice(LG_CLIPBOARD_DATA_TEXT);
  CHECK(eventually(ownedByUs));
  CHECK(eventually(textOnClipboard));

  // nobody pasted it: the window goes, and Windows asks it to fill in what it
  // offered, which stays on the clipboard
  win32CBStop();

  size_t size = 0;
  uint8_t * pasted = pasteAs(CF_UNICODETEXT, &size);
  CHECK(pasted);
  static const wchar_t expected[] = L"kept\r\nafter";
  CHECK(size >= sizeof(expected));
  CHECK(memcmp(pasted, expected, sizeof(expected)) == 0);
  free(pasted);
}

static int run(void)
{
  debug_init();

  g_core.notified  = CreateEventW(NULL, TRUE, FALSE, NULL);
  g_core.dataEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

  const Win32ClipboardCore core =
  {
    .notifyTypes = coreNotifyTypes,
    .release     = coreRelease,
    .data        = coreData,
    .abort       = coreAbort,
    .request     = coreRequest,
  };
  CHECK(win32CBStart(&core));

  testText();
  testGuestText();
  testImages();
  testKeptWhenTheClientEnds();

  puts("win32 clipboard real tests passed");
  return EXIT_SUCCESS;
}

int main(void)
{
  if (!enterOwnStation())
  {
    puts("win32 clipboard real tests skipped: no window station of its own");
    return EXIT_SUCCESS;
  }

  return run();
}
