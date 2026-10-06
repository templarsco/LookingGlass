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

#include "clipboard.h"
#include "clipboard_format.h"

#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* what a program that pastes may hold up the guest for, and how many requests
 * the guest can have at the window's thread before this one: the core makes
 * one at a time, and this is more than that */
#define MAX_REQUESTS 4

/* the types that are offered to the guest, in the order of the protocol */
#define TYPE_COUNT 5

struct Request
{
  bool                used;
  bool                canceled;
  LG_ClipboardRequest id;
  LG_ClipboardData    type;
};

enum RemoteAction
{
  REMOTE_NONE,
  REMOTE_NOTICE,
  REMOTE_RELEASE,
};

/* What a render waits for: the data that the guest sends. The render and the
 * reply that the core calls share it, and either may be done with it first:
 * the render when it gives up, and the reply when the core sends the data to
 * a request that nobody waits for any more. */
struct Wait
{
  LGEvent         * event;
  atomic_int        refs;
  LG_Lock           lock;
  bool              done;
  bool              abandoned;
  LG_ClipboardData  type;
  uint8_t         * data;
  size_t            size;
};

struct Win32Clipboard
{
  Win32ClipboardApi  api;
  Win32ClipboardCore core;
  unsigned           renderTimeoutMs;

  // what the hooks leave, for the thread of the window
  LG_Lock            lock;
  atomic_bool        stopped;
  enum RemoteAction  remoteAction;
  LG_ClipboardData   noticeType;
  struct Request     requests[MAX_REQUESTS];

  // what only the thread of the window uses
  bool               haveRemote;     // an offer of the guest is on the clipboard
  LG_ClipboardData   remoteType;
  bool               emptying;       // our own EmptyClipboard is going on
  bool               localKnown;     // localSequence is that of an offer
  bool               localOffered;   // the guest has been told of the types
  uint32_t           localSequence;
  LG_ClipboardData   localTypes[TYPE_COUNT];
  size_t             localCount;
};

/* formats and types */

static unsigned formatOf(const Win32Clipboard * c, LG_ClipboardData type)
{
  switch (type)
  {
    case LG_CLIPBOARD_DATA_TEXT: return WIN32_CF_UNICODETEXT;
    case LG_CLIPBOARD_DATA_PNG : return c->api.formatPng;
    case LG_CLIPBOARD_DATA_BMP : return WIN32_CF_DIB;
    case LG_CLIPBOARD_DATA_TIFF: return WIN32_CF_TIFF;
    case LG_CLIPBOARD_DATA_JPEG: return c->api.formatJfif;
    default                    : return 0;
  }
}

// does Windows ask for what an offer of this type holds?
static bool formatServes(const Win32Clipboard * c, LG_ClipboardData type,
    unsigned format)
{
  const unsigned wanted = formatOf(c, type);
  if (!wanted || !format)
    return false;
  if (type == LG_CLIPBOARD_DATA_BMP)
    return format == WIN32_CF_DIB || format == WIN32_CF_DIBV5;
  return format == wanted;
}

static bool typeKnown(LG_ClipboardData type)
{
  return type == LG_CLIPBOARD_DATA_TEXT || type == LG_CLIPBOARD_DATA_PNG ||
    type == LG_CLIPBOARD_DATA_BMP || type == LG_CLIPBOARD_DATA_TIFF ||
    type == LG_CLIPBOARD_DATA_JPEG;
}

/* waits */

static void waitUnref(struct Wait * wait)
{
  if (atomic_fetch_sub(&wait->refs, 1) != 1)
    return;

  lgFreeEvent(wait->event);
  free(wait->data);
  free(wait);
}

static struct Wait * waitCreate(LG_ClipboardData type)
{
  struct Wait * wait = calloc(1, sizeof(*wait));
  if (!wait)
    return NULL;

  wait->event = lgCreateEvent(true, 0);
  if (!wait->event)
  {
    free(wait);
    return NULL;
  }

  // one for the render and one for the reply
  atomic_init(&wait->refs, 2);
  LG_LOCK_INIT(wait->lock);
  wait->type = type;
  return wait;
}

// called by the core, on its thread, with its lock held
static void replyFn(void * opaque, LG_ClipboardData type, const uint8_t * data,
    uint32_t size)
{
  struct Wait * wait = opaque;

  LG_LOCK(wait->lock);
  if (!wait->abandoned && type == wait->type && (data || !size))
  {
    wait->data = malloc(size ? size : 1);
    if (wait->data)
    {
      if (size)
        memcpy(wait->data, data, size);
      wait->size = size;
    }
  }
  wait->done = true;
  LG_UNLOCK(wait->lock);

  lgSignalEvent(wait->event);
  waitUnref(wait);
}

