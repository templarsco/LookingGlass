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

/* Where the Win32 window goes when the position that is configured for it is
 * on a monitor that is no longer there. */

#include "test.h"

#include "../displayservers/Win32/placement.h"

// a window of the given size at x, y
static Win32Rect at(int x, int y, int w, int h)
{
  return (Win32Rect) { x, y, x + w, y + h };
}

static bool same(Win32Rect a, Win32Rect b)
{
  return a.left == b.left && a.top == b.top &&
    a.right == b.right && a.bottom == b.bottom;
}

// a 1920x1080 primary at the origin, with the taskbar's 48 pixels taken off
static const Win32Rect primary = { 0, 0, 1920, 1032 };

// a second one to its right, and another to the left of the primary, as a
// setup may have it, with a height that is not the same
static const Win32Rect right = { 1920, 0, 1920 + 2560, 1440 - 48 };
static const Win32Rect left  = { -1280, 100, 0, 100 + 1024 - 48 };

static void testStaysWhereItIs(void)
{
  Win32Rect work[] = { primary };
  Win32Rect win    = at(100, 100, 800, 600);
  CHECK(!win32Placement_fit(&win, work, 1));
  CHECK(same(win, at(100, 100, 800, 600)));

  // the corner of the area is as good as any place
  win = at(0, 0, 1920, 1032);
  CHECK(!win32Placement_fit(&win, work, 1));

  // hanging over an edge, with a title bar and a place to click still there
  win = at(1820, 900, 800, 600);
  CHECK(!win32Placement_fit(&win, work, 1));
  win = at(-700, -100, 800, 600);
  CHECK(!win32Placement_fit(&win, work, 1));
}

static void testSpansTwoMonitors(void)
{
  Win32Rect work[] = { primary, right };
  Win32Rect win    = at(1500, 100, 800, 600);
  CHECK(!win32Placement_fit(&win, work, 2));
  CHECK(same(win, at(1500, 100, 800, 600)));
}

static void testOnTheSecondMonitor(void)
{
  Win32Rect work[] = { primary, right };
  Win32Rect win    = at(2500, 200, 1280, 720);
  CHECK(!win32Placement_fit(&win, work, 2));
  CHECK(same(win, at(2500, 200, 1280, 720)));
}

static void testMonitorGone(void)
{
  // the position was on a monitor to the right, which is unplugged
  Win32Rect work[] = { primary };
  Win32Rect win    = at(2500, 200, 1280, 720);
  CHECK(win32Placement_fit(&win, work, 1));
  CHECK(win.left >= 0 && win.right <= 1920);
  CHECK(win.top  >= 0 && win.bottom <= 1032);
  // the size is what it was
  CHECK(win.right - win.left == 1280);
  CHECK(win.bottom - win.top == 720);
  // and it is the nearest place to where it was: against the right edge
  CHECK(same(win, at(1920 - 1280, 200, 1280, 720)));

  // far above and to the left
  win = at(-30000, -30000, 800, 600);
  CHECK(win32Placement_fit(&win, work, 1));
  CHECK(same(win, at(0, 0, 800, 600)));

  // far below and to the right
  win = at(30000, 30000, 800, 600);
  CHECK(win32Placement_fit(&win, work, 1));
  CHECK(same(win, at(1920 - 800, 1032 - 600, 800, 600)));

  // too little of it shows to grab: 40 pixels of its width
  win = at(1880, 100, 800, 600);
  CHECK(win32Placement_fit(&win, work, 1));
  CHECK(same(win, at(1920 - 800, 100, 800, 600)));
  // and too little of its height
  win = at(100, 1000, 800, 600);
  CHECK(win32Placement_fit(&win, work, 1));
  CHECK(same(win, at(100, 1032 - 600, 800, 600)));
}

static void testNearestOfSeveral(void)
{
  // on no monitor, nearest the one on the left
  Win32Rect work[] = { primary, left };
  Win32Rect win    = at(-2500, 300, 800, 600);
  CHECK(win32Placement_fit(&win, work, 2));
  CHECK(same(win, at(-1280, 300, 800, 600)));

  // and nearest the one on the right, with the primary in the list first
  Win32Rect work2[] = { primary, right, left };
  win = at(9000, 300, 800, 600);
  CHECK(win32Placement_fit(&win, work2, 3));
  CHECK(same(win, at(1920 + 2560 - 800, 300, 800, 600)));
}

static void testSmallAndLargeWindows(void)
{
  Win32Rect work[] = { primary };

  // a window smaller than what has to show is on the monitor if all of it is
  Win32Rect win = at(100, 100, 60, 30);
  CHECK(!win32Placement_fit(&win, work, 1));
  // but not when 20 of its 60 pixels are
  win = at(1900, 100, 60, 30);
  CHECK(win32Placement_fit(&win, work, 1));
  CHECK(same(win, at(1920 - 60, 100, 60, 30)));

  // a window larger than the area starts at its corner
  win = at(5000, 5000, 3000, 2000);
  CHECK(win32Placement_fit(&win, work, 1));
  CHECK(win.left == 0 && win.top == 0);
  CHECK(win.right - win.left == 3000 && win.bottom - win.top == 2000);
}

static void testNothingToFitTo(void)
{
  Win32Rect win = at(9000, 9000, 800, 600);
  CHECK(!win32Placement_fit(&win, NULL, 0));
  CHECK(same(win, at(9000, 9000, 800, 600)));
}

int main(void)
{
  testStaysWhereItIs();
  testSpansTwoMonitors();
  testOnTheSecondMonitor();
  testMonitorGone();
  testNearestOfSeveral();
  testSmallAndLargeWindows();
  testNothingToFitTo();
  puts("placement tests passed");
  return EXIT_SUCCESS;
}
