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

"""Runs the Windows client with the Direct3D 11 renderer (app:renderer=D3D11)
and checks what the renderer does by itself:

  warp      it makes a device on WARP, Windows' software rasterizer, which needs
            no GPU and no OpenGL driver, a flip model swap chain for the
            window, and composes it: the capture is as large as the window and
            all of one colour (the frames are not drawn yet)
  auto      the same with the adapter that the renderer picks, which is the GPU
            when there is one that works and WARP when there is not
  hardware  the same on the GPU, which a PC without one refuses with a message
            (the test passes), unless --require-hardware says it must have one
  resize    the window is made several sizes while frames arrive, and the
            swap chain follows each: the capture is as large as the window is
            at the end, and the client neither fails nor stops answering
  refused   an adapter that is not one is refused before the client starts

The window does not take the focus and is up for a few seconds. It needs
Windows, and does not run elsewhere."""

import argparse
import collections
import ctypes
import os
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from client_smoke_test import (CAPTURE_MAGIC, CAPTURE_RGBA8,  # noqa: E402
    CAPTURE_VERSION, FRAME_TYPE_BGRA, HEADER)

WINDOW_CLASS = 'LookingGlassClient'

WIDTH, HEIGHT = 256, 160

# what the renderer clears the window to, 0.03 0.03 0.05 of 255, as the
# swap chain's 8 bits keep it
CLEAR = (8, 8, 13, 255)

SWP_NOZORDER     = 0x0004
SWP_NOACTIVATE   = 0x0010

# the sizes that the window is made, in pixels
RESIZES = [(400, 300), (200, 120), (640, 360), (180, 100), (320, 200)]

REFUSAL = 'The adapter must be auto, hardware or warp'

# what the renderer logs when something it needs fails
FAILURES = ('Could not', 'Present failed', 'no back buffer',
            'no window to draw in', 'is not available')


class RECT(ctypes.Structure):
  _fields_ = [('left', wintypes.LONG), ('top', wintypes.LONG),
              ('right', wintypes.LONG), ('bottom', wintypes.LONG)]


def load_api():
  user32 = ctypes.WinDLL('user32', use_last_error=True)
  window_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND,
      wintypes.LPARAM)

  user32.EnumWindows.argtypes = [window_proc, wintypes.LPARAM]
  user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND,
      ctypes.POINTER(wintypes.DWORD)]
  user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
  user32.IsWindowVisible.argtypes = [wintypes.HWND]
  user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
  user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
  user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
      ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
  return user32, window_proc


def find_window(api, pid):
  user32, window_proc = api
  found = []

  def callback(hwnd, param):
    owner = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
    if owner.value == pid and user32.IsWindowVisible(hwnd):
      name = ctypes.create_unicode_buffer(256)
      user32.GetClassNameW(hwnd, name, len(name))
      if name.value == WINDOW_CLASS:
        found.append(hwnd)
    return True

  user32.EnumWindows(window_proc(callback), 0)
  return found[0] if found else None


def client_size(api, hwnd):
  rect = RECT()
  api[0].GetClientRect(hwnd, ctypes.byref(rect))
  return rect.right, rect.bottom


def client_command(args, adapter, capture, frames, rate, extra=()):
  return [args.client,
    'app:transport=test',
    f'test:width={WIDTH}', f'test:height={HEIGHT}', 'test:format=bgra',
    'test:damage=full', f'test:frameRate={rate}', f'test:frameCount={frames}',
    'test:holdLastFrame=yes', 'test:realtime=yes',
    'app:renderer=D3D11', f'd3d11:adapter={adapter}', 'd3d11:vsync=no',
    f'test:captureFile={capture}', f'test:captureFrame={frames}',
    'test:captureDelay=1',
    f'win:size={WIDTH}x{HEIGHT}', 'win:borderless=yes', 'win:autoResize=no',
    'win:allowResize=yes', 'win:quickSplash=yes', 'win:alerts=no',
    'win:noScreensaver=no',
    # the window does not take the focus from whoever is at the PC
    'win:showInactive=yes', 'input:grabKeyboard=no',
  ] + list(extra) + args.client_args


def check_capture(path, width, height):
  """The capture is as large as the window and one colour. Returns the errors."""
  data = Path(path).read_bytes()
  if len(data) < HEADER.size:
    return ['the capture is shorter than its header']

  (magic, version, headerSize, frameSerial, sourceType, captureFormat,
   got_width, got_height, stride, flags, dataSize) = HEADER.unpack_from(data)

  errors = []
  def expect(name, value, wanted):
    if value != wanted:
      errors.append(f'{name} is {value}, expected {wanted}')

  expect('magic', magic, CAPTURE_MAGIC)
  expect('version', version, CAPTURE_VERSION)
  expect('headerSize', headerSize, HEADER.size)
  expect('sourceType', sourceType, FRAME_TYPE_BGRA)
  expect('captureFormat', captureFormat, CAPTURE_RGBA8)
  expect('width', got_width, width)
  expect('height', got_height, height)
  expect('stride', stride, width * 4)
  expect('dataSize', dataSize, len(data) - HEADER.size)
  if errors:
    return errors

  pixels = data[HEADER.size:]
  colors = collections.Counter(zip(pixels[0::4], pixels[1::4], pixels[2::4],
      pixels[3::4]))
  off = [(color, count) for color, count in colors.items()
         if max(abs(a - b) for a, b in zip(color, CLEAR)) > 1]
  if off:
    errors.append(f'{sum(count for _, count in off)} of {width * height} '
        f'pixels are not {CLEAR}, such as {off[0][0]}')
  return errors


