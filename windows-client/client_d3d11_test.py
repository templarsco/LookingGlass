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
and checks what the renderer does by itself, with the frames of the client's test
transport:

  warp      it makes a device on WARP, Windows' software rasterizer, which needs
            no GPU and no OpenGL driver, a flip model swap chain for the
            window, and draws the test frame in it: every pixel of the capture
            that the client takes of its window is the one that was generated
  auto      the same with the adapter that the renderer picks, which is the GPU
            when there is one that works and WARP when there is not
  hardware  the same on the GPU, which a PC without one refuses with a message
            (the test passes), unless --require-hardware says it must have one
  larger    the window is made several sizes while frames arrive, the last
            larger than the frame and of another shape: the swap chain follows
            each, and the frame is where the core says the screen is, made
            larger, with the bars around it black
  smaller   the same with the last size smaller than the frame, which the frame
            is made to fit
  refused   an adapter that is not one is refused before the client starts

The frames are checked by the colour that client/transports/Test/test.c
generates at each pixel. Where the frame is not drawn pixel for pixel, as it is
not in a window of another size, the colours that are checked are those of
points where the colour is the same around them, so that the way that a frame is
made larger or smaller does not matter, and where it goes does.

The window does not take the focus and is up for a few seconds. It needs
Windows, and does not run elsewhere."""

import argparse
import ctypes
import os
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from client_smoke_test import (CAPTURE_BOTTOM_UP, CAPTURE_MAGIC,  # noqa: E402
    CAPTURE_RGBA8, CAPTURE_VERSION, FRAME_TYPE_BGRA, HEADER, HEIGHT, WIDTH,
    check_capture, expected_color)

WINDOW_CLASS = 'LookingGlassClient'

SWP_NOZORDER     = 0x0004
SWP_NOACTIVATE   = 0x0010

# the frame that is captured is the last one that the test transport makes
FRAMES = 20

# The sizes that the window is made, in pixels. The last is not the frame's
# shape (4:3 and 5:4, and the frame is 8:5), so the frame has bars around it, and
# it is larger than the frame in one case and smaller than it in the other.
LARGER  = [(400, 300), (200, 120), (640, 360), (180, 100), (320, 240)]
SMALLER = [(400, 300), (200, 120), (640, 360), (320, 240), (200, 160)]

REFUSAL = 'The adapter must be auto, hardware or warp'

# what the renderer logs when something it needs fails
FAILURES = ('Could not', 'Present failed', 'no back buffer',
            'no window to draw in', 'is not available', 'Retrying',
            'Unsupported frame type')

# how far a pixel of a frame that was made larger or smaller may be from the
# colour that was generated, which a gradient and the way that the frame is made
# larger or smaller leave a few steps of
TOLERANCE = 6


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


def read_capture(path, width, height, serial):
  """The header of the capture checked, and its pixels as a function of the
  position in the window, from the top. Returns (errors, function)."""
  data = Path(path).read_bytes()
  if len(data) < HEADER.size:
    return ['the capture is shorter than its header'], None

  (magic, version, headerSize, frameSerial, sourceType, captureFormat,
   got_width, got_height, stride, flags, dataSize) = HEADER.unpack_from(data)

  errors = []
  def expect(name, value, wanted):
    if value != wanted:
      errors.append(f'{name} is {value}, expected {wanted}')

  expect('magic', magic, CAPTURE_MAGIC)
  expect('version', version, CAPTURE_VERSION)
  expect('headerSize', headerSize, HEADER.size)
  expect('frameSerial', frameSerial, serial)
  expect('sourceType', sourceType, FRAME_TYPE_BGRA)
  expect('captureFormat', captureFormat, CAPTURE_RGBA8)
  expect('width', got_width, width)
  expect('height', got_height, height)
  expect('stride', stride, width * 4)
  expect('dataSize', dataSize, len(data) - HEADER.size)
  if errors:
    return errors, None

  pixels    = data[HEADER.size:]
  bottom_up = flags & CAPTURE_BOTTOM_UP

  def pixel(x, y):
    offset = ((height - 1 - y if bottom_up else y) * width + x) * 4
    return tuple(pixels[offset:offset + 3])

  return [], pixel


def screen_rect(width, height):
  """Where the core puts the frame in a window, as core_updatePositionInfo does:
  as large as fits, in the middle."""
  if height / width < HEIGHT / WIDTH:
    w, h = int(height * WIDTH / HEIGHT), height
    return (width >> 1) - (w >> 1), 0, w, h
  w, h = width, int(width * HEIGHT / WIDTH)
  return 0, (height >> 1) - (h >> 1), w, h


def check_placed(path, width, height, serial):
  """The frame is where the core says the screen is in the window of this size,
  made larger or smaller to fit it, and everywhere else the window is black."""
  errors, pixel = read_capture(path, width, height, serial)
  if errors:
    return errors

  x0, y0, w, h = screen_rect(width, height)
  margin       = 2   # the edge of the frame is not checked to the pixel

  # nothing but black around the frame
  stray = [(x, y) for y in range(height) for x in range(width)
           if not (x0 - margin <= x < x0 + w + margin and
                   y0 - margin <= y < y0 + h + margin) and pixel(x, y) != (0, 0, 0)]
  if stray:
    errors.append(f'{len(stray)} pixels outside the frame, such as {stray[0]} '
        f'{pixel(*stray[0])}, are not black ({x0},{y0} {w}x{h} is the frame)')

  # the frame, where its colour is the same for a pixel or two around the point
  # that the pixel is the middle of, whichever way the frame was made to fit
  checked, wrong = 0, []
  for y in range(y0 + 3, y0 + h - 3, 3):
    for x in range(x0 + 3, x0 + w - 3, 3):
      sx = (x + 0.5 - x0) * WIDTH  / w
      sy = (y + 0.5 - y0) * HEIGHT / h
      around = [expected_color(int(sx + dx), int(sy + dy), WIDTH, HEIGHT, serial)
                for dx in (-1.5, 0, 1.5) for dy in (-1.5, 0, 1.5)
                if 0 <= sx + dx < WIDTH and 0 <= sy + dy < HEIGHT]
      if max(max(abs(a - b) for a, b in zip(color, around[0]))
             for color in around) > TOLERANCE:
        continue

      wanted = expected_color(int(sx), int(sy), WIDTH, HEIGHT, serial)
      got    = pixel(x, y)
      checked += 1
      if max(abs(a - b) for a, b in zip(got, wanted)) > TOLERANCE:
        wrong.append(((x, y), got, wanted))

  if checked < 100:
    errors.append(f'only {checked} points of the frame could be checked')
  if wrong:
    errors.append(f'{len(wrong)} of {checked} points of the frame are not the '
        f'colour that was generated, such as {wrong[0][0]}, which is '
        f'{wrong[0][1]} and not {wrong[0][2]}')
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


def common_errors(status, text):
  errors = []
  if status != 0:
    errors.append('the client timed out' if status is None else
        f'the client exited with status {status}')
  for wanted in ('Using Renderer: D3D11', 'Swap chain'):
    if wanted not in text:
      errors.append(f'the client did not log "{wanted}"')
  return errors + [f'the client logged "{failure}"' for failure in FAILURES
                   if failure in text]


def test_draw(args, output, adapter):
  """The renderer starts, draws and ends on an adapter. Returns (errors, note)."""
  capture = output / f'{adapter}.lgcapture'
  capture.unlink(missing_ok=True)

  status, size, text = run_client(args, output, adapter,
      client_command(args, adapter, capture, frames=FRAMES, rate=20))

  if adapter == 'hardware' and status not in (0, None) and \
      'No Direct3D 11 device on a GPU' in text and not args.require_hardware:
    return [], 'this PC has no GPU that Direct3D 11 can use, which it said'

  errors = common_errors(status, text)
  if adapter == 'warp' and ', software' not in text:
    errors.append('the adapter that was logged is not a software one')

  if not capture.exists():
    errors.append('the client did not write a capture')
  elif size == (WIDTH, HEIGHT):
    # the frame is drawn pixel for pixel, so every pixel is checked
    errors += check_capture(capture, WIDTH, HEIGHT, FRAMES)
  elif size is not None:
    # a display that scales its windows makes the frame larger
    errors += check_placed(capture, *size, FRAMES)

  line = next((l for l in text.splitlines() if 'Adapter' in l), 'no adapter')
  return errors, line.split('|')[-1].strip()


def test_resize(args, output, name, sizes):
  capture = output / f'{name}.lgcapture'
  capture.unlink(missing_ok=True)

  status, size, text = run_client(args, output, name,
      client_command(args, 'warp', capture, frames=4 * FRAMES, rate=20),
      resizes=sizes)

  errors = common_errors(status, text)
  if size != sizes[-1]:
    errors.append(f'the window is {size} at the end, and was made {sizes[-1]}')

  if not capture.exists():
    errors.append('the client did not write a capture')
  elif size is not None:
    errors += check_placed(capture, *size, 4 * FRAMES)
  return errors, f'{len(sizes)} sizes, the last {sizes[-1][0]}x{sizes[-1][1]}'


def test_refused(args, output):
  capture = output / 'refused.lgcapture'
  capture.unlink(missing_ok=True)

  status, size, text = run_client(args, output, 'refused',
      client_command(args, 'bogus', capture, frames=FRAMES, rate=20))

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
      ('larger'  , lambda: test_resize(args, output, 'larger', LARGER)),
      ('smaller' , lambda: test_resize(args, output, 'smaller', SMALLER)),
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

  print('pass: the Direct3D 11 renderer drew the frame, resized and refused as '
        'it should')
  return 0


if __name__ == '__main__':
  sys.exit(main())
