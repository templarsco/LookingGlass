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

"""Runs the Windows client with each renderer where it shows its splash, and
checks what the renderer drew of Dear ImGui's overlay:

  The client is told to read a shared memory section that is not there, so
  there is no guest, no frame and no cursor, and what is on its window is the
  splash screen: a radial gradient from a purple in the middle to black, the
  logo (an image that the overlay makes a texture of), a tagline under it and a
  version and a copyright at the bottom (text, from the font atlas).

For each renderer the capture is checked for each part of it, where it should
be, and then the captures of the renderers are compared pixel by pixel: the
first of the renderers, which is OpenGL unless --renderers says otherwise, is
what the others are held to, as it is what the overlays were made for.

The window does not take the focus. It needs Windows, and does not run
elsewhere. The client must be built with ENABLE_TESTS, which adds the
framebuffer capture."""

import argparse
import ctypes
import os
import subprocess
import sys
import tempfile
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from client_smoke_test import (CAPTURE_BOTTOM_UP, CAPTURE_MAGIC,  # noqa: E402
    CAPTURE_RGBA8, CAPTURE_VERSION, HEADER)

WIDTH, HEIGHT = 640, 360

# the colour that the gradient is in the middle, 0.234375 0.015625 0.425781 of 255
CENTRE = (60, 4, 109)

# how far apart two renderers' pixels may be, and how many may be further
TOLERANCE = 12
MAX_DIFFERENT = 0.01


def read_capture(path):
  """The pixels of the capture as a function of the position from the top, or
  the errors."""
  data = Path(path).read_bytes()
  if len(data) < HEADER.size:
    return ['the capture is shorter than its header'], None

  (magic, version, headerSize, frameSerial, sourceType, captureFormat,
   width, height, stride, flags, dataSize) = HEADER.unpack_from(data)

  errors = []
  def expect(name, value, wanted):
    if value != wanted:
      errors.append(f'{name} is {value}, expected {wanted}')

  expect('magic', magic, CAPTURE_MAGIC)
  expect('version', version, CAPTURE_VERSION)
  expect('headerSize', headerSize, HEADER.size)
  expect('captureFormat', captureFormat, CAPTURE_RGBA8)
  expect('width', width, WIDTH)
  expect('height', height, HEIGHT)
  expect('stride', stride, WIDTH * 4)
  expect('dataSize', dataSize, len(data) - HEADER.size)
  if errors:
    return errors, None

  pixels    = data[HEADER.size:]
  bottom_up = flags & CAPTURE_BOTTOM_UP

  def pixel(x, y):
    offset = ((HEIGHT - 1 - y if bottom_up else y) * WIDTH + x) * 4
    return tuple(pixels[offset:offset + 3])

  return [], pixel


PAGE_READWRITE = 0x04
SECTION_SIZE   = 4 << 20


def make_section(name):
  """A section that nothing serves frames on: the client opens it, and finds no
  host in it, so it waits for one."""
  kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
  kernel32.CreateFileMappingW.restype = wintypes.HANDLE
  kernel32.CreateFileMappingW.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
      wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.LPCWSTR]
  handle = kernel32.CreateFileMappingW(wintypes.HANDLE(-1), None,
      PAGE_READWRITE, 0, SECTION_SIZE, name)
  if not handle:
    raise OSError(ctypes.get_last_error(), 'CreateFileMappingW failed')
  return kernel32, handle


