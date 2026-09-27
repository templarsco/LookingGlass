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

/* Linux-only client features that have no Windows implementation yet. The
 * stubs report the feature as absent so that the core falls back to what the
 * display server provides. */

#include "evdev.h"
#include "core/clipboard_files.h"

#include <windows.h>

// common's Windows timers use this window when set; the client leaves it
// NULL so that they run on the thread pool instead
HWND MessageHWND = NULL;

// evdev input capture

void evdev_earlyInit(void) {}
bool evdev_start(void) { return false; }
void evdev_stop(void) {}
void evdev_free(void) {}
void evdev_setGrab(bool keyboard, bool pointer) {}
int  evdev_getEventFD(void) { return -1; }
void evdev_dispatch(void * opaque) {}
bool evdev_isExclusive(void) { return false; }
bool evdev_isPointerExclusive(void) { return false; }

// file copy and paste, which uses FUSE on Linux

bool clipboardFiles_init(void) { return true; }
void clipboardFiles_free(void) {}

bool clipboardFiles_setLocal(const char * mime, const void * data, size_t size)
{
  return false;
}

void clipboardFiles_clearLocal(void) {}

bool clipboardFiles_getRemote(const char * mime, char ** data, size_t * size)
{
  return false;
}

uint64_t clipboardFiles_remotePresentationAcquire(void) { return 0; }

bool clipboardFiles_getRemotePresentation(uint64_t presentation,
    const char * mime, char ** data, size_t * size)
{
  return false;
}

void clipboardFiles_remotePresentationDelivered(uint64_t presentation) {}
void clipboardFiles_remotePresentationRelease(uint64_t presentation) {}
bool clipboardFiles_remoteReady(uint64_t dataset) { return false; }
bool clipboardFiles_remoteOffer(uint64_t dataset) { return false; }
void clipboardFiles_remoteClear(void) {}
void clipboardFiles_providerUnavailable(void) {}

void clipboardFiles_remoteAcquired(uint64_t dataset, uint64_t acquisition,
    LG_ClipboardFileError error)
{
}

LG_ClipboardResult clipboardFiles_remoteDataBegin(
    const LG_ClipboardFileRequest * request, uint64_t sizeHint)
{
  return LG_CLIPBOARD_RESULT_FAILED;
}

LG_ClipboardResult clipboardFiles_remoteDataChunk(
    const LG_ClipboardFileRequest * request, uint64_t offset,
    const void * data, size_t size)
{
  return LG_CLIPBOARD_RESULT_FAILED;
}

LG_ClipboardResult clipboardFiles_remoteDataEnd(
    const LG_ClipboardFileRequest * request, uint64_t size)
{
  return LG_CLIPBOARD_RESULT_FAILED;
}

void clipboardFiles_remoteCancel(uint64_t dataset, uint64_t request,
    LG_ClipboardFileError reason)
{
}

void clipboardFiles_localAcquire(uint64_t dataset, uint64_t acquisition) {}
void clipboardFiles_localRelease(uint64_t dataset, uint64_t acquisition) {}
void clipboardFiles_localRequest(const LG_ClipboardFileRequest * request) {}

void clipboardFiles_localCancel(uint64_t dataset, uint64_t request,
    LG_ClipboardFileError reason)
{
}

void clipboardFiles_localReady(uint64_t request) {}