/* creation */

Win32Clipboard * win32Clipboard_create(const Win32ClipboardApi * api,
    const Win32ClipboardCore * core, unsigned renderTimeoutMs)
{
  if (!api || !core || !api->wake || !api->open || !api->close ||
      !api->empty || !api->sequence || !api->isOwner || !api->hasFormat ||
      !api->get || !api->set || !api->setDelayed || !api->setPrivate ||
      !core->notifyTypes || !core->release || !core->data || !core->abort ||
      !core->request)
    return NULL;

  Win32Clipboard * c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;

  c->api             = *api;
  c->core            = *core;
  c->renderTimeoutMs = renderTimeoutMs ? renderTimeoutMs :
    WIN32_CLIPBOARD_RENDER_TIMEOUT_MS;
  LG_LOCK_INIT(c->lock);
  atomic_init(&c->stopped, false);
  return c;
}

void win32Clipboard_destroy(Win32Clipboard * c)
{
  if (!c)
    return;

  atomic_store(&c->stopped, true);
  free(c);
}

/* the hooks of the core */

void win32Clipboard_notice(Win32Clipboard * c, LG_ClipboardData type)
{
  if (atomic_load(&c->stopped) || !typeKnown(type))
    return;

  LG_LOCK(c->lock);
  c->remoteAction = REMOTE_NOTICE;
  c->noticeType   = type;
  LG_UNLOCK(c->lock);
  c->api.wake(c->api.opaque);
}

void win32Clipboard_release(Win32Clipboard * c)
{
  if (atomic_load(&c->stopped))
    return;

  LG_LOCK(c->lock);
  c->remoteAction = REMOTE_RELEASE;
  LG_UNLOCK(c->lock);
  c->api.wake(c->api.opaque);
}

void win32Clipboard_request(Win32Clipboard * c, LG_ClipboardRequest request,
    LG_ClipboardData type)
{
  if (atomic_load(&c->stopped))
    return;

  bool queued = false;
  LG_LOCK(c->lock);
  for (unsigned i = 0; i < MAX_REQUESTS && !queued; ++i)
    if (!c->requests[i].used)
    {
      c->requests[i] = (struct Request)
      {
        .used = true, .id = request, .type = type,
      };
      queued = true;
    }
  LG_UNLOCK(c->lock);

  if (queued)
    c->api.wake(c->api.opaque);
  else
    // the core asks for one at a time: it did not hear of the others
    c->core.abort(c->core.opaque, request);
}

void win32Clipboard_requestCancel(Win32Clipboard * c,
    LG_ClipboardRequest request)
{
  if (atomic_load(&c->stopped))
    return;

  LG_LOCK(c->lock);
  for (unsigned i = 0; i < MAX_REQUESTS; ++i)
    if (c->requests[i].used && c->requests[i].id == request)
      c->requests[i].canceled = true;
  LG_UNLOCK(c->lock);
}

/* what the guest copies, for Windows to paste */

static void publishRemote(Win32Clipboard * c, LG_ClipboardData type)
{
  const unsigned format = formatOf(c, type);
  if (!format)
    return;

  if (!c->api.open(c->api.opaque))
  {
    DEBUG_WARN("Could not open the clipboard to offer what the guest copied");
    return;
  }

  c->emptying = true;
  bool ok = c->api.empty(c->api.opaque);
  c->emptying = false;

  ok = ok && c->api.setDelayed(c->api.opaque, format);
  if (ok)
    c->api.setPrivate(c->api.opaque);
  c->api.close(c->api.opaque);

  if (!ok)
  {
    DEBUG_WARN("Could not offer what the guest copied on the clipboard");
    c->haveRemote = false;
    return;
  }

  c->haveRemote = true;
  c->remoteType = type;

  // the guest's offer is the one that is on the clipboard now
  c->localOffered = false;
  c->localKnown   = false;
}

// the guest has nothing to offer any more: what we put there is taken off
static void dropRemote(Win32Clipboard * c)
{
  if (!c->haveRemote)
    return;
  c->haveRemote = false;

  if (!c->api.open(c->api.opaque))
    return;

  // not what another program put there since
  if (c->api.isOwner(c->api.opaque))
  {
    c->emptying = true;
    c->api.empty(c->api.opaque);
    c->emptying = false;
  }
  c->api.close(c->api.opaque);
}

/* what is copied on the PC, for the guest to paste */

