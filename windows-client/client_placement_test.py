#!/usr/bin/env python3
#
# Looking Glass
# Copyright © 2017-2026 The Looking Glass Authors
# https://looking-glass.io
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc., 59
# Temple Place, Suite 330, Boston, MA 02111-1307 USA

"""Starts the Windows client with positions for its window and checks where
the window is, as the desktop has it: a position that was saved for a monitor
that is not there any more must not leave the window on no screen, and a
position that is on one must be where it was asked to be.

  center     the window is on a monitor, whole
  far        a position far off to the right and below, and one far off to the
             left and above, which are on no monitor of any PC: the window is
             on a monitor
  origin     the position 0,0 of the desktop, which is on a monitor: the window
             is where it was asked to be
  edge       a position that leaves 150 pixels of the window on the last
             monitor to the right: it stays

The window does not take the focus (win:showInactive) and is up for a second
or so for each of the cases. The test uses the desktop that it runs on, and
reads the monitors' work areas from it, so a PC with one monitor and one with
five give the same answer. It needs Windows, and does not run elsewhere."""

import argparse
import ctypes
import os
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes
from pathlib import Path

# the least of a window that is a window that can be reached: client/displayservers
# /Win32/placement.h has the same
MIN_WIDTH  = 100
MIN_HEIGHT = 50

WINDOW_CLASS = 'LookingGlassClient'


class RECT(ctypes.Structure):
  _fields_ = [('left', wintypes.LONG), ('top', wintypes.LONG),
              ('right', wintypes.LONG), ('bottom', wintypes.LONG)]


class MONITORINFO(ctypes.Structure):
  _fields_ = [('cbSize', wintypes.DWORD), ('rcMonitor', RECT),
              ('rcWork', RECT), ('dwFlags', wintypes.DWORD)]


def load_user32():
  user32 = ctypes.WinDLL('user32', use_last_error=True)
  # without this the desktop is as a program that does not know of DPI sees
  # it, and the sizes are not the ones that the client has
  user32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
  user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))

  window_proc  = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
  monitor_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HANDLE,
      wintypes.HDC, ctypes.POINTER(RECT), wintypes.LPARAM)

  user32.EnumWindows.argtypes = [window_proc, wintypes.LPARAM]
  user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND,
      ctypes.POINTER(wintypes.DWORD)]
  user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
  user32.IsWindowVisible.argtypes = [wintypes.HWND]
  user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
  user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
  user32.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
  user32.EnumDisplayMonitors.argtypes = [wintypes.HDC, ctypes.POINTER(RECT),
      monitor_proc, wintypes.LPARAM]
  user32.GetMonitorInfoW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MONITORINFO)]
  user32.GetMonitorInfoW.restype = wintypes.BOOL
  return user32, window_proc, monitor_proc


def work_areas(user32, monitor_proc):
  areas = []

  def callback(monitor, dc, rect, param):
    info = MONITORINFO(cbSize=ctypes.sizeof(MONITORINFO))
    if user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
      work = info.rcWork
      areas.append((work.left, work.top, work.right, work.bottom))
    return True

  user32.EnumDisplayMonitors(None, None, monitor_proc(callback), 0)
  return areas


def find_window(user32, window_proc, pid):
  found = []

  def callback(hwnd, param):
    owner = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
    if owner.value != pid or not user32.IsWindowVisible(hwnd):
      return True

    name = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, name, len(name))
    if name.value == WINDOW_CLASS:
      found.append(hwnd)
    return True

  user32.EnumWindows(window_proc(callback), 0)
  return found[0] if found else None


def overlap(lo1, hi1, lo2, hi2):
  return max(0, min(hi1, hi2) - max(lo1, lo2))


