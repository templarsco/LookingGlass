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

#include "vblank_source.h"

#include "common/debug.h"
#include "common/locking.h"
#include "common/windebug.h"

#include <dwmapi.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// the few definitions of the graphics kernel's thunks that this needs. They
// are in gdi32.dll, and MinGW has no d3dkmthk.h
typedef UINT LgKmtHandle;

typedef struct
{
  HDC         hDc;
  LgKmtHandle hAdapter;
  LUID        AdapterLuid;
  UINT        VidPnSourceId;
}
LgKmtOpenAdapterFromHdc;

typedef struct
{
  LgKmtHandle hAdapter;
}
LgKmtCloseAdapter;

typedef struct
{
  LgKmtHandle hAdapter;
  LgKmtHandle hDevice;
  UINT        VidPnSourceId;
}
LgKmtWaitForVerticalBlank;

LONG WINAPI D3DKMTOpenAdapterFromHdc(LgKmtOpenAdapterFromHdc *);
LONG WINAPI D3DKMTCloseAdapter(const LgKmtCloseAdapter *);
LONG WINAPI D3DKMTWaitForVerticalBlankEvent(const LgKmtWaitForVerticalBlank *);

struct Win32KmtSource
{
  /* the display to wait for: set by any thread, and read by the one that
   * waits, which opens it again when the generation moves */
  LG_Lock           lock;
  WCHAR             device[CCHDEVICENAME];
  _Atomic(unsigned) generation;

  /* the waiting thread's */
  LgKmtHandle adapter;
  UINT        source;
  unsigned    opened;
};

Win32KmtSource * win32KmtSource_create(const WCHAR * device)
{
  Win32KmtSource * source = calloc(1, sizeof(*source));
  if (!source)
  {
    DEBUG_ERROR("Out of memory");
    return NULL;
  }

  LG_LOCK_INIT(source->lock);
  atomic_init(&source->generation, 1);
  win32KmtSource_retarget(source, device);
  return source;
}

static void closeAdapter(Win32KmtSource * source)
{
  if (!source->adapter)
    return;

  const LgKmtCloseAdapter close = { source->adapter };
  D3DKMTCloseAdapter(&close);
  source->adapter = 0;
}

void win32KmtSource_destroy(Win32KmtSource * source)
{
  if (!source)
    return;

  closeAdapter(source);
  LG_LOCK_FREE(source->lock);
  free(source);
}

void win32KmtSource_retarget(Win32KmtSource * source, const WCHAR * device)
{
  LG_LOCK(source->lock);
  wcsncpy(source->device, device ? device : L"", CCHDEVICENAME - 1);
  source->device[CCHDEVICENAME - 1] = L'\0';
  LG_UNLOCK(source->lock);

  atomic_fetch_add_explicit(&source->generation, 1, memory_order_release);
}

// opens the adapter of the display that is wanted now. The display is found by
// a device context of its name, which is what the graphics kernel needs and
// the window's would not have, as it can be on another display
static bool openAdapter(Win32KmtSource * source)
{
  closeAdapter(source);

  WCHAR device[CCHDEVICENAME];
  LG_LOCK(source->lock);
  const unsigned generation = atomic_load_explicit(&source->generation,
      memory_order_acquire);
  memcpy(device, source->device, sizeof(device));
  LG_UNLOCK(source->lock);

  HDC dc = CreateDCW(device, device, NULL, NULL);
  if (!dc)
  {
    DEBUG_WINERROR("CreateDCW failed for the vertical blank", GetLastError());
    return false;
  }

  LgKmtOpenAdapterFromHdc adapter = { .hDc = dc };
  const LONG status = D3DKMTOpenAdapterFromHdc(&adapter);
  DeleteDC(dc);
  if (status)
  {
    DEBUG_WARN("D3DKMTOpenAdapterFromHdc failed: 0x%08lx",
        (unsigned long)status);
    return false;
  }

  source->adapter = adapter.hAdapter;
  source->source  = adapter.VidPnSourceId;
  source->opened  = generation;
  return true;
}

bool win32KmtSource_wait(void * opaque)
{
  Win32KmtSource * source = opaque;

  if (!source->adapter || source->opened !=
      atomic_load_explicit(&source->generation, memory_order_acquire))
    if (!openAdapter(source))
      return false;

  LgKmtWaitForVerticalBlank wait = { source->adapter, 0, source->source };
  if (!D3DKMTWaitForVerticalBlankEvent(&wait))
    return true;

  // the display changed under it, or the driver was reset: once more
  // with an adapter that is opened again
  if (!openAdapter(source))
    return false;

  wait = (LgKmtWaitForVerticalBlank) { source->adapter, 0, source->source };
  return !D3DKMTWaitForVerticalBlankEvent(&wait);
}

bool win32DwmSource_wait(void * unused)
{
  return SUCCEEDED(DwmFlush());
}
