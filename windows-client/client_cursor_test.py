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

"""Runs the Windows client on the cursors that its test transport serves
(test:cursor=color, masked and mono) and compares the framebuffer it composed in
its window with the test frame and the cursor on it, pixel by pixel:

  color     a 16x16 cursor of 32 bit pixels with an alpha channel: a border that
            is not there, and a quadrant each of red, green, blue and half of
            white, which are blended with what is under them
  masked    the same kind of cursor with the alpha of a masked color one: the
            left half replaces what is under it, in red, and the right half,
            which would be XORed with it, is left as it is
  mono      a cursor of two masks: black, white, what is under it, and what is
            under it inverted, in the quadrants from the top left
  edge      the color cursor with the frame's corner under it, where only a
            quarter of it is on the frame
  corner    the color cursor placed so that only a quarter of it is on the frame
            at its top left

The renderer is the client's default, OpenGL, unless the options after the
client's path choose one, as app:renderer=D3D11. The client must be built with
ENABLE_TESTS, which adds the framebuffer capture and the test transport's
cursors. The window does not take the focus. It needs Windows."""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from client_smoke_test import (CAPTURE_BOTTOM_UP, CAPTURE_MAGIC,  # noqa: E402
    CAPTURE_RGBA8, CAPTURE_VERSION, FRAME_TYPE_BGRA, HEADER, HEIGHT, WIDTH,
    expected_color)

SERIAL = 4
SIZE   = 16
TOLERANCE = 2

# kind, where the left and top of the cursor are on the frame
CASES = [
  ('color' , 'color' , 40, 24),
  ('masked', 'masked', 40, 24),
  ('mono'  , 'mono'  , 40, 24),
  ('edge'  , 'color' , WIDTH - 8, HEIGHT - 8),
  ('corner', 'color' , -8, -8),
]


def over(color, alpha, base):
  return tuple(round((c * alpha + b * (255 - alpha)) / 255)
               for c, b in zip(color, base))


