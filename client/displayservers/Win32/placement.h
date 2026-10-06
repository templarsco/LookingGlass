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

#ifndef _H_LG_CLIENT_DISPLAYSERVER_WIN32_PLACEMENT_
#define _H_LG_CLIENT_DISPLAYSERVER_WIN32_PLACEMENT_

/* Where the window goes that a configuration put at a position. A position
 * that was on a second monitor is on no monitor when that one is unplugged,
 * or a laptop is undocked, and the window is then on no screen at all: it has
 * no title bar or taskbar button to take it back with that anyone can reach.
 * Nothing here is specific to Windows, so that the tests can run on it. */

#include <stdbool.h>
#include <stddef.h>

typedef struct Win32Rect
{
  int left, top, right, bottom;
}
Win32Rect;

/* how much of a window has to be on a monitor to count as being on it: a
 * title bar to grab, and a place to click, or all of the window if that is
 * less */
#define WIN32_PLACEMENT_MIN_WIDTH  100
#define WIN32_PLACEMENT_MIN_HEIGHT  50

static inline int win32Placement_overlap(int lo1, int hi1, int lo2, int hi2)
{
  const int lo = lo1 > lo2 ? lo1 : lo2;
  const int hi = hi1 < hi2 ? hi1 : hi2;
  return hi > lo ? hi - lo : 0;
}

static inline int win32Placement_clamp(int value, int lo, int hi)
{
  // a window larger than the area goes to its start
  if (hi < lo)
    return lo;
  return value < lo ? lo : value > hi ? hi : value;
}

/* Moves the window, whose rectangle is the one to check, onto the work area
 * (the monitor without the taskbar) that is nearest to it, if too little of
 * it is on any of them. Returns true if it moved the window. A window that
 * shows enough on one work area is left where it is, as it may span several,
 * and so is one when there is no work area to put it on. */
static inline bool win32Placement_fit(Win32Rect * window,
    const Win32Rect * work, size_t count)
{
  if (!count)
    return false;

  const int width  = window->right  - window->left;
  const int height = window->bottom - window->top;
  const int needW  = width  < WIN32_PLACEMENT_MIN_WIDTH  ?
    width  : WIN32_PLACEMENT_MIN_WIDTH;
  const int needH  = height < WIN32_PLACEMENT_MIN_HEIGHT ?
    height : WIN32_PLACEMENT_MIN_HEIGHT;

  size_t        nearest  = 0;
  long long     distance = -1;
  for (size_t i = 0; i < count; ++i)
  {
    if (win32Placement_overlap(window->left, window->right,
          work[i].left, work[i].right) >= needW &&
        win32Placement_overlap(window->top, window->bottom,
          work[i].top, work[i].bottom) >= needH)
      return false;

    // the squared distance between the centres, which orders them the same
    const long long dx = ((long long)window->left + window->right -
        work[i].left - work[i].right) / 2;
    const long long dy = ((long long)window->top + window->bottom -
        work[i].top - work[i].bottom) / 2;
    const long long d = dx * dx + dy * dy;
    if (distance < 0 || d < distance)
    {
      distance = d;
      nearest  = i;
    }
  }

  const Win32Rect * area = &work[nearest];
  const int left = win32Placement_clamp(window->left, area->left,
      area->right - width);
  const int top  = win32Placement_clamp(window->top, area->top,
      area->bottom - height);

  window->left   = left;
  window->top    = top;
  window->right  = left + width;
  window->bottom = top + height;
  return true;
}

#endif
