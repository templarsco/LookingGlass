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

"""Runs the Windows client on frames that lg-windows-client-producer serves on
a shared memory section while the producer does what a guest does: it sets
another resolution with the session going on, or its capture host restarts on
the same memory, which gives the LGMP session a new ID. A test passes if the
client composes the last frame in its window with the size and every pixel
that frame has, and it logged each size that it was given, in order.

  resize   the resolution changes twice, in one session
  restart  the host restarts at once, and serves another resolution
  same     the host restarts at once, and serves the same resolution
  gone     the host is gone for longer than the second that a client waits
           for it, and then serves another resolution

The client must be built with ENABLE_TESTS, which adds the framebuffer
capture. It opens a small window at a time, and each scenario takes a few
seconds. What this does not cover: a frame of a guest, as the producer is not
a guest, and focus, which no test here takes from the user."""

import argparse
import os
import re
import sys
import tempfile
from collections import namedtuple
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from client_smoke_test import Producer, check_capture, run_client  # noqa: E402

Step     = namedtuple('Step', 'width height frames restart')
Scenario = namedtuple('Scenario', 'name text steps gap')


SCENARIOS = [
  Scenario('resize', 'the resolution changes twice in one session', [
    Step(256, 160, 2, False),
    Step(320, 200, 4, False),
    Step(192, 120, 6, False),
  ], 0),
  Scenario('restart', 'the host restarts at once, at another resolution', [
    Step(256, 160, 3, False),
    Step(320, 200, 4, True),
  ], 0),
  Scenario('same', 'the host restarts at once, at the same resolution', [
    Step(256, 160, 3, False),
    Step(256, 160, 4, True),
  ], 0),
  Scenario('gone', 'the host is gone for longer than a client waits for it', [
    Step(256, 160, 3, False),
    Step(320, 200, 4, True),
  ], 1600),
]

# what client/src/main.c logs
FORMAT  = re.compile(r'Format: \S+ (\d+)x(\d+) \(')
STARTED = 'Starting session'
LOST    = 'Waiting for the host to restart'


def producer_options(scenario, args):
  first = scenario.steps[0]
  extra = [f'--dwell={args.dwell}', f'--gap={scenario.gap}',
           f'--fps={args.fps}']
  for step in scenario.steps[1:]:
    extra.append(f'--then={step.width}x{step.height}:{step.frames}' +
        (':restart' if step.restart else ''))
  return (first.width, first.height), first.frames, extra


def sizes_logged(text):
  """The sizes the client was given, in order, with a repeat counted once."""
  sizes = []
  for width, height in FORMAT.findall(text):
    size = (int(width), int(height))
    if not sizes or sizes[-1] != size:
      sizes.append(size)
  return sizes


def expected_sizes(scenario):
  sizes = []
  for step in scenario.steps:
    if not sizes or sizes[-1] != (step.width, step.height):
      sizes.append((step.width, step.height))
  return sizes


def run_scenario(args, scenario, output):
  output.mkdir(parents=True, exist_ok=True)
  capture = output / 'capture.lgcapture'
  log     = output / 'client.log'
  capture.unlink(missing_ok=True)
  log.unlink(missing_ok=True)

  size, frames, extra = producer_options(scenario, args)
  last = scenario.steps[-1]

  # a fresh name, as the producer refuses a section that already exists
  name     = f'Local\\lg-session-{os.getpid()}-{scenario.name}'
  producer = Producer(args, name, output / 'producer.log', size=size,
      frames=frames, extra=extra)

  errors = []
  try:
    if not producer.wait_ready(args.timeout):
      return ['the producer did not start serving frames']

    # the window follows the frames' size, as it does for a guest
    followed = argparse.Namespace(**vars(args))
    followed.client_args = list(args.client_args) + [
      'win:autoResize=yes',
      'win:setGuestRes=no',
    ]
    status = run_client(followed,
        ['app:transport=lgmp', f'lgmp:shmDevice={name}'], capture, log,
        frame=last.frames, size=size)
  finally:
    producer.stop()

  text = log.read_text(errors='replace') if log.exists() else ''
  if status is None:
    errors.append('the client timed out')
  elif status != 0:
    errors.append(f'the client exited with status {status}')

  if capture.exists():
    errors += check_capture(capture, last.width, last.height, last.frames)
  else:
    errors.append('the client did not capture a frame')

  # one session for each host, and the client saw each one end
  sessions = 1 + sum(step.restart for step in scenario.steps)
  if text.count(STARTED) != sessions:
    errors.append(f'the client started {text.count(STARTED)} sessions, '
        f'expected {sessions}')
  if sessions > 1 and LOST not in text:
    errors.append('the client did not log that the host was gone')
  if sessions == 1 and LOST in text:
    errors.append('the client logged that the host was gone, which it was not')

  logged = sizes_logged(text)
  wanted = expected_sizes(scenario)
  if logged != wanted:
    errors.append('the client was given these sizes, in order: ' +
        ', '.join(f'{w}x{h}' for w, h in logged) + '; expected ' +
        ', '.join(f'{w}x{h}' for w, h in wanted))

  return errors


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--producer', required=True,
      help='path to lg-windows-client-producer.exe')
  parser.add_argument('--scenario', action='append',
      choices=[s.name for s in SCENARIOS],
      help='run only this one; the default is all of them')
  parser.add_argument('--runner', default='',
      help='command that runs Windows programs, such as wine64')
  parser.add_argument('--output', help='keep the captures and logs here')
  parser.add_argument('--timeout', type=float, default=90)
  parser.add_argument('--dwell', type=int, default=500,
      help='milliseconds that the producer waits after a step (default 500)')
  parser.add_argument('--fps', type=int, default=60)
  parser.add_argument('--amd-pinned-mem', choices=('yes', 'no'), default='yes',
      help='exercise the default pinned-memory path when the GPU supports it')
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after --')
  args = parser.parse_args()
  args.world = False
  args.section = None

  output = Path(args.output or tempfile.mkdtemp(prefix='lg-win-sessions-'))
  failed = 0
  for scenario in SCENARIOS:
    if args.scenario and scenario.name not in args.scenario:
      continue

    errors = run_scenario(args, scenario, output / scenario.name)
    if errors:
      failed += 1
      print(f'FAIL {scenario.name}: {scenario.text}')
      for error in errors:
        print(f'  {error}')
      for kept in ('producer.log', 'client.log'):
        path = output / scenario.name / kept
        if path.exists():
          print(f'--- {kept}')
          print(path.read_text(errors='replace'))
    else:
      last = scenario.steps[-1]
      print(f'pass {scenario.name}: {scenario.text}; the client composed '
          f'frame {last.frames} at {last.width}x{last.height} with the '
          f'expected pixels')

  return 1 if failed else 0


if __name__ == '__main__':
  sys.exit(main())