static size_t detectTypes(Win32Clipboard * c, LG_ClipboardData types[TYPE_COUNT])
{
  size_t count = 0;
  void * opaque = c->api.opaque;

  if (c->api.hasFormat(opaque, WIN32_CF_UNICODETEXT))
    types[count++] = LG_CLIPBOARD_DATA_TEXT;
  if (c->api.formatPng && c->api.hasFormat(opaque, c->api.formatPng))
    types[count++] = LG_CLIPBOARD_DATA_PNG;
  if (c->api.hasFormat(opaque, WIN32_CF_DIBV5) ||
      c->api.hasFormat(opaque, WIN32_CF_DIB))
    types[count++] = LG_CLIPBOARD_DATA_BMP;
  if (c->api.hasFormat(opaque, WIN32_CF_TIFF))
    types[count++] = LG_CLIPBOARD_DATA_TIFF;
  if (c->api.formatJfif && c->api.hasFormat(opaque, c->api.formatJfif))
    types[count++] = LG_CLIPBOARD_DATA_JPEG;

  return count;
}

void win32Clipboard_start(Win32Clipboard * c)
{
  win32Clipboard_changed(c);
}

void win32Clipboard_changed(Win32Clipboard * c)
{
  if (atomic_load(&c->stopped) || c->emptying)
    return;

  // an offer of the guest is not a copy, and nor is the clipboard that it
  // leaves when it is taken off
  if (c->api.isOwner(c->api.opaque))
    return;

  // another program owns the clipboard: the offer of the guest is gone
  c->haveRemote = false;

  /* Pasting what a program has not rendered yet makes it render it, which
   * is a change that Windows tells of. It is not another copy. */
  const uint32_t sequence = c->api.sequence(c->api.opaque);
  if (c->localKnown && sequence == c->localSequence)
    return;

  LG_ClipboardData types[TYPE_COUNT];
  const size_t count = detectTypes(c, types);

  c->localKnown    = true;
  c->localSequence = sequence;
  c->localCount    = count;
  memcpy(c->localTypes, types, sizeof(types));

  if (count)
  {
    c->localOffered = true;
    c->core.notifyTypes(c->core.opaque, types, count);
  }
  else if (c->localOffered)
  {
    c->localOffered = false;
    c->core.release(c->core.opaque);
  }
}

void win32Clipboard_lost(Win32Clipboard * c)
{
  // our own EmptyClipboard says it to us when we replace an offer
  if (!c->emptying)
    c->haveRemote = false;
}

/* The data of a type that the clipboard holds, which is of a size that the
 * core can send, or NULL. The clipboard is open. */
static uint8_t * readLocal(Win32Clipboard * c, LG_ClipboardData type,
    size_t * size)
{
  void * opaque = c->api.opaque;
  size_t rawSize = 0;
  uint8_t * raw;
  uint8_t * out = NULL;

  switch (type)
  {
    case LG_CLIPBOARD_DATA_TEXT:
      raw = c->api.get(opaque, WIN32_CF_UNICODETEXT, &rawSize);
      if (raw)
        out = lgClipboardTextFromWindows((const uint16_t *)(void *)raw,
            rawSize / 2, size);
      free(raw);
      return out;

    case LG_CLIPBOARD_DATA_BMP:
      // the version with an alpha channel first, as a program that has it
      // puts it there
      raw = c->api.hasFormat(opaque, WIN32_CF_DIBV5) ?
        c->api.get(opaque, WIN32_CF_DIBV5, &rawSize) : NULL;
      if (raw)
        out = lgClipboardBmpFromDib(raw, rawSize, size);
      free(raw);
      if (out)
        return out;

      raw = c->api.get(opaque, WIN32_CF_DIB, &rawSize);
      if (raw)
        out = lgClipboardBmpFromDib(raw, rawSize, size);
      free(raw);
      return out;

    case LG_CLIPBOARD_DATA_PNG:
      raw = c->api.get(opaque, c->api.formatPng, &rawSize);
      if (raw)
        *size = lgClipboardPngLength(raw, rawSize);
      return raw;

    case LG_CLIPBOARD_DATA_TIFF:
    case LG_CLIPBOARD_DATA_JPEG:
      raw = c->api.get(opaque, formatOf(c, type), &rawSize);
      if (raw)
        *size = rawSize;
      return raw;

    default:
      return NULL;
  }
}

