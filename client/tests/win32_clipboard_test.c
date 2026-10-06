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

/* The clipboard of the Windows display server, with a clipboard that the test
 * makes and a core that records what it is told. What Windows does is
 * done here by the test, in the order that it does it in: a message that the
 * clipboard changed, a request to render a format that was left for later.
 * Nothing here touches the clipboard of the PC. */

#include "test.h"

#include "common/debug.h"
#include "../displayservers/Win32/clipboard.h"
#include "../displayservers/Win32/clipboard_format.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FORMAT_PNG   0xC001
#define FORMAT_JFIF  0xC002

/* the clipboard of the test */

#define MAX_ITEMS 16

struct Item
{
  unsigned  format;
  bool      delayed;
  uint8_t * data;
  size_t    size;
};

struct Clipboard
{
  Win32Clipboard * lib;

  struct Item items[MAX_ITEMS];
  size_t      count;
  bool        opened;
  bool        ownerIsUs;
  uint32_t    sequence;
  unsigned    openFailures;      // opens that fail before one works
  bool        privateSet;
  unsigned    wakes;
  unsigned    empties;
  unsigned    gets;
  bool        getBumpsSequence;  // what a program that renders late does
  unsigned    pngToDibCalls;
};

static void clear(struct Clipboard * cb)
{
  for (size_t i = 0; i < cb->count; ++i)
    free(cb->items[i].data);
  cb->count      = 0;
  cb->privateSet = false;
}

static struct Item * find(struct Clipboard * cb, unsigned format)
{
  for (size_t i = 0; i < cb->count; ++i)
    if (cb->items[i].format == format)
      return &cb->items[i];
  return NULL;
}

static void put(struct Clipboard * cb, unsigned format, const void * data,
    size_t size, bool delayed)
{
  struct Item * item = find(cb, format);
  if (!item)
  {
    CHECK(cb->count < MAX_ITEMS);
    item = &cb->items[cb->count++];
    item->data = NULL;
  }
  free(item->data);
  item->format  = format;
  item->delayed = delayed;
  item->size    = size;
  item->data    = NULL;
  if (!delayed)
  {
    item->data = malloc(size ? size : 1);
    CHECK(item->data);
    if (size)
      memcpy(item->data, data, size);
  }
}

// the table that the library gets

static void apiWake(void * o)
{
  ++((struct Clipboard *)o)->wakes;
}

static bool apiOpen(void * o)
{
  struct Clipboard * cb = o;
  CHECK(!cb->opened);
  if (cb->openFailures)
  {
    --cb->openFailures;
    return false;
  }
  cb->opened = true;
  return true;
}

static void apiClose(void * o)
{
  struct Clipboard * cb = o;
  CHECK(cb->opened);
  cb->opened = false;
}

static bool apiEmpty(void * o)
{
  struct Clipboard * cb = o;
  CHECK(cb->opened);

  // Windows tells the owner that it is not the owner any more, and the owner
  // that is asked is the one that empties it
  const bool wasUs = cb->ownerIsUs;
  clear(cb);
  cb->ownerIsUs = true;
  ++cb->sequence;
  ++cb->empties;
  if (wasUs)
    win32Clipboard_lost(cb->lib);
  return true;
}

static uint32_t apiSequence(void * o)
{
  return ((struct Clipboard *)o)->sequence;
}

static bool apiIsOwner(void * o)
{
  return ((struct Clipboard *)o)->ownerIsUs;
}

static bool apiHasFormat(void * o, unsigned format)
{
  return find(o, format) != NULL;
}

static uint8_t * apiGet(void * o, unsigned format, size_t * size)
{
  struct Clipboard * cb = o;
  CHECK(cb->opened);
  ++cb->gets;

  struct Item * item = find(cb, format);
  if (!item || item->delayed)
    return NULL;

  // a program that renders late renders when it is asked, and that is a change
  if (cb->getBumpsSequence)
    ++cb->sequence;

  uint8_t * copy = malloc(item->size ? item->size : 1);
  CHECK(copy);
  memcpy(copy, item->data, item->size);
  *size = item->size;
  return copy;
}

static bool apiSet(void * o, unsigned format, const void * data, size_t size)
{
  struct Clipboard * cb = o;
  put(cb, format, data, size, false);
  return true;
}

static bool apiSetDelayed(void * o, unsigned format)
{
  struct Clipboard * cb = o;
  CHECK(cb->opened && cb->ownerIsUs);
  put(cb, format, NULL, 0, true);
  return true;
}

