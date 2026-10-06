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

"""Runs the Windows client on the synthetic test transport for a matrix of
frame formats, sizes, strides and upload paths, and compares the framebuffer
it composed in its window with the generated pattern for each case.

client_smoke_test.py checks one frame. This checks the paths around it: padded
strides, odd sizes, packed 24-bit formats, and sequences that cycle the
renderer's upload buffers, with and without GL_AMD_pinned_memory. A PC whose
OpenGL driver does not offer that extension runs the ordinary upload path for
the pinned cases and says so. Pass --require-pinned on a PC with an AMD GPU to
fail instead.

The client must be built with ENABLE_TESTS, which adds the framebuffer
capture. Under Wine, pass --runner wine64. Only the Python standard library is
used. Each case runs with its own empty configuration directory, so the
user's client.ini cannot change what is tested.

HDR formats are not covered: the OpenGL renderer has no HDR-to-SDR path."""

import argparse
import hashlib
import json
import os
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

CAPTURE_MAGIC     = 0x4c47434150545552
CAPTURE_VERSION   = 1
CAPTURE_RGBA8     = 0
CAPTURE_BOTTOM_UP = 0x8

# LG_TestCaptureHeader in client/include/interface/test_capture.h
HEADER = struct.Struct('<QIIQIIIIIIQ')

# KVMFRFrameType in repos/LGProtocol, as the client reports it in the capture
SOURCE_TYPE = {'bgra': 1, 'rgba': 2, 'bgr32': 5, 'rgb24': 6}

TOLERANCE = 2

PINNED_IN_USE      = 'in use'
PINNED_UNUSED      = 'available but not in use'
PINNED_UNAVAILABLE = 'not available'


class Case:
  def __init__(self, name, fmt, width, height, stride, pinned,
      frames=4, rate=60, realtime=False):
    self.name     = name
    self.fmt      = fmt
    self.width    = width
    self.height   = height
    self.stride   = stride
    self.pinned   = pinned
    self.frames   = frames
    self.rate     = rate
    self.realtime = realtime


def build_cases():
  cases = []

  # Every format, packed and with the stride padded past the width, on both
  # upload paths. The test transport fills the padding with a value that
  # changes every frame, so a stride error shows up as garbage in the image.
  for fmt in ('bgra', 'rgba', 'bgr32', 'rgb24'):
    for pinned in ('yes', 'no'):
      cases.append(Case(f'{fmt}-packed-pinned-{pinned}',
          fmt, 256, 160, 0, pinned))
      cases.append(Case(f'{fmt}-padded-pinned-{pinned}',
          fmt, 256, 160, 272, pinned))

  # Sizes that are not a multiple of anything
  for fmt in ('bgra', 'rgb24', 'bgr32'):
    cases.append(Case(f'{fmt}-odd-pinned-yes', fmt, 257, 159, 0, 'yes'))
  cases.append(Case('rgb24-odd-padded-pinned-yes',
      'rgb24', 257, 159, 273, 'yes'))

  # Sequences cycle the renderer's upload buffers, which is what its fences
  # protect, so a first frame that is right does not cover them. The paced
  # ones also give the client frames while it is busy drawing the last.
  for pinned in ('yes', 'no'):
    cases.append(Case(f'bgra-sequence-pinned-{pinned}',
        'bgra', 640, 360, 656, pinned, frames=240))
    cases.append(Case(f'bgra-sequence-paced-pinned-{pinned}',
        'bgra', 640, 360, 656, pinned, frames=120, rate=120, realtime=True))

  # A real display size
  cases.append(Case('bgra-1080p-pinned-yes',
      'bgra', 1920, 1080, 1936, 'yes', frames=4))

  return cases