def run_client(args, output, renderer):
  directory = output / renderer
  directory.mkdir(parents=True, exist_ok=True)
  capture = directory / 'capture.lgcapture'
  log     = directory / 'client.log'
  capture.unlink(missing_ok=True)

  config = (directory / 'appdata').resolve()
  config.mkdir(exist_ok=True)
  env = dict(os.environ, APPDATA=str(config), LOCALAPPDATA=str(config))

  # a section with no host in it, so that no guest connects, and the client
  # waits, with its splash up
  section = 'Local' + chr(92) + f'lg-overlay-{os.getpid()}-{renderer}'
  kernel32, handle = make_section(section)

  command = [args.client,
    'app:transport=lgmp', f'lgmp:shmDevice={section}',
    f'app:renderer={renderer}',
    # captured after this many renders, with no frame to wait for
    f'test:captureFile={capture}', 'test:captureFrame=0',
    'test:captureDelay=12',
    f'win:size={WIDTH}x{HEIGHT}', 'win:borderless=yes', 'win:autoResize=no',
    'win:allowResize=no', 'win:quickSplash=no', 'win:alerts=no',
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
  finally:
    kernel32.CloseHandle(handle)

  # a client that has no guest to show ends, once it has written its capture,
  # with the status of one that did not get to show one, so what is looked for
  # is the capture, and what the renderer logged
  errors = []
  if status is None:
    errors.append('the client timed out')
  elif not capture.exists():
    errors.append('the client did not write a capture')

  text = log.read_text(errors='replace')
  for failure in ('Failed to initialize ImGui', 'Could not', 'Present failed',
                  'Unsupported', 'FATAL'):
    if failure in text:
      errors.append(f'the client logged "{failure}"')
  return errors, capture


def check_splash(pixel):
  """Each part of the splash is where it should be."""
  errors = []

  # the gradient, at a point left of the logo (the logo is 200 pixels square, in
  # the middle), a fifth of the way to the edge, which is nearly the middle's
  # colour with a little less of it
  x, y = WIDTH // 2 - 150, HEIGHT // 2
  r, g, b = pixel(x, y)
  if not (20 <= r <= CENTRE[0] and g <= 20 and 50 <= b <= CENTRE[2]):
    errors.append(f'the gradient at {x},{y} is {(r, g, b)}, not a purple a '
        f'little less than {CENTRE}')

  # and a good deal darker at the corners, which it fades towards
  for x, y in ((0, 0), (WIDTH - 1, 0), (0, HEIGHT - 1), (WIDTH - 1, HEIGHT - 1)):
    r, g, b = pixel(x, y)
    if not (max(r, g) <= 24 and 8 <= b <= CENTRE[2] // 2):
      errors.append(f'the corner {x},{y} is {(r, g, b)}, not a dark purple '
          f'(the middle is {CENTRE})')

  # the logo, in the middle, has light pixels in it
  logo = [pixel(x, y) for y in range(HEIGHT // 2 - 90, HEIGHT // 2 + 90, 3)
          for x in range(WIDTH // 2 - 90, WIDTH // 2 + 90, 3)]
  light = sum(1 for p in logo if min(p) > 150)
  if light < 40:
    errors.append(f'only {light} of the {len(logo)} points of the logo are '
        'light')

  # the tagline, under the logo, and the footer, at the bottom, are light text
  # on the dark: a row of them has light pixels in it, and the row between
  # the logo and the footer, away from both, has none
  def light_pixels(top, bottom):
    return sum(1 for y in range(top, bottom) for x in range(WIDTH // 2 - 160,
               WIDTH // 2 + 160) if min(pixel(x, y)) > 120)

  tagline = light_pixels(HEIGHT // 2 + 100, HEIGHT // 2 + 150)
  footer  = light_pixels(HEIGHT - 50, HEIGHT - 4)
  if tagline < 100:
    errors.append(f'the tagline under the logo has {tagline} light pixels')
  if footer < 100:
    errors.append(f'the footer has {footer} light pixels')
  return errors


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--renderers', default='OpenGL,D3D11',
      help='the renderers to run, the first of which is what the others are '
           'compared with (default OpenGL,D3D11)')
  parser.add_argument('--output', help='keep the captures and logs here')
  parser.add_argument('--timeout', type=float, default=60)
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after the client')
  args = parser.parse_args()

  if sys.platform != 'win32':
    print('skipped: this test needs Windows')
    return 0

  output = Path(args.output or tempfile.mkdtemp(prefix='lg-win-overlay-'))
  output.mkdir(parents=True, exist_ok=True)

  failed  = False
  pixels  = {}
  for renderer in args.renderers.split(','):
    errors, capture = run_client(args, output, renderer)
    if not errors:
      errors, pixel = read_capture(capture)
      if not errors:
        errors += check_splash(pixel)
        pixels[renderer] = pixel
    if errors:
      failed = True
      print(f'FAIL {renderer}: ' + '; '.join(errors))
      print(f'     the log is {output / renderer / "client.log"}')
    else:
      print(f'pass {renderer}: the splash is where it should be')

  names = list(pixels)
  for name in names[1:]:
    reference, other = pixels[names[0]], pixels[name]
    different = worst = 0
    for y in range(HEIGHT):
      for x in range(WIDTH):
        a, b = reference(x, y), other(x, y)
        delta = max(abs(i - j) for i, j in zip(a, b))
        worst = max(worst, delta)
        different += delta > TOLERANCE

    share = different / (WIDTH * HEIGHT)
    if share > MAX_DIFFERENT:
      failed = True
      print(f'FAIL {name} against {names[0]}: {different} of '
          f'{WIDTH * HEIGHT} pixels ({100 * share:.2f}%) are more than '
          f'{TOLERANCE} apart, by up to {worst}')
    else:
      print(f'pass {name} against {names[0]}: {different} of '
          f'{WIDTH * HEIGHT} pixels ({100 * share:.2f}%) are more than '
          f'{TOLERANCE} apart, by up to {worst}')

  if failed:
    return 1

  print('pass: each renderer drew the splash, and alike')
  return 0


if __name__ == '__main__':
  sys.exit(main())