static bool apiSetPrivate(void * o)
{
  struct Clipboard * cb = o;
  CHECK(cb->opened && cb->ownerIsUs);
  cb->privateSet = true;
  return true;
}

// a bitmap of one pixel, whatever the PNG is: the converter is not the test's
static const uint8_t g_fakeDib[] =
{
  40, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 32, 0,
  0, 0, 0, 0,  4, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
  0, 0, 0, 0,  0, 0, 0, 0,  0xAA, 0xBB, 0xCC, 0xDD,
};

static uint8_t * apiPngToDib(void * o, const uint8_t * png, size_t size,
    size_t * dibSize)
{
  struct Clipboard * cb = o;
  ++cb->pngToDibCalls;

  uint8_t * copy = malloc(sizeof(g_fakeDib));
  CHECK(copy);
  memcpy(copy, g_fakeDib, sizeof(g_fakeDib));
  *dibSize = sizeof(g_fakeDib);
  return copy;
}

/* the core of the test */

enum Reply
{
  REPLY_NOW,        // before request() returns
  REPLY_LATER,      // from another thread, after replyDelayMs
  REPLY_NEVER,
  REPLY_REFUSE,     // request() says no
  REPLY_NONE_TYPE,  // the core cancels it: the type is NONE
  REPLY_WRONG_TYPE, // an answer for another type than it was asked
};

struct Core
{
  // what it was told
  unsigned          notifies;
  LG_ClipboardData  types[8];
  size_t            typeCount;
  unsigned          releases;
  unsigned          datas;
  LG_ClipboardRequest dataRequest;
  LG_ClipboardData  dataType;
  uint8_t           dataBytes[4096];
  size_t            dataSize;
  unsigned          aborts;
  LG_ClipboardRequest abortRequest;

  // what it does when it is asked for the guest's data
  unsigned          requests;
  LG_ClipboardData  requestedType;
  enum Reply        reply;
  const void      * replyData;
  size_t            replySize;
  unsigned          replyDelayMs;

  // a reply that was kept, and sent to the library by the test
  LG_ClipboardReplyFn keptFn;
  void              * keptOpaque;
  pthread_t           thread;
  bool                threadStarted;
};

static void coreNotify(void * o, const LG_ClipboardData types[], size_t count)
{
  struct Core * core = o;
  ++core->notifies;
  CHECK(count <= 8);
  memcpy(core->types, types, count * sizeof(*types));
  core->typeCount = count;
}

static void coreRelease(void * o)
{
  ++((struct Core *)o)->releases;
}

static void coreData(void * o, LG_ClipboardRequest request,
    LG_ClipboardData type, const void * data, size_t size)
{
  struct Core * core = o;
  ++core->datas;
  core->dataRequest = request;
  core->dataType    = type;
  CHECK(size <= sizeof(core->dataBytes));
  memcpy(core->dataBytes, data, size);
  core->dataSize = size;
}

static void coreAbort(void * o, LG_ClipboardRequest request)
{
  struct Core * core = o;
  ++core->aborts;
  core->abortRequest = request;
}

static void * replyThread(void * o)
{
  struct Core * core = o;
  usleep(core->replyDelayMs * 1000);
  core->keptFn(core->keptOpaque, core->requestedType, core->replyData,
      (uint32_t)core->replySize);
  return NULL;
}

static bool coreRequest(void * o, LG_ClipboardData type,
    LG_ClipboardReplyFn replyFn, void * replyOpaque)
{
  struct Core * core = o;
  ++core->requests;
  core->requestedType = type;

  switch (core->reply)
  {
    case REPLY_NOW:
      replyFn(replyOpaque, type, core->replyData, (uint32_t)core->replySize);
      return true;

    case REPLY_LATER:
      core->keptFn     = replyFn;
      core->keptOpaque = replyOpaque;
      CHECK(pthread_create(&core->thread, NULL, replyThread, core) == 0);
      core->threadStarted = true;
      return true;

    case REPLY_NEVER:
      core->keptFn     = replyFn;
      core->keptOpaque = replyOpaque;
      return true;

    case REPLY_NONE_TYPE:
      replyFn(replyOpaque, LG_CLIPBOARD_DATA_NONE, NULL, 0);
      return true;

    case REPLY_WRONG_TYPE:
      replyFn(replyOpaque, LG_CLIPBOARD_DATA_PNG, core->replyData,
          (uint32_t)core->replySize);
      return true;

    case REPLY_REFUSE:
      return false;
  }
  return false;
}

