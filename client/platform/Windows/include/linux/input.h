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

#ifndef _H_LG_WINDOWS_LINUX_INPUT_
#define _H_LG_WINDOWS_LINUX_INPUT_

// The client uses the Linux evdev key and button codes as its key space.
// Windows only needs their definitions, not the evdev structures.

// winuser.h defines SW_MAX for ShowWindow, keep that one in either order
#pragma push_macro("SW_MAX")
#undef SW_MAX
#include "linux/input-event-codes.h"
#undef SW_MAX
#pragma pop_macro("SW_MAX")

#endif
