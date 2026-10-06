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

"""Runs the Windows client on frames that the test producer serves at 120 per
second, and measures what the client's process costs the PC in CPU time while
its window can be seen, while it is minimized and when it is back:

  visible    the window is up, and every frame is copied, uploaded and drawn
  minimized  nobody can see it, so the client renders nothing, and the frames
             that arrive only replace each other
  restored   rendering resumes, and the last frame that the producer serves is
             what the client composes, with every pixel as generated

The test passes if the client logged that it paused and resumed, used under a
third of the CPU time that it used visible while it was minimized (when
visible it used enough for that to mean something), and composed the last frame
with the expected pixels after it was restored. The
window does not take the focus, and is minimized for a few seconds. CPU time is
the process's own, as Windows counts it: it says nothing of the GPU. It needs
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
from client_smoke_test import Producer, check_capture  # noqa: E402

WINDOW_CLASS = 'LookingGlassClient'

SW_SHOWNOACTIVATE   = 4
SW_SHOWMINNOACTIVE  = 7

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

# what the producer serves, unless told otherwise: enough for what a visible
# client does with each frame to cost far more than what it does when no frame
# is drawn (the threads that poll the guest cost the same either way)
SIZE = '1280x720'
FPS  = 120

# a client that is hidden costs at most this much of what it costs visible
MAX_HIDDEN_FRACTION = 1 / 3

# the least that a visible client has to cost for the test to tell the two
# apart, as a fraction of a core
MIN_VISIBLE = 0.03


class FILETIME(ctypes.Structure):
  _fields_ = [('low', wintypes.DWORD), ('high', wintypes.DWORD)]

  def seconds(self):
    return ((self.high << 32) | self.low) / 1e7


def load_api():
  kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
  user32   = ctypes.WinDLL('user32', use_last_error=True)
  window_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND,
      wintypes.LPARAM)

  kernel32.OpenProcess.restype = wintypes.HANDLE
  kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL,
      wintypes.DWORD]
  kernel32.GetProcessTimes.argtypes = [wintypes.HANDLE] + \
      [ctypes.POINTER(FILETIME)] * 4
  kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

  user32.EnumWindows.argtypes = [window_proc, wintypes.LPARAM]
  user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND,
      ctypes.POINTER(wintypes.DWORD)]
  user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
  user32.IsWindowVisible.argtypes = [wintypes.HWND]
  user32.IsIconic.argtypes = [wintypes.HWND]
  user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
  return kernel32, user32, window_proc


def find_window(api, pid):
  kernel32, user32, window_proc = api
  found = []

  def callback(hwnd, param):
    owner = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
    if owner.value == pid:
      name = ctypes.create_unicode_buffer(256)
      user32.GetClassNameW(hwnd, name, len(name))
      if name.value == WINDOW_CLASS:
        found.append(hwnd)
    return True

  user32.EnumWindows(window_proc(callback), 0)
  return found[0] if found else None


def cpu_seconds(api, pid):
  """The kernel and user time that the process has used, in seconds."""
  kernel32 = api[0]
  handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
  if not handle:
    raise OSError(ctypes.get_last_error(), 'OpenProcess failed')
  try:
    created, exited, kernel, user = (FILETIME() for _ in range(4))
    kernel32.GetProcessTimes(handle, ctypes.byref(created),
        ctypes.byref(exited), ctypes.byref(kernel), ctypes.byref(user))
    return kernel.seconds() + user.seconds()
  finally:
    kernel32.CloseHandle(handle)


def measure(api, pid, seconds):
  """The CPU that the process used over a time, as a fraction of one core."""
  before = cpu_seconds(api, pid)
  start  = time.monotonic()
  time.sleep(seconds)
  used   = cpu_seconds(api, pid) - before
  return used / (time.monotonic() - start)


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--producer', required=True,
      help='path to lg-windows-client-producer.exe')
  parser.add_argument('--seconds', type=float, default=3,
      help='how long to measure each state (default 3)')
  parser.add_argument('--size', default=SIZE,
      help=f'the size of the frames, WxH (default {SIZE})')
  parser.add_argument('--fps', type=int, default=FPS,
      help=f'the frames per second that the producer serves (default {FPS})')
  parser.add_argument('--output', help='keep the logs here')
  parser.add_argument('--timeout', type=float, default=60)
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after --')
  args = parser.parse_args()

  if sys.platform != 'win32':
    print('skipped: this test needs Windows')
    return 0

  args.runner  = ''
  args.world   = False
  args.section = None

  width, height = (int(v) for v in args.size.split('x'))
  api    = load_api()
  output = Path(args.output or tempfile.mkdtemp(prefix='lg-win-idle-'))
  output.mkdir(parents=True, exist_ok=True)
  capture = output / 'capture.lgcapture'
  log     = output / 'client.log'
  capture.unlink(missing_ok=True)

  # frames for as long as the test goes on, which it does for about this long
  duration = 3 * args.seconds + 12
  frames   = int(args.fps * duration)
  name     = 'Local' + chr(92) + f'lg-idle-{os.getpid()}'
  producer = Producer(args, name, output / 'producer.log',
      size=(width, height), frames=frames, extra=[f'--fps={args.fps}'])

  env     = dict(os.environ, APPDATA=str(output), LOCALAPPDATA=str(output))
  errors  = []
  client  = None
  results = {}
  try:
    if not producer.wait_ready(args.timeout):
      print('FAIL: the producer did not start serving frames')
      return 1

    command = [args.client,
      'app:transport=lgmp', f'lgmp:shmDevice={name}',
      'app:renderer=OpenGL',
      f'test:captureFile={capture}', f'test:captureFrame={frames}',
      'test:captureDelay=1',
      f'win:size={width}x{height}', 'win:borderless=yes',
      'win:autoResize=no', 'win:allowResize=no', 'win:quickSplash=yes',
      'win:alerts=no', 'win:noScreensaver=no', 'win:showInactive=yes',
      'input:grabKeyboard=no', 'opengl:mipmap=no', 'opengl:vsync=no',
      'opengl:preventBuffer=yes',
    ] + args.client_args

    with open(log, 'wb') as stream:
      client = subprocess.Popen(command, stdout=stream,
          stderr=subprocess.STDOUT, env=env)

    deadline = time.monotonic() + args.timeout
    hwnd = None
    while hwnd is None and time.monotonic() < deadline:
      if client.poll() is not None:
        print(f'FAIL: the client exited with status {client.returncode}')
        return 1
      hwnd = find_window(api, client.pid)
      time.sleep(0.05)
    if hwnd is None:
      print('FAIL: the client did not show a window')
      return 1

    user32 = api[1]
    time.sleep(1.5)
    results['visible'] = measure(api, client.pid, args.seconds)

    user32.ShowWindow(hwnd, SW_SHOWMINNOACTIVE)
    time.sleep(0.7)
    if not user32.IsIconic(hwnd):
      errors.append('the window is not minimized')
    results['minimized'] = measure(api, client.pid, args.seconds)

    user32.ShowWindow(hwnd, SW_SHOWNOACTIVATE)
    time.sleep(0.7)
    if user32.IsIconic(hwnd):
      errors.append('the window is still minimized')
    results['restored'] = measure(api, client.pid, args.seconds)

    try:
      status = client.wait(timeout=args.timeout)
    except subprocess.TimeoutExpired:
      status = None
      errors.append('the client did not exit after it captured')
    if status not in (0, None):
      errors.append(f'the client exited with status {status}')
  finally:
    if client is not None and client.poll() is None:
      client.kill()
      client.wait()
    producer.stop()

  text = log.read_text(errors='replace') if log.exists() else ''
  for wanted in ('rendering is paused', 'rendering resumes'):
    if wanted not in text:
      errors.append(f'the client did not log "{wanted}"')

  if capture.exists():
    errors += check_capture(capture, width, height, frames)
  else:
    errors.append('the client did not capture the last frame')

  print(f'{args.size} at {args.fps} frames per second; CPU time of the client, '
        'as a fraction of one core: ' +
        ', '.join(f'{state} {value:.3f}' for state, value in results.items()))
  if 'visible' in results and 'minimized' in results:
    if results['visible'] < MIN_VISIBLE:
      errors.append('the visible client cost only %.3f of a core: the stream '
                    'is too small to tell it from a client that is not drawing '
                    '(try --size and --fps that are larger)' %
                    results['visible'])
    elif results['minimized'] > results['visible'] * MAX_HIDDEN_FRACTION:
      errors.append('the client used %.3f of a core minimized and %.3f '
                    'visible: more than %.0f%%' % (results['minimized'],
                    results['visible'], MAX_HIDDEN_FRACTION * 100))
    # that it resumed is what the last frame says: it is not composed unless
    # the client renders again. The CPU time that it takes after is printed

  if errors:
    for error in errors:
      print('FAIL ' + error)
    return 1

  print('pass: the client paused while minimized, resumed when restored, and '
        'composed the last frame with the expected pixels')
  return 0


if __name__ == '__main__':
  sys.exit(main())