static void serve(Win32Clipboard * c, LG_ClipboardRequest id,
    LG_ClipboardData type)
{
  bool offered = false;
  for (size_t i = 0; i < c->localCount; ++i)
    offered |= c->localTypes[i] == type;

  if (!c->localOffered || !offered)
  {
    c->core.abort(c->core.opaque, id);
    return;
  }

  if (!c->api.open(c->api.opaque))
  {
    DEBUG_WARN("Could not open the clipboard to give the guest what it holds");
    c->core.abort(c->core.opaque, id);
    return;
  }

  /* what was copied since is not what the guest asked for. A clipboard that
   * the guest's own offer is on, and that we own, is not a copy either */
  uint8_t * data = NULL;
  size_t size    = 0;
  if (c->api.sequence(c->api.opaque) == c->localSequence &&
      !c->api.isOwner(c->api.opaque))
  {
    data = readLocal(c, type, &size);

    // reading a format that a program renders later is a change of the
    // clipboard's number that is ours, and no other copy: the open clipboard
    // keeps anyone from changing it between
    c->localSequence = c->api.sequence(c->api.opaque);
  }
  c->api.close(c->api.opaque);

  if (!data)
  {
    c->core.abort(c->core.opaque, id);
    return;
  }

  c->core.data(c->core.opaque, id, type, data, size);
  free(data);
}

void win32Clipboard_process(Win32Clipboard * c)
{
  if (atomic_load(&c->stopped))
    return;

  for (;;)
  {
    LG_LOCK(c->lock);
    const enum RemoteAction action = c->remoteAction;
    const LG_ClipboardData  type   = c->noticeType;
    c->remoteAction = REMOTE_NONE;
    LG_UNLOCK(c->lock);

    if (action == REMOTE_NOTICE)
      publishRemote(c, type);
    else if (action == REMOTE_RELEASE)
      dropRemote(c);

    struct Request request = { 0 };
    LG_LOCK(c->lock);
    for (unsigned i = 0; i < MAX_REQUESTS; ++i)
      if (c->requests[i].used)
      {
        request = c->requests[i];
        c->requests[i].used = false;
        break;
      }
    LG_UNLOCK(c->lock);

    if (request.used && !request.canceled)
      serve(c, request.id, request.type);

    if (action == REMOTE_NONE && !request.used)
      break;
  }
}

/* what the guest copied, pasted */

static bool renderSet(Win32Clipboard * c, LG_ClipboardData type,
    unsigned format, const uint8_t * data, size_t size)
{
  bool ok = false;

  switch (type)
  {
    case LG_CLIPBOARD_DATA_TEXT:
    {
      size_t units;
      uint16_t * text = lgClipboardTextToWindows(data, size, &units);
      if (text)
        ok = c->api.set(c->api.opaque, format, text, units * sizeof(*text));
      free(text);
      break;
    }

    case LG_CLIPBOARD_DATA_BMP:
    {
      size_t dibSize;
      uint8_t * dib = lgClipboardDibFromBmp(data, size, &dibSize, NULL);
      if (dib)
        ok = c->api.set(c->api.opaque, format, dib, dibSize);
      free(dib);
      break;
    }

    default:
      ok = c->api.set(c->api.opaque, format, data, size);
      break;
  }

  if (!ok)
    DEBUG_WARN("Could not put what the guest copied on the clipboard");
  return ok;
}

bool win32Clipboard_render(Win32Clipboard * c, unsigned format)
{
  if (atomic_load(&c->stopped) || !c->haveRemote ||
      !formatServes(c, c->remoteType, format))
    return false;

  const LG_ClipboardData type = c->remoteType;
  struct Wait * wait = waitCreate(type);
  if (!wait)
    return false;

  // the reply may come before this returns, and the core cannot call it off
  if (!c->core.request(c->core.opaque, type, replyFn, wait))
  {
    waitUnref(wait);
    waitUnref(wait);
    return false;
  }

  lgWaitEvent(wait->event, c->renderTimeoutMs);

  LG_LOCK(wait->lock);
  if (!wait->done)
    wait->abandoned = true;
  uint8_t * data = wait->data;
  const size_t size = wait->size;
  wait->data = NULL;
  LG_UNLOCK(wait->lock);
  waitUnref(wait);

  if (!data)
  {
    DEBUG_WARN("Could not get what the guest copied");
    return false;
  }

  const bool ok = renderSet(c, type, format, data, size);
  free(data);
  return ok;
}

void win32Clipboard_renderAll(Win32Clipboard * c)
{
  if (atomic_load(&c->stopped) || !c->haveRemote)
    return;

  // the window is going away, and what it offered goes with it unless it is
  // filled in now
  const LG_ClipboardData type = c->remoteType;
  if (c->api.open(c->api.opaque))
  {
    if (c->api.isOwner(c->api.opaque))
      win32Clipboard_render(c, formatOf(c, type));
    c->api.close(c->api.opaque);
  }
  c->haveRemote = false;
}
