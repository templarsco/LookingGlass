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

"""Runs the Windows client with win:jitRender on a synthetic stream for a few
seconds, and reports what its pacing logged when it stopped: the time between
the display's vertical blanks, how soon after a blank the render thread was
let go, and how soon after it the frame had been submitted.

The client must be built with ENABLE_TESTS, which adds the framebuffer
capture that ends the run after the last frame. It opens a small window at
the corner of the display for as long as the run takes.

These are what the client measured itself, between a blank and its own
SwapBuffers. They are not when a frame was on the display, which only a
camera or an analyzer on the display can say, and not a latency.

Without --require-hardware only the run is checked, as a runner without a
display adapter has nothing to pace by, and the client falls back. On a PC
with a display, --require-hardware also wants the graphics kernel's blanks,
measured, and a stable rate."""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BLANK = re.compile(
  r'Vertical blank by (?P<source>.+?): (?P<blanks>\d+) blanks, '
  r'(?P<period>[\d.]+) ms \((?P<hz>[\d.]+) Hz\) (?P<how>measured|of the display mode), '
  r'p50 (?P<p50>[\d.]+) p95 (?P<p95>[\d.]+) p99 (?P<p99>[\d.]+) '
  r'max (?P<max>[\d.]+) ms, (?P<missed>\d+) missed, (?P<early>\d+) early')

RENDER = re.compile(
  r'Render after the blank: let go at p50 (?P<wake_p50>[\d.]+) '
  r'p95 (?P<wake_p95>[\d.]+) p99 (?P<wake_p99>[\d.]+) '
  r'max (?P<wake_max>[\d.]+) ms, frame submitted at p50 (?P<submit_p50>[\d.]+) '
  r'p95 (?P<submit_p95>[\d.]+) p99 (?P<submit_p99>[\d.]+) '
  r'max (?P<submit_max>[\d.]+) ms; (?P<late>\d+) of (?P<frames>\d+) frames')


def run_client(args, capture, log, frames):
  # an empty configuration, so that the user's client.ini cannot change it
  config = (Path(log).parent / 'appdata').resolve()
  config.mkdir(exist_ok=True)
  environment = os.environ.copy()
  environment['APPDATA']         = str(config)
  environment['LOCALAPPDATA']    = str(config)
  environment['XDG_CONFIG_HOME'] = str(config)

  command = shlex.split(args.runner) + [
    args.client,
    'app:transport=test',
    'app:renderer=OpenGL',
    f'test:width={args.width}',
    f'test:height={args.height}',
    'test:format=bgra',
    'test:damage=full',
    f'test:frameRate={args.rate}',
    f'test:frameCount={frames}',
    'test:holdLastFrame=yes',
    'test:realtime=yes',
    f'test:captureFile={capture}',
    f'test:captureFrame={frames}',
    'test:captureDelay=2',
    f'win:size={args.width}x{args.height}',
    f'win:position={args.position}',
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
    'win:jitRender=yes',
  ] + args.client_args

  started = time.monotonic()
  with open(log, 'wb') as output:
    try:
      status = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
          timeout=args.timeout, env=environment).returncode
    except subprocess.TimeoutExpired:
      status = None
  return status, time.monotonic() - started


