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

#ifndef _H_LG_WIN32_KEYMAP_
#define _H_LG_WIN32_KEYMAP_

/* Converts between Windows keyboard scan codes and the Linux KEY_* codes
 * the client uses. Windows scan codes are PS/2 set 1 make codes plus the E0
 * prefix flag, so this inverts linux_to_ps2. The code has no Windows
 * dependencies so that the tests can run on Linux. */

#include <stdbool.h>

// the virtual key codes that need special handling, from winuser.h
#define WIN32_KEYMAP_VK_PAUSE   0x13
#define WIN32_KEYMAP_VK_NUMLOCK 0x90

void win32KeymapInit(void);

/* Returns the Linux key for a scan code as WM_KEYDOWN or a low level
 * keyboard hook reports it, or 0 when there is none. vk is the virtual key
 * of the same event. */
int win32KeymapToLinux(unsigned int vk, unsigned int scanCode, bool extended);

/* Returns the scan code of a Linux key, false when it has none. */
bool win32KeymapFromLinux(int key, unsigned int * scanCode, bool * extended);

#endif