/* making one */

struct Setup
{
  struct Clipboard cb;
  struct Core      core;
};

static struct Setup * setupWith(unsigned timeoutMs, bool decoder)
{
  struct Setup * s = calloc(1, sizeof(*s));
  CHECK(s);

  const Win32ClipboardApi api =
  {
    .opaque     = &s->cb,
    .formatPng  = FORMAT_PNG,
    .formatJfif = FORMAT_JFIF,
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
    .pngToDib   = decoder ? apiPngToDib : NULL,
  };
  const Win32ClipboardCore coreTable =
  {
    .opaque      = &s->core,
    .notifyTypes = coreNotify,
    .release     = coreRelease,
    .data        = coreData,
    .abort       = coreAbort,
    .request     = coreRequest,
  };

  s->cb.lib = win32Clipboard_create(&api, &coreTable, timeoutMs);
  CHECK(s->cb.lib);
  return s;
}

static struct Setup * setup(unsigned timeoutMs)
{
  return setupWith(timeoutMs, false);
}

static void finish(struct Setup * s)
{
  if (s->core.threadStarted)
    pthread_join(s->core.thread, NULL);
  win32Clipboard_destroy(s->cb.lib);
  clear(&s->cb);
  free(s);
}

// another program copies: its formats, and Windows saying so
static void copy(struct Setup * s, size_t count, const unsigned formats[],
    const void * const data[], const size_t sizes[])
{
  clear(&s->cb);
  for (size_t i = 0; i < count; ++i)
    put(&s->cb, formats[i], data[i], sizes[i], false);
  s->cb.ownerIsUs = false;
  ++s->cb.sequence;
  win32Clipboard_changed(s->cb.lib);
}

#define U16(...) (const uint16_t[]) { __VA_ARGS__ }

static void copyText(struct Setup * s, const uint16_t * text, size_t units)
{
  const unsigned formats[] = { WIN32_CF_UNICODETEXT };
  const void * data[]      = { text };
  const size_t sizes[]     = { units * sizeof(*text) };
  copy(s, 1, formats, data, sizes);
}

static bool hasTypes(const struct Core * core, size_t count,
    const LG_ClipboardData expected[])
{
  return core->typeCount == count &&
    memcmp(core->types, expected, count * sizeof(*expected)) == 0;
}

/* what is copied on the PC */

static void testLocalText(void)
{
  struct Setup * s = setup(1000);

  // "Olá" and a line, in the UTF-16 with a NUL that a program puts there
  copyText(s, U16('O', 'l', 0x00E1, '\r', '\n', 'x', 0), 7);
  CHECK(s->core.notifies == 1);
  CHECK(hasTypes(&s->core, 1, (LG_ClipboardData[]) { LG_CLIPBOARD_DATA_TEXT }));

  // the guest pastes
  win32Clipboard_request(s->cb.lib, 11, LG_CLIPBOARD_DATA_TEXT);
  CHECK(s->cb.wakes == 1);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->core.datas == 1 && s->core.aborts == 0);
  CHECK(s->core.dataRequest == 11 && s->core.dataType == LG_CLIPBOARD_DATA_TEXT);
  CHECK(s->core.dataSize == 6);
  CHECK(memcmp(s->core.dataBytes, "Ol\xc3\xa1\nx", 6) == 0);
  CHECK(!s->cb.opened);

  finish(s);
}

static void testLocalNotChanged(void)
{
  struct Setup * s = setup(1000);
  s->cb.getBumpsSequence = true;

  copyText(s, U16('a', 'b', 0), 3);
  CHECK(s->core.notifies == 1);

  // a message that nothing changed, which Windows sends all the same
  win32Clipboard_changed(s->cb.lib);
  CHECK(s->core.notifies == 1);

  // the program that owns it renders when it is read, which is a change of
  // the clipboard's number, and a message for us, and no other copy
  win32Clipboard_request(s->cb.lib, 1, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->core.datas == 1);
  win32Clipboard_changed(s->cb.lib);
  CHECK(s->core.notifies == 1);

  // and it can be read again, as that did not make it another copy
  win32Clipboard_request(s->cb.lib, 2, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->core.datas == 2 && s->core.dataRequest == 2);
  CHECK(s->core.aborts == 0);

  // a real copy after that is one
  copyText(s, U16('c', 0), 2);
  CHECK(s->core.notifies == 2);

  finish(s);
}

