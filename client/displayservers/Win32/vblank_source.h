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

#ifndef _H_LG_CLIENT_DISPLAYSERVER_WIN32_VBLANK_SOURCE_
#define _H_LG_CLIENT_DISPLAYSERVER_WIN32_VBLANK_SOURCE_

/* The sources of vertical blanks that Windows has, for vblank.h.
 *
 * The graphics kernel's own wait is for the blank of one display, so a window
 * on a 144 Hz display is paced at 144 Hz when another display runs at 240 Hz.
 * DwmFlush is the fallback for a session that has no such wait. DWM composes
 * the whole desktop at the fastest display's rate, so it is only right for the
 * window when that is the display it is on. */

#include <stdbool.h>
#include <windows.h>

typedef struct Win32KmtSource Win32KmtSource;

/* device is the name of the display, such as \\.\DISPLAY1, which a
 * MONITORINFOEX holds. */
Win32KmtSource * win32KmtSource_create(const WCHAR * device);
void             win32KmtSource_destroy(Win32KmtSource * source);

/* The window went to another display: the next wait is for its blank */
void win32KmtSource_retarget(Win32KmtSource * source, const WCHAR * device);

/* Win32VBlankSource wait functions */
bool win32KmtSource_wait(void * source);
bool win32DwmSource_wait(void * unused);

#endif