def run_client(args, output, name, command, resizes=()):
  """Runs the client to the end. Returns (status, size of the window at the end,
  the log), where the status is None if it did not end."""
  log = output / f'{name}.log'
  env = dict(os.environ, APPDATA=str(output), LOCALAPPDATA=str(output))
  api = load_api()

  with open(log, 'wb') as stream:
    client = subprocess.Popen(command, stdout=stream,
        stderr=subprocess.STDOUT, env=env)

  size = None
  try:
    deadline = time.monotonic() + args.timeout
    hwnd = None
    while hwnd is None and time.monotonic() < deadline:
      if client.poll() is not None:
        break
      hwnd = find_window(api, client.pid)
      time.sleep(0.05)

    if hwnd is not None:
      time.sleep(0.8)
      for width, height in resizes:
        rect = RECT()
        api[0].GetWindowRect(hwnd, ctypes.byref(rect))
        api[0].SetWindowPos(hwnd, None, rect.left, rect.top, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE)
        time.sleep(0.25)
      size = client_size(api, hwnd)

    try:
      status = client.wait(timeout=max(1, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
      status = None
  finally:
    if client.poll() is None:
      client.kill()
      client.wait()

  return status, size, log.read_text(errors='replace')


def test_draw(args, output, adapter):
  """The renderer starts, draws and ends on an adapter. Returns (errors, note)."""
  capture = output / f'{adapter}.lgcapture'
  capture.unlink(missing_ok=True)

  status, size, text = run_client(args, output, adapter,
      client_command(args, adapter, capture, frames=20, rate=20))

  if adapter == 'hardware' and status not in (0, None) and \
      'No Direct3D 11 device on a GPU' in text and not args.require_hardware:
    return [], 'this PC has no GPU that Direct3D 11 can use, which it said'

  errors = []
  if status != 0:
    errors.append('the client timed out' if status is None else
        f'the client exited with status {status}')
  for wanted in ('Using Renderer: D3D11', 'Swap chain'):
    if wanted not in text:
      errors.append(f'the client did not log "{wanted}"')
  if adapter == 'warp' and ', software' not in text:
    errors.append('the adapter that was logged is not a software one')
  errors += [f'the client logged "{failure}"' for failure in FAILURES
             if failure in text]

  if not capture.exists():
    errors.append('the client did not write a capture')
  elif size is not None:
    errors += check_capture(capture, *size)

  line = next((l for l in text.splitlines() if 'Adapter' in l), 'no adapter')
  return errors, line.split('|')[-1].strip()


def test_resize(args, output):
  capture = output / 'resize.lgcapture'
  capture.unlink(missing_ok=True)

  status, size, text = run_client(args, output, 'resize',
      client_command(args, 'warp', capture, frames=80, rate=20),
      resizes=RESIZES)

  errors = []
  if status != 0:
    errors.append('the client timed out' if status is None else
        f'the client exited with status {status}')
  if size != RESIZES[-1]:
    errors.append(f'the window is {size} at the end, and was made '
        f'{RESIZES[-1]}')
  errors += [f'the client logged "{failure}"' for failure in FAILURES
             if failure in text]

  if not capture.exists():
    errors.append('the client did not write a capture')
  elif size is not None:
    errors += check_capture(capture, *size)
  return errors, f'{len(RESIZES)} sizes, the last {RESIZES[-1][0]}x' \
    f'{RESIZES[-1][1]}'


def test_refused(args, output):
  capture = output / 'refused.lgcapture'
  capture.unlink(missing_ok=True)

  status, size, text = run_client(args, output, 'refused',
      client_command(args, 'bogus', capture, frames=20, rate=20))

  errors = []
  if status in (0, None):
    errors.append('the client started with an adapter that is not one')
  if REFUSAL not in text:
    errors.append(f'the client did not say "{REFUSAL}"')
  if size is not None or capture.exists():
    errors.append('the client opened a window')
  return errors, 'refused before it started'


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--require-hardware', action='store_true',
      help='fail if the renderer cannot make a device on a GPU')
  parser.add_argument('--output', help='keep the captures and logs here')
  parser.add_argument('--timeout', type=float, default=60)
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after --')
  args = parser.parse_args()

  if sys.platform != 'win32':
    print('skipped: this test needs Windows')
    return 0

  output = Path(args.output or tempfile.mkdtemp(prefix='lg-win-d3d11-'))
  output.mkdir(parents=True, exist_ok=True)

  failed = False
  for name, test in (
      ('warp'    , lambda: test_draw(args, output, 'warp')),
      ('auto'    , lambda: test_draw(args, output, 'auto')),
      ('hardware', lambda: test_draw(args, output, 'hardware')),
      ('resize'  , lambda: test_resize(args, output)),
      ('refused' , lambda: test_refused(args, output))):
    errors, note = test()
    if errors:
      failed = True
      print(f'FAIL {name}: ' + '; '.join(errors))
      print(f'     the log is {output / (name + ".log")}')
    else:
      print(f'pass {name}: {note}')

  if failed:
    return 1

  print('pass: the Direct3D 11 renderer drew, resized and refused as it should')
  return 0


if __name__ == '__main__':
  sys.exit(main())