def draw_color(x, y, base):
  if x in (0, SIZE - 1) or y in (0, SIZE - 1):
    return base
  if y < SIZE // 2:
    return over((255, 0, 0) if x < SIZE // 2 else (0, 255, 0), 255, base)
  return over((0, 0, 255), 255, base) if x < SIZE // 2 else \
    over((255, 255, 255), 128, base)


def draw_masked(x, y, base):
  return (255, 0, 0) if x < SIZE // 2 else base


def draw_mono(x, y, base):
  top, left = y < SIZE // 2, x < SIZE // 2
  if top:
    return (0, 0, 0) if left else (255, 255, 255)
  return base if left else tuple(255 - b for b in base)


DRAW = {'color': draw_color, 'masked': draw_masked, 'mono': draw_mono}


def expected(kind, left, top, x, y):
  base = expected_color(x, y, WIDTH, HEIGHT, SERIAL)
  cx, cy = x - left, y - top
  if 0 <= cx < SIZE and 0 <= cy < SIZE:
    return DRAW[kind](cx, cy, base)
  return base


def check_capture(path, kind, left, top):
  data = Path(path).read_bytes()
  if len(data) < HEADER.size:
    return ['the capture is shorter than its header']

  (magic, version, headerSize, frameSerial, sourceType, captureFormat,
   width, height, stride, flags, dataSize) = HEADER.unpack_from(data)

  errors = []
  def expect(name, value, wanted):
    if value != wanted:
      errors.append(f'{name} is {value}, expected {wanted}')

  expect('magic', magic, CAPTURE_MAGIC)
  expect('version', version, CAPTURE_VERSION)
  expect('headerSize', headerSize, HEADER.size)
  expect('frameSerial', frameSerial, SERIAL)
  expect('sourceType', sourceType, FRAME_TYPE_BGRA)
  expect('captureFormat', captureFormat, CAPTURE_RGBA8)
  expect('width', width, WIDTH)
  expect('height', height, HEIGHT)
  expect('stride', stride, WIDTH * 4)
  expect('dataSize', dataSize, len(data) - HEADER.size)
  if errors:
    return errors

  pixels    = data[HEADER.size:]
  bottom_up = flags & CAPTURE_BOTTOM_UP
  wrong     = []
  on_cursor = 0
  for y in range(HEIGHT):
    row = (HEIGHT - 1 - y if bottom_up else y) * stride
    for x in range(WIDTH):
      got  = tuple(pixels[row + x * 4:row + x * 4 + 3])
      want = expected(kind, left, top, x, y)
      on_cursor += 0 <= x - left < SIZE and 0 <= y - top < SIZE
      if max(abs(a - b) for a, b in zip(got, want)) > TOLERANCE:
        wrong.append(((x, y), got, want))

  if wrong:
    errors.append(f'{len(wrong)} of {WIDTH * HEIGHT} pixels are not as '
        f'expected, such as {wrong[0][0]}, which is {wrong[0][1]} and not '
        f'{wrong[0][2]}; {sum(1 for (x, y), _, _ in wrong if 0 <= x - left < SIZE and 0 <= y - top < SIZE)} '
        f'of them are in the cursor, which has {on_cursor} pixels on the frame')
  return errors


def run_case(args, output, name, cursor, left, top):
  directory = output / name
  directory.mkdir(parents=True, exist_ok=True)
  capture = directory / 'capture.lgcapture'
  log     = directory / 'client.log'
  capture.unlink(missing_ok=True)

  config = (directory / 'appdata').resolve()
  config.mkdir(exist_ok=True)
  env = dict(os.environ, APPDATA=str(config), LOCALAPPDATA=str(config))

  command = [args.client,
    'app:transport=test', 'app:renderer=OpenGL',
    f'test:width={WIDTH}', f'test:height={HEIGHT}', 'test:format=bgra',
    'test:damage=full', 'test:frameRate=60', f'test:frameCount={SERIAL}',
    'test:holdLastFrame=yes', 'test:realtime=no',
    f'test:cursor={cursor}', f'test:cursorX={left}', f'test:cursorY={top}',
    f'test:captureFile={capture}', f'test:captureFrame={SERIAL}',
    'test:captureDelay=4',
    f'win:size={WIDTH}x{HEIGHT}', 'win:borderless=yes', 'win:autoResize=no',
    'win:allowResize=no', 'win:quickSplash=yes', 'win:alerts=no',
    'win:noScreensaver=no',
    # the window does not take the focus from whoever is at the PC
    'win:showInactive=yes', 'input:grabKeyboard=no',
    'opengl:mipmap=no', 'opengl:vsync=no',
  ] + args.client_args

  try:
    with open(log, 'wb') as stream:
      status = subprocess.run(command, stdout=stream,
          stderr=subprocess.STDOUT, timeout=args.timeout, env=env).returncode
  except subprocess.TimeoutExpired:
    status = None

  text = log.read_text(errors='replace')
  errors = []
  if status is None:
    errors.append('the client timed out')
  elif status != 0:
    errors.append(f'the client exited with status {status}')
  elif not capture.exists():
    errors.append('the client did not write a capture')
  else:
    errors += check_capture(capture, cursor, left, top)
  return errors, text


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--output', help='keep the captures and logs here')
  parser.add_argument('--timeout', type=float, default=60)
  parser.add_argument('client_args', nargs='*',
      help='extra client options, as app:renderer=D3D11')
  args = parser.parse_args()

  if sys.platform != 'win32':
    print('skipped: this test needs Windows')
    return 0

  output = Path(args.output or tempfile.mkdtemp(prefix='lg-win-cursor-'))
  output.mkdir(parents=True, exist_ok=True)

  failed = False
  for name, cursor, left, top in CASES:
    errors, text = run_case(args, output, name, cursor, left, top)
    if errors:
      failed = True
      print(f'FAIL {name}: ' + '; '.join(errors))
      print(f'     the log is {output / name / "client.log"}')
    else:
      print(f'pass {name}: the {cursor} cursor at {left},{top} is as expected')

  if failed:
    return 1

  print('pass: every cursor was drawn with the expected pixels')
  return 0


if __name__ == '__main__':
  sys.exit(main())
