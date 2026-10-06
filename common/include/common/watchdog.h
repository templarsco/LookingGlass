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

#ifndef _H_LG_COMMON_WATCHDOG_
#define _H_LG_COMMON_WATCHDOG_

#include <stdbool.h>

// the exit code of a process that the watchdog ended
#define LG_WATCHDOG_EXIT_CODE 3

/* Ends the process if it is still running ms milliseconds from now, with the
 * reason in the log, and does not wait for any thread to return. It is for a
 * shutdown that can wait for ever on a thread that is stuck in the graphics
 * driver, as after a reset of the GPU that did not finish, which would leave
 * the window of a program that is closed open on the desktop. Arming it a
 * second time does nothing. Returns false if the watchdog could not start. */
bool lgWatchdogArm(unsigned ms, const char * reason);

#endif