static void testLocalChangedBeforeTheGuestAsks(void)
{
  struct Setup * s = setup(1000);

  copyText(s, U16('a', 0), 2);
  CHECK(s->core.notifies == 1);

  // something else is copied, and the message has not been handled yet
  const unsigned formats[] = { WIN32_CF_UNICODETEXT };
  const uint16_t other[]   = { 'b', 0 };
  const void * data[]      = { other };
  const size_t sizes[]     = { sizeof(other) };
  clear(&s->cb);
  put(&s->cb, formats[0], data[0], sizes[0], false);
  ++s->cb.sequence;

  win32Clipboard_request(s->cb.lib, 5, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->core.datas == 0);
  CHECK(s->core.aborts == 1 && s->core.abortRequest == 5);
  CHECK(!s->cb.opened);

  finish(s);
}

static void testLocalImages(void)
{
  struct Setup * s = setup(1000);

  // a bitmap of 2x2 at 24 bits, and a PNG that Windows padded
  uint8_t dib[56] = { 0 };
  dib[0] = 40; dib[4] = 2; dib[8] = 2; dib[12] = 1; dib[14] = 24;
  for (size_t i = 40; i < sizeof(dib); ++i)
    dib[i] = (uint8_t)i;

  // a PNG that is only a signature and its end, and what Windows pads it with
  static const uint8_t pngHead[8] =
    { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  uint8_t realPng[20] = { 0 };
  memcpy(realPng, pngHead, 8);
  memcpy(realPng + 12, "IEND", 4);
  uint8_t png[64] = { 0 };
  memcpy(png, realPng, sizeof(realPng));

  const unsigned formats[] = { WIN32_CF_DIB, FORMAT_PNG };
  const void * data[]      = { dib, png };
  const size_t sizes[]     = { sizeof(dib), sizeof(png) };
  copy(s, 2, formats, data, sizes);

  // PNG comes before the bitmap, as the protocol lists them
  CHECK(s->core.notifies == 1);
  CHECK(hasTypes(&s->core, 2, (LG_ClipboardData[])
        { LG_CLIPBOARD_DATA_PNG, LG_CLIPBOARD_DATA_BMP }));

  win32Clipboard_request(s->cb.lib, 1, LG_CLIPBOARD_DATA_BMP);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->core.datas == 1 && s->core.dataType == LG_CLIPBOARD_DATA_BMP);
  CHECK(s->core.dataSize == 14 + sizeof(dib));
  CHECK(s->core.dataBytes[0] == 'B' && s->core.dataBytes[1] == 'M');
  CHECK(memcmp(s->core.dataBytes + 14, dib, sizeof(dib)) == 0);

  win32Clipboard_request(s->cb.lib, 2, LG_CLIPBOARD_DATA_PNG);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->core.datas == 2 && s->core.dataType == LG_CLIPBOARD_DATA_PNG);
  CHECK(s->core.dataSize == sizeof(realPng));
  CHECK(memcmp(s->core.dataBytes, realPng, sizeof(realPng)) == 0);

  // a type that was not offered is not given
  win32Clipboard_request(s->cb.lib, 3, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->core.aborts == 1 && s->core.abortRequest == 3);

  finish(s);
}

static void testLocalImageOfVersion5(void)
{
  struct Setup * s = setup(1000);

  // a bitmap with an alpha channel is on the clipboard as its version 5
  // header, and as the one that has none: the first is what is sent
  uint8_t v5[124 + 16] = { 0 };
  v5[0] = 124; v5[4] = 2; v5[8] = 2; v5[12] = 1; v5[14] = 32; v5[16] = 3;
  uint8_t plain[40 + 12 + 16] = { 0 };
  plain[0] = 40; plain[4] = 2; plain[8] = 2; plain[12] = 1; plain[14] = 32;
  plain[16] = 3;

  const unsigned formats[] = { WIN32_CF_DIB, WIN32_CF_DIBV5 };
  const void * data[]      = { plain, v5 };
  const size_t sizes[]     = { sizeof(plain), sizeof(v5) };
  copy(s, 2, formats, data, sizes);
  CHECK(hasTypes(&s->core, 1, (LG_ClipboardData[]) { LG_CLIPBOARD_DATA_BMP }));

  win32Clipboard_request(s->cb.lib, 1, LG_CLIPBOARD_DATA_BMP);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->core.datas == 1);
  CHECK(s->core.dataSize == 14 + sizeof(v5));

  finish(s);
}

