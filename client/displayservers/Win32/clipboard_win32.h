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

#ifndef _H_LG_CLIENT_DISPLAYSERVER_WIN32_CLIPBOARD_WIN32_
#define _H_LG_CLIENT_DISPLAYSERVER_WIN32_CLIPBOARD_WIN32_

/* The clipboard of the Windows display server, with Windows under it: the
 * hooks that the display server hands the core are these. */

#include "clipboard.h"
#include "interface/clipboard.h"

#include <stdbool.h>

/* Starts the thread that has the window that Windows sends the clipboard's
 * messages to, and offers what is on the clipboard to the core. False if the
 * clipboard cannot be used, and the client goes on without it. */
bool win32CBStart(const Win32ClipboardCore * core);

/* Stops the thread. What was offered to Windows and not filled in is
 * filled in first, if the guest is there to ask. */
void win32CBStop(void);

// the same, with the clipboard of the client for the core: what the display
// server calls
bool win32CBInit(void);
void win32CBFree(void);

void win32CBNotice(LG_ClipboardData type);
void win32CBRelease(void);
void win32CBRequest(LG_ClipboardRequest request, LG_ClipboardData type);
void win32CBRequestReady(LG_ClipboardRequest request);
void win32CBRequestCancel(LG_ClipboardRequest request,
    LG_ClipboardCancelReason reason);

#endif
