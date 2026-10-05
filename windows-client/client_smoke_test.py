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

"""Runs the Windows client on the synthetic test transport and compares the
framebuffer it composed in its window with the generated test frame.

The client must be built with ENABLE_TESTS, which adds the framebuffer
capture. Under Wine, pass --runner wine64."""

import argparse
import shlex
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

WIDTH  = 256
HEIGHT = 160
SERIAL = 4

CAPTURE_MAGIC     = 0x4c47434150545552
CAPTURE_VERSION   = 1
CAPTURE_RGBA8     = 0
CAPTURE_BOTTOM_UP = 0x8
FRAME_TYPE_BGRA   = 1

# LG_TestCaptureHeader in client/include/interface/test_capture.h
HEADER = struct.Struct('<QIIQIIIIIIQ')


def expected_color(x, y):
  """The colour client/transports/Test/test.c generates at x, y."""
  r = x * 255 // (WIDTH  - 1)
  g = y * 255 // (HEIGHT - 1)
  b = 0x30 if ((x // 32) ^ (y // 32)) & 1 else 0x90

  box  = min(WIDTH, HEIGHT, 64)
  boxX = (SERIAL * 7) % (WIDTH  - box)
  boxY = (SERIAL * 5) % (HEIGHT - box)
  if boxX <= x < boxX + box and boxY <= y < boxY + box:
    color = (SERIAL * 2654435761) & 0xffffffff
    r = (color >> 16) & 0xff
    g = (color >>  8) & 0xff
    b =  color        & 0xff

  return r, g, b


def run_client(args, capture, log):
  command = shlex.split(args.runner) + [
    args.client,
    'app:transport=test',
    'app:renderer=OpenGL',
    f'test:width={WIDTH}',
    f'test:height={HEIGHT}',
    'test:format=bgra',
    'test:damage=full',
    'test:frameRate=60',
    f'test:frameCount={SERIAL}',
    'test:holdLastFrame=yes',
    'test:realtime=no',
    f'test:captureFile={capture}',
    f'test:captureFrame={SERIAL}',
    'test:captureDelay=1',
    f'win:size={WIDTH}x{HEIGHT}',
    'win:borderless=yes',
    'win:autoResize=no',
    'win:allowResize=no',
    'win:quickSplash=yes',
    'win:alerts=no',
    'win:noScreensaver=no',
    'opengl:mipmap=no',
    'opengl:vsync=no',
    f'opengl:amdPinnedMem={args.amd_pinned_mem}',
    'opengl:preventBuffer=yes',
  ] + args.client_args

  with open(log, 'wb') as output:
    try:
      return subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
          timeout=args.timeout).returncode
    except subprocess.TimeoutExpired:
      return None


def check_capture(path):
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

  pixels     = data[HEADER.size:]
  bottomUp   = flags & CAPTURE_BOTTOM_UP
  mismatches = 0
  for y in range(HEIGHT):
    row = (HEIGHT - 1 - y if bottomUp else y) * stride
    for x in range(WIDTH):
      offset = row + x * 4
      got    = tuple(pixels[offset:offset + 3])
      wanted = expected_color(x, y)
      if max(abs(a - b) for a, b in zip(got, wanted)) > 2:
        if mismatches < 10:
          errors.append(f'pixel {x},{y} is {got}, expected {wanted}')
        mismatches += 1

  if mismatches:
    errors.append(f'{mismatches} of {WIDTH * HEIGHT} pixels differ')
  return errors


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--runner', default='',
      help='command that runs Windows programs, such as wine64')
  parser.add_argument('--output', help='keep the capture and log here')
  parser.add_argument('--timeout', type=float, default=60)
  parser.add_argument('--amd-pinned-mem', choices=('yes', 'no'), default='yes',
      help='exercise the default pinned-memory path when the GPU supports it')
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after --')
  args = parser.parse_args()

  output = Path(args.output or tempfile.mkdtemp(prefix='lg-win-smoke-'))
  output.mkdir(parents=True, exist_ok=True)
  capture = output / 'capture.lgcapture'
  log     = output / 'client.log'
  capture.unlink(missing_ok=True)

  status = run_client(args, capture, log)
  if status != 0:
    print(log.read_text(errors='replace'))
    print('the client timed out' if status is None else
        f'the client exited with status {status}')
    return 1

  if not capture.exists():
    print(log.read_text(errors='replace'))
    print('the client did not write a capture')
    return 1

  errors = check_capture(capture)
  if errors:
    print(log.read_text(errors='replace'))
    print('\n'.join(errors))
    return 1

  print(f'The client composed the {WIDTH}x{HEIGHT} test frame {SERIAL} in '
      'its window with the expected pixels')
  return 0


if __name__ == '__main__':
  sys.exit(main())