static void testLocalCleared(void)
{
  struct Setup * s = setup(1000);

  copyText(s, U16('a', 0), 2);
  CHECK(s->core.notifies == 1 && s->core.releases == 0);

  // a program empties the clipboard, which tells the guest that there is no
  // offer of ours, and it is told once
  clear(&s->cb);
  s->cb.ownerIsUs = false;
  ++s->cb.sequence;
  win32Clipboard_changed(s->cb.lib);
  CHECK(s->core.releases == 1);
  win32Clipboard_changed(s->cb.lib);
  CHECK(s->core.releases == 1);

  // a clipboard that has nothing, and had nothing, says nothing
  clear(&s->cb);
  ++s->cb.sequence;
  win32Clipboard_changed(s->cb.lib);
  CHECK(s->core.releases == 1 && s->core.notifies == 1);

  finish(s);
}

static void testStart(void)
{
  struct Setup * s = setup(1000);

  // what was copied before the client started is offered
  const uint16_t text[] = { 'h', 'i', 0 };
  put(&s->cb, WIN32_CF_UNICODETEXT, text, sizeof(text), false);
  s->cb.sequence = 41;
  win32Clipboard_start(s->cb.lib);
  CHECK(s->core.notifies == 1);
  CHECK(hasTypes(&s->core, 1, (LG_ClipboardData[]) { LG_CLIPBOARD_DATA_TEXT }));

  finish(s);
}

/* what the guest copies */

static void testGuestText(void)
{
  struct Setup * s = setup(1000);
  static const char reply[] = "h\xc3\xa9llo\nworld";
  s->core.reply     = REPLY_NOW;
  s->core.replyData = reply;
  s->core.replySize = sizeof(reply) - 1;

  // an offer of the guest is on the clipboard, and nothing is in it yet
  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_TEXT);
  CHECK(s->cb.wakes == 1);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 1 && s->cb.items[0].format == WIN32_CF_UNICODETEXT);
  CHECK(s->cb.items[0].delayed);
  CHECK(s->cb.ownerIsUs && s->cb.privateSet);
  CHECK(!s->cb.opened);

  // the message that our own emptying brings is not a copy, and
  // changes nothing
  win32Clipboard_changed(s->cb.lib);
  CHECK(s->core.notifies == 0 && s->core.releases == 0);

  // a program pastes
  CHECK(win32Clipboard_render(s->cb.lib, WIN32_CF_UNICODETEXT));
  CHECK(s->core.requests == 1 && s->core.requestedType == LG_CLIPBOARD_DATA_TEXT);
  struct Item * item = find(&s->cb, WIN32_CF_UNICODETEXT);
  CHECK(item && !item->delayed);
  static const uint16_t expected[] =
    { 'h', 0x00E9, 'l', 'l', 'o', '\r', '\n', 'w', 'o', 'r', 'l', 'd', 0 };
  CHECK(item->size == sizeof(expected));
  CHECK(memcmp(item->data, expected, sizeof(expected)) == 0);

  // and that is not a copy either
  win32Clipboard_changed(s->cb.lib);
  CHECK(s->core.notifies == 0);

  finish(s);
}

static void testGuestImage(void)
{
  struct Setup * s = setup(1000);

  // a BMP file of 2x2 at 24 bits
  uint8_t bmp[14 + 56] = { 'B', 'M' };
  bmp[2] = sizeof(bmp);
  bmp[10] = 54;
  bmp[14] = 40; bmp[18] = 2; bmp[22] = 2; bmp[26] = 1; bmp[28] = 24;
  for (size_t i = 54; i < sizeof(bmp); ++i)
    bmp[i] = (uint8_t)i;
  s->core.reply     = REPLY_NOW;
  s->core.replyData = bmp;
  s->core.replySize = sizeof(bmp);

  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_BMP);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 1 && s->cb.items[0].format == WIN32_CF_DIB);

  // Windows makes the other versions of a bitmap itself, by asking for this
  CHECK(!win32Clipboard_render(s->cb.lib, WIN32_CF_TIFF));
  CHECK(win32Clipboard_render(s->cb.lib, WIN32_CF_DIB));
  struct Item * item = find(&s->cb, WIN32_CF_DIB);
  CHECK(item && !item->delayed && item->size == sizeof(bmp) - 14);
  CHECK(memcmp(item->data, bmp + 14, item->size) == 0);

  // a PNG is kept as it came
  static const uint8_t png[] = { 0x89, 'P', 'N', 'G', 1, 2, 3 };
  s->core.replyData = png;
  s->core.replySize = sizeof(png);
  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_PNG);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 1 && s->cb.items[0].format == FORMAT_PNG);
  CHECK(win32Clipboard_render(s->cb.lib, FORMAT_PNG));
  item = find(&s->cb, FORMAT_PNG);
  CHECK(item && item->size == sizeof(png));
  CHECK(memcmp(item->data, png, sizeof(png)) == 0);

  finish(s);
}