def expected_row(case, y):
  """The RGB bytes client/transports/Test/test.c generates for row y of the
  last frame, which test_getColor derives from the frame's serial."""
  width, height, serial = case.width, case.height, case.frames

  widthRange  = width  - 1 if width  > 1 else 1
  heightRange = height - 1 if height > 1 else 1
  boxSize     = min(width, height, 64)
  rangeX      = width  - boxSize if width  > boxSize else 1
  rangeY      = height - boxSize if height > boxSize else 1
  boxX        = (serial * 7) % rangeX
  boxY        = (serial * 5) % rangeY
  color       = (serial * 2654435761) & 0xffffffff
  inBoxRow    = boxY <= y < boxY + boxSize

  row = bytearray(width * 3)
  g   = y * 255 // heightRange
  for x in range(width):
    if inBoxRow and boxX <= x < boxX + boxSize:
      r, gg, b = (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff
    else:
      r  = x * 255 // widthRange
      gg = g
      b  = 0x30 if ((x // 32) ^ (y // 32)) & 1 else 0x90
    row[x * 3    ] = r
    row[x * 3 + 1] = gg
    row[x * 3 + 2] = b
  return bytes(row)


def check_capture(case, path):
  """Returns the problems found in the capture, and its largest channel
  error."""
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
  expect('frameSerial', frameSerial, case.frames)
  expect('sourceType', sourceType, SOURCE_TYPE[case.fmt])
  expect('captureFormat', captureFormat, CAPTURE_RGBA8)
  expect('width', width, case.width)
  expect('height', height, case.height)
  expect('stride', stride, case.width * 4)
  expect('dataSize', dataSize, len(data) - HEADER.size)
  if errors:
    return errors, None

  pixels     = data[HEADER.size:]
  bottomUp   = flags & CAPTURE_BOTTOM_UP
  mismatches = 0
  worst      = 0
  for y in range(case.height):
    start  = (case.height - 1 - y if bottomUp else y) * stride
    actual = pixels[start:start + case.width * 4]
    got    = bytearray(case.width * 3)
    got[0::3] = actual[0::4]
    got[1::3] = actual[1::4]
    got[2::3] = actual[2::4]
    wanted = expected_row(case, y)
    if got == wanted:
      continue

    for x in range(case.width):
      error = max(abs(got[x * 3 + c] - wanted[x * 3 + c]) for c in range(3))
      worst = max(worst, error)
      if error > TOLERANCE:
        if mismatches < 5:
          errors.append(f'pixel {x},{y} is {tuple(got[x * 3:x * 3 + 3])}, '
              f'expected {tuple(wanted[x * 3:x * 3 + 3])}')
        mismatches += 1

  if mismatches:
    errors.append(f'{mismatches} of {case.width * case.height} pixels differ')
  return errors, worst


def pinned_state(log):
  # client/renderers/OpenGL/opengl.c logs one of these when it starts
  if 'Using GL_AMD_pinned_memory' in log:
    return PINNED_IN_USE
  if 'GL_AMD_pinned_memory is available but not in use' in log:
    return PINNED_UNUSED
  return PINNED_UNAVAILABLE


def gl_driver(log):
  info = {}
  for key in ('Vendor', 'Renderer', 'Version'):
    match = re.search(rf'opengl_renderStartup\s+\|\s+{key}\s*:\s*(.*)', log)
    if match:
      info[key.lower()] = match.group(1).strip()
  return info


def run_case(args, case, output):
  directory = output / case.name
  directory.mkdir(parents=True, exist_ok=True)
  capture = directory / 'capture.lgcapture'
  log     = directory / 'client.log'
  capture.unlink(missing_ok=True)

  # Empty configuration and data directories, so that the user's client.ini
  # cannot change what is tested. The client takes these environment
  # variables over the user's folders (common/src/platform/windows/paths.c),
  # and creates the directories with a call that needs absolute paths.
  config = (directory / 'appdata').resolve()
  data   = (directory / 'localappdata').resolve()
  config.mkdir(exist_ok=True)
  data.mkdir(exist_ok=True)
  environment = os.environ.copy()
  environment['APPDATA']         = str(config)
  environment['LOCALAPPDATA']    = str(data)
  environment['XDG_CONFIG_HOME'] = str(config)

  command = shlex.split(args.runner) + [
    args.client,
    'app:transport=test',
    'app:renderer=OpenGL',
    f'test:width={case.width}',
    f'test:height={case.height}',
    f'test:stride={case.stride}',
    f'test:format={case.fmt}',
    'test:damage=full',
    f'test:frameRate={case.rate}',
    f'test:frameCount={case.frames}',
    'test:holdLastFrame=yes',
    f'test:realtime={"yes" if case.realtime else "no"}',
    f'test:captureFile={capture}',
    f'test:captureFrame={case.frames}',
    'test:captureDelay=2' if case.realtime else 'test:captureDelay=1',
    f'win:size={case.width}x{case.height}',
    'win:borderless=yes',
    'win:autoResize=no',
    'win:allowResize=no',
    'win:quickSplash=yes',
    'win:alerts=no',
    'win:noScreensaver=no',
    # the window does not take the focus from whoever is at the PC
    'win:showInactive=yes',
    'input:grabKeyboard=no',
    'opengl:mipmap=no',
    'opengl:vsync=no',
    f'opengl:amdPinnedMem={case.pinned}',
    'opengl:preventBuffer=yes',
  ] + args.client_args

  result = {
    'name'    : case.name,
    'format'  : case.fmt,
    'width'   : case.width,
    'height'  : case.height,
    'stride'  : case.stride,
    'frames'  : case.frames,
    'pinned_requested': case.pinned,
  }

  started = time.monotonic()
  try:
    with open(log, 'wb') as stream:
      status = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
          timeout=args.timeout, env=environment).returncode
  except subprocess.TimeoutExpired:
    status = None
  result['seconds'] = round(time.monotonic() - started, 3)

  text = log.read_text(errors='replace')
  result['pinned']    = pinned_state(text)
  result['gl_driver'] = gl_driver(text)

  errors = []
  if status is None:
    errors.append('the client timed out')
  elif status != 0:
    errors.append(f'the client exited with status {status}')
  elif not capture.exists():
    errors.append('the client did not write a capture')
  else:
    problems, worst = check_capture(case, capture)
    errors += problems
    result['maximum_channel_error'] = worst
    result['pixels'] = case.width * case.height

  if case.pinned == 'yes' and args.require_pinned and \
      result['pinned'] != PINNED_IN_USE:
    errors.append('the pinned upload path was not in use, and '
        '--require-pinned asks for it (GL_AMD_pinned_memory is '
        f'{result["pinned"]})')

  result['errors'] = errors
  result['passed'] = not errors
  return result


def sha256(path):
  digest = hashlib.sha256()
  with open(path, 'rb') as stream:
    for block in iter(lambda: stream.read(1 << 20), b''):
      digest.update(block)
  return digest.hexdigest()


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('client', nargs='?', help='path to looking-glass-client.exe')
  parser.add_argument('--runner', default='',
      help='command that runs Windows programs, such as wine64')
  parser.add_argument('--output', help='keep the captures, logs and result.json here')
  parser.add_argument('--timeout', type=float, default=60,
      help='seconds each case may take')
  parser.add_argument('--only', action='append', default=[], metavar='SUBSTRING',
      help='run only the cases whose name contains this, repeatable')
  parser.add_argument('--list', action='store_true',
      help='list the cases and exit')
  parser.add_argument('--require-pinned', action='store_true',
      help='fail a pinned case that did not use GL_AMD_pinned_memory, for a PC '
           'with an AMD GPU')
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after --')
  args = parser.parse_args()

  cases = build_cases()
  if args.only:
    cases = [case for case in cases
        if any(part in case.name for part in args.only)]

  if args.list:
    for case in cases:
      print(f'{case.name}: {case.fmt} {case.width}x{case.height} '
          f'stride {case.stride}, pinned {case.pinned}, {case.frames} frames')
    return 0

  if not args.client:
    parser.error('the path to looking-glass-client.exe is required')
  if not cases:
    parser.error('no case matches --only')

  output = Path(args.output or tempfile.mkdtemp(prefix='lg-win-formats-'))
  output.mkdir(parents=True, exist_ok=True)

  results = []
  for case in cases:
    result = run_case(args, case, output)
    results.append(result)
    state = 'ok  ' if result['passed'] else 'FAIL'
    detail = '' if result['passed'] else '  ' + '; '.join(result['errors'])
    print(f'{state} {case.name:<40} pinned: {result["pinned"]}{detail}',
        flush=True)

  failed  = [result for result in results if not result['passed']]
  drivers = {json.dumps(r['gl_driver'], sort_keys=True) for r in results}
  summary = {
    'client_sha256': sha256(args.client),
    'cases'        : len(results),
    'passed'       : len(results) - len(failed),
    'failed'       : len(failed),
    'pixels'       : sum(r.get('pixels', 0) for r in results),
    'pinned_used'  : sum(r['pinned'] == PINNED_IN_USE for r in results),
    'pinned_wanted': sum(r['pinned_requested'] == 'yes' for r in results),
    'gl_drivers'   : [json.loads(driver) for driver in sorted(drivers)],
    'results'      : results,
  }
  (output / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')

  for result in failed:
    print()
    print(f'{result["name"]}:')
    print((output / result['name'] / 'client.log').read_text(errors='replace'))
    print('\n'.join(result['errors']))

  print()
  print(f'{summary["passed"]} of {summary["cases"]} cases composed the '
      f'expected pixels; the pinned upload path was used by '
      f'{summary["pinned_used"]} of the {summary["pinned_wanted"]} cases '
      'that asked for it')
  return 1 if failed else 0


if __name__ == '__main__':
  sys.exit(main())