def number(match, key):
  return float(match.group(key))


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--runner', default='',
      help='command that runs Windows programs, such as wine64')
  parser.add_argument('--output', help='keep the log and pacing.json here')
  parser.add_argument('--rate', type=int, default=240,
      help='frames a second that the synthetic stream makes (default 240)')
  parser.add_argument('--seconds', type=float, default=5,
      help='how long the stream runs (default 5)')
  parser.add_argument('--width', type=int, default=320)
  parser.add_argument('--height', type=int, default=200)
  parser.add_argument('--position', default='0x0',
      help='where the window is, such as 0x0 or center')
  parser.add_argument('--timeout', type=float, default=90)
  parser.add_argument('--require-hardware', action='store_true',
      help='also fail unless the graphics kernel gave measured blanks, none '
           'were missed or early, and the frames were in time')
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after --')
  args = parser.parse_args()

  output = Path(args.output or tempfile.mkdtemp(prefix='lg-win-pacing-'))
  output.mkdir(parents=True, exist_ok=True)
  capture = output / 'capture.lgcapture'
  log     = output / 'client.log'
  capture.unlink(missing_ok=True)

  frames = max(2, round(args.rate * args.seconds))
  status, seconds = run_client(args, capture, log, frames)
  text = log.read_text(errors='replace')

  errors = []
  if status is None:
    errors.append('the client timed out')
  elif status != 0:
    errors.append(f'the client exited with status {status}')
  if 'Using JIT render mode' not in text:
    errors.append('the client did not use JIT render mode')

  result = {'seconds': round(seconds, 2), 'frames_asked': frames,
    'rate': args.rate}
  blank  = BLANK.search(text)
  render = RENDER.search(text)
  if blank:
    result['blank'] = {k: (v if k in ('source', 'how') else float(v))
        for k, v in blank.groupdict().items()}
  if render:
    result['render'] = {k: float(v) for k, v in render.groupdict().items()}

  if args.require_hardware:
    if not blank:
      errors.append('the client logged no vertical blanks')
    else:
      b = result['blank']
      if b['source'] != 'graphics kernel':
        errors.append(f'the blanks came from {b["source"]}, not the graphics '
            'kernel')
      if b['how'] != 'measured':
        errors.append('the period was not measured')
      if b['missed'] > b['blanks'] * 0.01:
        errors.append(f'{int(b["missed"])} of {int(b["blanks"])} blanks were '
            'missed')
      if b['early']:
        errors.append(f'{int(b["early"])} blanks came early')
      if b['p99'] > b['period'] * 1.5:
        errors.append(f'the 99th percentile of the interval, {b["p99"]} ms, '
            'is over one and a half periods')
    if not render:
      errors.append('the client logged no frames after a blank')
    else:
      r = result['render']
      # the first frame makes the renderer's buffers and textures
      if r['late'] > r['frames'] * 0.01 + 1:
        errors.append(f'{int(r["late"])} of {int(r["frames"])} frames took '
            'over a period')
      if blank and r['submit_p99'] > result['blank']['period']:
        errors.append(f'the 99th percentile of the time to submit, '
            f'{r["submit_p99"]} ms, is over a period')

  result['passed'] = not errors
  result['errors'] = errors
  (output / 'pacing.json').write_text(json.dumps(result, indent=2) + '\n')

  if blank:
    b = result['blank']
    print(f'Vertical blank by {b["source"]}: {int(b["blanks"])} blanks, '
        f'{b["period"]:.4f} ms ({b["hz"]:.3f} Hz, {b["how"]}); interval p50 '
        f'{b["p50"]:.4f}, p95 {b["p95"]:.4f}, p99 {b["p99"]:.4f}, max '
        f'{b["max"]:.4f} ms; {int(b["missed"])} missed, {int(b["early"])} early')
  else:
    print('The client logged no vertical blank statistics')
  if render:
    r = result['render']
    print(f'The render thread was let go at p50 {r["wake_p50"]:.4f}, p95 '
        f'{r["wake_p95"]:.4f}, p99 {r["wake_p99"]:.4f}, max '
        f'{r["wake_max"]:.4f} ms after the blank, and the frame was submitted '
        f'at p50 {r["submit_p50"]:.4f}, p95 {r["submit_p95"]:.4f}, p99 '
        f'{r["submit_p99"]:.4f}, max {r["submit_max"]:.4f} ms; '
        f'{int(r["late"])} of {int(r["frames"])} frames took over a period')

  if errors:
    print(text)
    print('\n'.join(errors))
    return 1
  return 0


if __name__ == '__main__':
  sys.exit(main())