static void testGuestPng(void)
{
  struct Setup * s = setupWith(1000, true);
  static const uint8_t png[] = { 0x89, 'P', 'N', 'G', 1, 2, 3, 4 };
  s->core.reply     = REPLY_NOW;
  s->core.replyData = png;
  s->core.replySize = sizeof(png);

  // the PNG, and the bitmap that Windows has no PNG to ask for
  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_PNG);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 2);
  CHECK(find(&s->cb, FORMAT_PNG) && find(&s->cb, FORMAT_PNG)->delayed);
  CHECK(find(&s->cb, WIN32_CF_DIB) && find(&s->cb, WIN32_CF_DIB)->delayed);

  // the bitmap is made of what the guest sent
  CHECK(win32Clipboard_render(s->cb.lib, WIN32_CF_DIB));
  CHECK(s->core.requests == 1 && s->cb.pngToDibCalls == 1);
  struct Item * item = find(&s->cb, WIN32_CF_DIB);
  CHECK(item && !item->delayed && item->size == sizeof(g_fakeDib));
  CHECK(memcmp(item->data, g_fakeDib, sizeof(g_fakeDib)) == 0);

  // and the PNG is not asked for again
  CHECK(win32Clipboard_render(s->cb.lib, FORMAT_PNG));
  CHECK(s->core.requests == 1 && s->cb.pngToDibCalls == 1);
  item = find(&s->cb, FORMAT_PNG);
  CHECK(item && !item->delayed && item->size == sizeof(png));
  CHECK(memcmp(item->data, png, sizeof(png)) == 0);

  // a version 5 bitmap asked for is made of it, as Windows may ask for it
  CHECK(win32Clipboard_render(s->cb.lib, WIN32_CF_DIBV5));
  CHECK(s->core.requests == 1 && s->cb.pngToDibCalls == 2);

  // another offer is not the one that was kept
  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_PNG);
  win32Clipboard_process(s->cb.lib);
  CHECK(win32Clipboard_render(s->cb.lib, FORMAT_PNG));
  CHECK(s->core.requests == 2);

  finish(s);
}

static void testGuestPngWithoutADecoder(void)
{
  struct Setup * s = setupWith(1000, false);
  static const uint8_t png[] = { 0x89, 'P', 'N', 'G' };
  s->core.reply     = REPLY_NOW;
  s->core.replyData = png;
  s->core.replySize = sizeof(png);

  // only a PNG can be offered, and a bitmap is not asked for
  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_PNG);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 1 && find(&s->cb, FORMAT_PNG));
  CHECK(!win32Clipboard_render(s->cb.lib, WIN32_CF_DIB));
  CHECK(s->core.requests == 0);
  CHECK(win32Clipboard_render(s->cb.lib, FORMAT_PNG));

  finish(s);
}

static void testGuestPngKeptWhenTheWindowGoes(void)
{
  struct Setup * s = setupWith(1000, true);
  static const uint8_t png[] = { 0x89, 'P', 'N', 'G', 9 };
  s->core.reply     = REPLY_NOW;
  s->core.replyData = png;
  s->core.replySize = sizeof(png);

  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_PNG);
  win32Clipboard_process(s->cb.lib);

  // both formats are filled in, from one answer of the guest
  win32Clipboard_renderAll(s->cb.lib);
  CHECK(find(&s->cb, FORMAT_PNG) && !find(&s->cb, FORMAT_PNG)->delayed);
  CHECK(find(&s->cb, WIN32_CF_DIB) && !find(&s->cb, WIN32_CF_DIB)->delayed);
  CHECK(s->core.requests == 1);

  finish(s);
}

