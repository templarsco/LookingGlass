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

"""Runs the Windows client where there is no OpenGL that it can use, and checks
that it uses Direct3D 11 instead:

  fallback  with the renderer left to the client (the default, auto), it finds
            that OpenGL cannot be used before it makes its window, says so,
            uses the Direct3D 11 renderer, and composes the test frame in its
            window with every pixel as generated
  forced    with OpenGL asked for, it does not use another renderer: it says
            that OpenGL cannot be used and stops, with a status that is not 0

A Windows PC has an OpenGL on it that cannot be used when it has no driver for
its GPU, which is what a virtual machine or a remote session often has, and
then it is Microsoft's software OpenGL 1.1. The client is run from a copy of
itself, in a folder of its own, so that the OpenGL that is next to it, which
Mesa's is in the tests of CI, is not what it finds.

On a PC that has an OpenGL driver, the client would use it, so there are two
ways to run this test:

  --require-fallback  for a PC that has none (a runner that has no GPU): the
                      client has to fall back, or the test fails
  --simulate          for a PC that has one: the client is told with an
                      environment variable, which only a client built with
                      ENABLE_TESTS reads, that OpenGL cannot be used

With neither, the test says that OpenGL could be used, and passes if the client
then used it. The client must be built with ENABLE_TESTS, which adds the
framebuffer capture. The window does not take the focus. It needs Windows."""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from client_smoke_test import WIDTH, HEIGHT, SERIAL, check_capture  # noqa: E402

CANNOT = 'OpenGL cannot be used here'


def run_client(args, directory, name, renderer, simulate):
  output = directory / name
  output.mkdir(parents=True, exist_ok=True)
  capture = output / 'capture.lgcapture'
  log     = output / 'client.log'

  config = (output / 'appdata').resolve()
  config.mkdir(exist_ok=True)
  env = dict(os.environ, APPDATA=str(config), LOCALAPPDATA=str(config))
  if simulate:
    env['LG_TEST_NO_OPENGL'] = '1'

  command = [str(directory / 'client' / 'looking-glass-client.exe'),
    'app:transport=test', f'test:width={WIDTH}', f'test:height={HEIGHT}',
    'test:format=bgra', 'test:damage=full', 'test:frameRate=60',
    f'test:frameCount={SERIAL}', 'test:holdLastFrame=yes',
    'test:realtime=no', f'test:captureFile={capture}',
    f'test:captureFrame={SERIAL}', 'test:captureDelay=1',
    f'win:size={WIDTH}x{HEIGHT}', 'win:borderless=yes', 'win:autoResize=no',
    'win:allowResize=no', 'win:quickSplash=yes', 'win:alerts=no',
    'win:noScreensaver=no',
    # the window does not take the focus from whoever is at the PC
    'win:showInactive=yes', 'input:grabKeyboard=no',
    'opengl:mipmap=no', 'opengl:vsync=no',
  ] + ([f'app:renderer={renderer}'] if renderer else []) + args.client_args

  try:
    with open(log, 'wb') as stream:
      status = subprocess.run(command, stdout=stream,
          stderr=subprocess.STDOUT, timeout=args.timeout, env=env).returncode
  except subprocess.TimeoutExpired:
    status = None
  return status, capture, log.read_text(errors='replace')


def test_fallback(args, directory):
  status, capture, text = run_client(args, directory, 'fallback', None,
      args.simulate)

  errors = []
  fell_back = CANNOT in text
  if status != 0:
    errors.append('the client timed out' if status is None else
        f'the client exited with status {status}')

  if args.simulate or args.require_fallback:
    if not fell_back:
      errors.append(f'the client did not say "{CANNOT}"')
    if 'Using Renderer: D3D11' not in text:
      errors.append('the client did not use the Direct3D 11 renderer')
    note = 'OpenGL could not be used, and Direct3D 11 was'
  else:
    wanted = 'D3D11' if fell_back else 'OpenGL'
    if f'Using Renderer: {wanted}' not in text:
      errors.append(f'the client did not use the {wanted} renderer')
    note = ('OpenGL could not be used, and Direct3D 11 was' if fell_back else
            'OpenGL could be used here, and was')

  if not capture.exists():
    errors.append('the client did not write a capture')
  else:
    errors += check_capture(capture)
  return errors, note


def test_forced(args, directory):
  status, capture, text = run_client(args, directory, 'forced', 'OpenGL',
      args.simulate)

  if not (args.simulate or args.require_fallback) and CANNOT not in text:
    return [], 'OpenGL could be used here, so there was nothing to refuse'

  errors = []
  if status in (0, None):
    errors.append('the client started with OpenGL that cannot be used')
  if CANNOT not in text:
    errors.append(f'the client did not say "{CANNOT}"')
  if 'Using Renderer: D3D11' in text:
    errors.append('the client used Direct3D 11 where OpenGL was asked for')
  if capture.exists():
    errors.append('the client opened a window')
  return errors, 'refused to use another renderer'


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--require-fallback', action='store_true',
      help='fail if the client can use OpenGL here, as a PC with no driver '
           'for its GPU cannot')
  parser.add_argument('--simulate', action='store_true',
      help='tell the client that OpenGL cannot be used (only one built with '
           'ENABLE_TESTS listens)')
  parser.add_argument('--output', help='keep the captures and logs here')
  parser.add_argument('--timeout', type=float, default=60)
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after the client')
  args = parser.parse_args()

  if sys.platform != 'win32':
    print('skipped: this test needs Windows')
    return 0

  directory = Path(args.output or tempfile.mkdtemp(prefix='lg-win-fallback-'))
  directory.mkdir(parents=True, exist_ok=True)

  # a copy of the client alone, so that no OpenGL is next to it
  (directory / 'client').mkdir(exist_ok=True)
  shutil.copy2(args.client, directory / 'client' / 'looking-glass-client.exe')

  failed = False
  for name, test in (('fallback', test_fallback), ('forced', test_forced)):
    errors, note = test(args, directory)
    if errors:
      failed = True
      print(f'FAIL {name}: ' + '; '.join(errors))
      print(f'     the log is {directory / name / "client.log"}')
    else:
      print(f'pass {name}: {note}')

  if failed:
    return 1

  if args.simulate or args.require_fallback:
    print('pass: the client used Direct3D 11 where OpenGL could not be used')
  else:
    print('pass: the client used the renderer that could be used')
  return 0


if __name__ == '__main__':
  sys.exit(main())