def visible_on_a_monitor(window, areas):
  """The window's rectangle shows at least what can be reached on some work
  area, or all of it if that is less."""
  left, top, right, bottom = window
  need_w = min(MIN_WIDTH, right - left)
  need_h = min(MIN_HEIGHT, bottom - top)
  return any(overlap(left, right, a[0], a[2]) >= need_w and
             overlap(top, bottom, a[1], a[3]) >= need_h for a in areas)


def run_case(args, user32, window_proc, areas, name, position, env, output):
  size    = (400, 300)
  command = [args.client,
    'app:renderer=OpenGL',
    f'win:size={size[0]}x{size[1]}',
    f'win:position={position}',
    'win:autoResize=no',
    'win:allowResize=no',
    'win:quickSplash=yes',
    'win:alerts=no',
    'win:noScreensaver=no',
    'win:showInactive=yes',
    'input:grabKeyboard=no',
    'opengl:vsync=no',
  ] + args.client_args

  log = output / f'{name}.log'
  with open(log, 'wb') as stream:
    client = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT,
        env=env)

  try:
    hwnd     = None
    deadline = time.monotonic() + args.timeout
    while hwnd is None and time.monotonic() < deadline:
      if client.poll() is not None:
        return None, f'the client exited with status {client.returncode}'
      hwnd = find_window(user32, window_proc, client.pid)
      if hwnd is None:
        time.sleep(0.05)
    if hwnd is None:
      return None, 'the client did not show a window'

    # the window may still be moved by what the client does when it is up
    time.sleep(0.5)
    rect = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    origin = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(origin))
    return ((rect.left, rect.top, rect.right, rect.bottom),
            (origin.x, origin.y)), None
  finally:
    client.kill()
    client.wait()


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--timeout', type=float, default=20,
      help='seconds to wait for each window')
  parser.add_argument('--output', help='keep the logs here')
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after --')
  args = parser.parse_args()

  if sys.platform != 'win32':
    print('skipped: this test needs Windows')
    return 0

  user32, window_proc, monitor_proc = load_user32()
  areas = work_areas(user32, monitor_proc)
  if not areas:
    print('FAIL: the desktop has no monitor')
    return 1
  print('work areas: ' + ', '.join('%d,%d..%d,%d' % a for a in areas))

  output = Path(args.output or tempfile.mkdtemp(prefix='lg-win-placement-'))
  output.mkdir(parents=True, exist_ok=True)

  # no configuration of the person who runs it
  home = output / 'appdata'
  home.mkdir(exist_ok=True)
  env = dict(os.environ, APPDATA=str(home), LOCALAPPDATA=str(home))

  last_right = max(a[2] for a in areas)
  last_area  = max(areas, key=lambda a: a[2])

  # the position is where the client area starts. 'cases' are the name, the
  # position, and whether the window must be where it was asked to be
  cases = [
    ('center', 'center', False),
    ('far-right', '30000x30000', False),
    ('far-left', '-30000x-30000', False),
    ('origin', '0x0', True),
    ('edge', '%dx%d' % (last_right - 150, last_area[1] + 100), True),
  ]

  failed = 0
  for name, position, exact in cases:
    result, error = run_case(args, user32, window_proc, areas, name,
        position, env, output)
    if error:
      failed += 1
      print(f'FAIL {name} ({position}): {error}')
      continue

    window, origin = result
    problems = []
    if not visible_on_a_monitor(window, areas):
      problems.append('the window is on no monitor: %d,%d..%d,%d' % window)
    if exact:
      wanted = tuple(int(v) for v in position.split('x'))
      if origin != wanted:
        problems.append('the client area starts at %d,%d, and %d,%d was asked '
                        'for' % (origin + wanted))

    if problems:
      failed += 1
      print(f'FAIL {name} ({position}):')
      for problem in problems:
        print('  ' + problem)
    else:
      print('pass %s (%s): window %d,%d..%d,%d, client area at %d,%d' %
            ((name, position) + window + origin))

  return 1 if failed else 0


if __name__ == '__main__':
  sys.exit(main())