static void testGuestOfferReplaced(void)
{
  struct Setup * s = setup(1000);

  // two offers before the thread looks: the last is the one
  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_PNG);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 1 && s->cb.items[0].format == FORMAT_PNG);
  CHECK(s->cb.empties == 1);

  // one after the other: the first is gone, and Windows says so to us, which
  // is not another program copying
  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_BMP);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 1 && s->cb.items[0].format == WIN32_CF_DIB);
  CHECK(s->cb.empties == 2);
  CHECK(s->core.notifies == 0 && s->core.releases == 0);

  // what was offered before is not what a request for the other is for
  s->core.reply = REPLY_NOW;
  CHECK(!win32Clipboard_render(s->cb.lib, FORMAT_PNG));
  CHECK(s->core.requests == 0);

  finish(s);
}

static void testGuestRelease(void)
{
  struct Setup * s = setup(1000);

  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 1);

  // the guest has no more to offer: the offer is taken off, and the guest is
  // not told that its clipboard is empty, which it would take for ours
  win32Clipboard_release(s->cb.lib);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 0);
  win32Clipboard_changed(s->cb.lib);
  CHECK(s->core.releases == 0 && s->core.notifies == 0);

  // and it is not asked for
  CHECK(!win32Clipboard_render(s->cb.lib, WIN32_CF_UNICODETEXT));
  CHECK(s->core.requests == 0);

  finish(s);
}

static void testGuestReleaseAfterACopy(void)
{
  struct Setup * s = setup(1000);

  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);

  // a program copies over the offer, and then the guest withdraws it: what the
  // program copied stays
  copyText(s, U16('m', 'i', 'n', 'e', 0), 5);
  CHECK(s->core.notifies == 1);
  win32Clipboard_release(s->cb.lib);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 1 && find(&s->cb, WIN32_CF_UNICODETEXT));
  CHECK(!find(&s->cb, WIN32_CF_UNICODETEXT)->delayed);

  finish(s);
}

static void testGuestOfferCopiedOver(void)
{
  struct Setup * s = setup(1000);
  s->core.reply = REPLY_NOW;

  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);

  // another program takes the clipboard: Windows tells us that we are not the
  // owner, and that is a copy, which is offered
  clear(&s->cb);
  s->cb.ownerIsUs = false;
  put(&s->cb, WIN32_CF_UNICODETEXT, U16('z', 0), 4, false);
  ++s->cb.sequence;
  win32Clipboard_lost(s->cb.lib);
  win32Clipboard_changed(s->cb.lib);
  CHECK(s->core.notifies == 1);

  // and the guest's offer is not rendered into it
  CHECK(!win32Clipboard_render(s->cb.lib, WIN32_CF_UNICODETEXT));
  CHECK(s->core.requests == 0);

  finish(s);
}

static void testRenderTimeout(void)
{
  struct Setup * s = setup(60);
  s->core.reply = REPLY_NEVER;

  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);

  CHECK(!win32Clipboard_render(s->cb.lib, WIN32_CF_UNICODETEXT));
  CHECK(s->core.requests == 1);
  CHECK(find(&s->cb, WIN32_CF_UNICODETEXT)->delayed);

  // the guest answers when nobody waits for it: nothing is read, or freed
  // twice, which the sanitizers see
  static const char late[] = "late";
  s->core.keptFn(s->core.keptOpaque, LG_CLIPBOARD_DATA_TEXT,
      (const uint8_t *)late, sizeof(late) - 1);
  CHECK(find(&s->cb, WIN32_CF_UNICODETEXT)->delayed);

  finish(s);
}

static void testRenderRefused(void)
{
  struct Setup * s = setup(1000);
  s->core.reply = REPLY_REFUSE;

  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);
  CHECK(!win32Clipboard_render(s->cb.lib, WIN32_CF_UNICODETEXT));
  CHECK(s->core.requests == 1);

  // the core cancels it, which is an answer of no type
  s->core.reply = REPLY_NONE_TYPE;
  CHECK(!win32Clipboard_render(s->cb.lib, WIN32_CF_UNICODETEXT));
  CHECK(s->core.requests == 2);

  // an answer of another type than the one that was asked for
  static const char other[] = "x";
  s->core.reply     = REPLY_WRONG_TYPE;
  s->core.replyData = other;
  s->core.replySize = 1;
  CHECK(!win32Clipboard_render(s->cb.lib, WIN32_CF_UNICODETEXT));
  CHECK(find(&s->cb, WIN32_CF_UNICODETEXT)->delayed);

  // and one of the type
  s->core.reply = REPLY_NOW;
  CHECK(win32Clipboard_render(s->cb.lib, WIN32_CF_UNICODETEXT));

  finish(s);
}

static void testRenderFromAnotherThread(void)
{
  struct Setup * s = setup(2000);
  static const char reply[] = "from the guest";
  s->core.reply        = REPLY_LATER;
  s->core.replyData    = reply;
  s->core.replySize    = sizeof(reply) - 1;
  s->core.replyDelayMs = 100;

  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);

  CHECK(win32Clipboard_render(s->cb.lib, WIN32_CF_UNICODETEXT));
  struct Item * item = find(&s->cb, WIN32_CF_UNICODETEXT);
  CHECK(item && item->size == (sizeof(reply)) * 2);

  finish(s);
}

static void testRenderAll(void)
{
  struct Setup * s = setup(1000);
  static const char reply[] = "kept";
  s->core.reply     = REPLY_NOW;
  s->core.replyData = reply;
  s->core.replySize = sizeof(reply) - 1;

  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);

  // the window is going away: what the guest copied is put on the clipboard
  win32Clipboard_renderAll(s->cb.lib);
  struct Item * item = find(&s->cb, WIN32_CF_UNICODETEXT);
  CHECK(item && !item->delayed && item->size == (sizeof(reply)) * 2);
  CHECK(!s->cb.opened);

  // and again does nothing
  win32Clipboard_renderAll(s->cb.lib);
  CHECK(s->core.requests == 1);

  finish(s);
}

static void testOpenFails(void)
{
  struct Setup * s = setup(1000);

  // another program has the clipboard open: the offer is not made, and
  // nothing is rendered for it
  s->cb.openFailures = 1;
  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 0 && s->cb.empties == 0);
  CHECK(!win32Clipboard_render(s->cb.lib, WIN32_CF_UNICODETEXT));

  // and a request for what is copied is answered with a no
  copyText(s, U16('a', 0), 2);
  s->cb.openFailures = 1;
  win32Clipboard_request(s->cb.lib, 9, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->core.aborts == 1 && s->core.abortRequest == 9);

  finish(s);
}

static void testRequests(void)
{
  struct Setup * s = setup(1000);
  copyText(s, U16('a', 0), 2);

  // canceled before the thread looks: nothing is said to the core
  win32Clipboard_request(s->cb.lib, 7, LG_CLIPBOARD_DATA_TEXT);
  win32Clipboard_requestCancel(s->cb.lib, 7);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->core.datas == 0 && s->core.aborts == 0);

  // more than it can hold: the last is said no to at once
  for (LG_ClipboardRequest id = 20; id < 25; ++id)
    win32Clipboard_request(s->cb.lib, id, LG_CLIPBOARD_DATA_TEXT);
  CHECK(s->core.aborts == 1 && s->core.abortRequest == 24);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->core.datas == 4 && s->core.aborts == 1);

  // a cancel of one that is not there is nothing
  win32Clipboard_requestCancel(s->cb.lib, 999);

  finish(s);
}

static void testTypesTheGuestCannotHave(void)
{
  struct Setup * s = setup(1000);

  // files, and a type that does not exist: no offer
  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_FILES);
  win32Clipboard_notice(s->cb.lib, LG_CLIPBOARD_DATA_NONE);
  win32Clipboard_process(s->cb.lib);
  CHECK(s->cb.count == 0 && s->cb.empties == 0);

  finish(s);
}

int main(void)
{
  debug_init();

  testLocalText();
  testLocalNotChanged();
  testLocalChangedBeforeTheGuestAsks();
  testLocalImages();
  testLocalImageOfVersion5();
  testLocalCleared();
  testStart();
  testGuestText();
  testGuestImage();
  testGuestPng();
  testGuestPngWithoutADecoder();
  testGuestPngKeptWhenTheWindowGoes();
  testGuestOfferReplaced();
  testGuestRelease();
  testGuestReleaseAfterACopy();
  testGuestOfferCopiedOver();
  testRenderTimeout();
  testRenderRefused();
  testRenderFromAnotherThread();
  testRenderAll();
  testOpenFails();
  testRequests();
  testTypesTheGuestCannotHave();

  puts("win32 clipboard tests passed");
  return EXIT_SUCCESS;
}
