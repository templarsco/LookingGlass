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

"""Runs the Windows client and compares the framebuffer it composed in its
window with test frame 4.

By default the frame comes from the client's synthetic test transport. With
--producer, it comes over the LGMP transport from lg-windows-client-producer,
which serves it on a named shared memory section. Adding --world makes the
producer let every account open the section, and the test then passes only
if the client refuses it.

With --section, the client reads an existing section instead, such as the
one that the HCS probe shares with a Windows guest whose Looking Glass IDD
serves its display there, and the test passes on the first frame it
composes, whatever it shows.

The client must be built with ENABLE_TESTS, which adds the framebuffer
capture. Under Wine, pass --runner wine64."""

import argparse
import os
import queue
import shlex
import struct
import subprocess
import sys
import tempfile
import threading
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


def expected_color(x, y, width=WIDTH, height=HEIGHT, serial=SERIAL):
  """The colour client/transports/Test/test.c generates at x, y."""
  r = x * 255 // max(width  - 1, 1)
  g = y * 255 // max(height - 1, 1)
  b = 0x30 if ((x // 32) ^ (y // 32)) & 1 else 0x90

  box  = min(width, height, 64)
  boxX = (serial * 7) % (width  - box if width  > box else 1)
  boxY = (serial * 5) % (height - box if height > box else 1)
  if boxX <= x < boxX + box and boxY <= y < boxY + box:
    color = (serial * 2654435761) & 0xffffffff
    r = (color >> 16) & 0xff
    g = (color >>  8) & 0xff
    b =  color        & 0xff

  return r, g, b


# the refusal ivshmemOpenDev() logs for a section others can open
REFUSAL = 'only this user and the VM may have access'


def test_transport():
  return [
    'app:transport=test',
    f'test:width={WIDTH}',
    f'test:height={HEIGHT}',
    'test:format=bgra',
    'test:damage=full',
    'test:frameRate=60',
    f'test:frameCount={SERIAL}',
    'test:holdLastFrame=yes',
    'test:realtime=no',
  ]


class Producer:
  """lg-windows-client-producer serving frames 1 to SERIAL on a section, or
  what its extra options say."""

  def __init__(self, args, name, log, size=(WIDTH, HEIGHT), frames=SERIAL,
      extra=()):
    self.log   = log
    self.lines = queue.Queue()
    command = shlex.split(args.runner) + [
      args.producer,
      f'--size={size[0]}x{size[1]}',
      f'--frames={frames}',
      f'--timeout={int(args.timeout) + 60}',
      name,
    ] + list(extra) + (['--world'] if args.world else [])
    self.process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        errors='replace')
    self.reader = threading.Thread(target=self._read, daemon=True)
    self.reader.start()

  def _read(self):
    with open(self.log, 'w') as output:
      for line in self.process.stdout:
        output.write(line)
        output.flush()
        self.lines.put(line)
    self.lines.put(None)

  def wait_ready(self, timeout):
    """Waits for the producer to report that the section is ready."""
    try:
      while (line := self.lines.get(timeout=timeout)) is not None:
        if line.startswith('Serving '):
          return True
    except queue.Empty:
      pass
    return False

  def stop(self):
    if self.process.poll() is None:
      self.process.terminate()
    try:
      self.process.wait(timeout=10)
    except subprocess.TimeoutExpired:
      self.process.kill()
      self.process.wait()
    self.reader.join(timeout=10)


def run_client(args, transport, capture, log, frame=SERIAL,
    size=(WIDTH, HEIGHT)):
  command = shlex.split(args.runner) + [args.client] + transport + [
    'app:renderer=OpenGL',
    f'test:captureFile={capture}',
    f'test:captureFrame={frame}',
    'test:captureDelay=1',
    f'win:size={size[0]}x{size[1]}',
    'win:borderless=yes',
    'win:autoResize=no',
    'win:allowResize=no',
    'win:quickSplash=yes',
    'win:alerts=no',
    'win:noScreensaver=no',
    'opengl:mipmap=no',
    'opengl:vsync=no',
    'opengl:amdPinnedMem=no',
    'opengl:preventBuffer=yes',
  ] + args.client_args

  with open(log, 'wb') as output:
    try:
      return subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
          timeout=args.timeout).returncode
    except subprocess.TimeoutExpired:
      return None


def check_capture(path, width=WIDTH, height=HEIGHT, serial=SERIAL):
  data = Path(path).read_bytes()
  if len(data) < HEADER.size:
    return ['the capture is shorter than its header']

  (magic, version, headerSize, frameSerial, sourceType, captureFormat,
   gotWidth, gotHeight, stride, flags, dataSize) = HEADER.unpack_from(data)

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
  expect('width', gotWidth, width)
  expect('height', gotHeight, height)
  expect('stride', stride, width * 4)
  expect('dataSize', dataSize, len(data) - HEADER.size)
  if errors:
    return errors

  pixels     = data[HEADER.size:]
  bottomUp   = flags & CAPTURE_BOTTOM_UP
  mismatches = 0
  for y in range(height):
    row = (height - 1 - y if bottomUp else y) * stride
    for x in range(width):
      offset = row + x * 4
      got    = tuple(pixels[offset:offset + 3])
      wanted = expected_color(x, y, width, height, serial)
      if max(abs(a - b) for a, b in zip(got, wanted)) > 2:
        if mismatches < 10:
          errors.append(f'pixel {x},{y} is {got}, expected {wanted}')
        mismatches += 1

  if mismatches:
    errors.append(f'{mismatches} of {width * height} pixels differ')
  return errors


def check_any_capture(path):
  """A frame from a guest, which shows whatever the guest displayed: the
  header must be sound, and the pixels are only described."""
  data = Path(path).read_bytes()
  if len(data) < HEADER.size:
    return ['the capture is shorter than its header'], None

  (magic, version, headerSize, frameSerial, sourceType, captureFormat,
   width, height, stride, flags, dataSize) = HEADER.unpack_from(data)

  errors = []
  if magic != CAPTURE_MAGIC or version != CAPTURE_VERSION or \
      headerSize != HEADER.size:
    errors.append(f'the capture header is not one this test knows: magic '
        f'{magic:#x}, version {version}, size {headerSize}')
  if frameSerial < 1:
    errors.append(f'frameSerial is {frameSerial}')
  if captureFormat != CAPTURE_RGBA8 or not width or not height or \
      stride < width * 4 or dataSize != len(data) - HEADER.size or \
      dataSize < stride * height:
    errors.append(f'the capture is {width}x{height}, stride {stride}, '
        f'format {captureFormat}, {dataSize} bytes of data')
  if errors:
    return errors, None

  pixels = data[HEADER.size:]
  colors = set()
  dark   = 0
  step   = max(1, (width * height) // 20000)
  for i in range(0, width * height, step):
    x, y   = i % width, i // width
    offset = y * stride + x * 4
    rgb    = tuple(pixels[offset:offset + 3])
    colors.add(rgb)
    dark  += max(rgb) < 16

  samples = (width * height + step - 1) // step
  return [], (f'frame {frameSerial} of type {sourceType}, composed at '
      f'{width}x{height}: {len(colors)} colours in {samples} samples, '
      f'{100 * dark // samples}% near black')


def run(args, output, capture, log):
  if args.section:
    return run_client(args,
        ['app:transport=lgmp', f'lgmp:shmDevice={args.section}'], capture,
        log, frame=1, size=(1280, 720))

  if not args.producer:
    return run_client(args, test_transport(), capture, log)

  # a fresh name, as the producer refuses a section that already exists
  name     = f'Local\\lg-smoke-{os.getpid()}'
  producer = Producer(args, name, output / 'producer.log')
  try:
    if not producer.wait_ready(args.timeout):
      producer.stop()
      print((output / 'producer.log').read_text(errors='replace'))
      print('the producer did not start serving frames')
      return None

    return run_client(args,
        ['app:transport=lgmp', f'lgmp:shmDevice={name}'], capture, log)
  finally:
    producer.stop()


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--producer',
      help='path to lg-windows-client-producer.exe, to use LGMP')
  parser.add_argument('--world', action='store_true',
      help='check that the client refuses a section every account can open')
  parser.add_argument('--section',
      help='read any frame from this existing section instead')
  parser.add_argument('--runner', default='',
      help='command that runs Windows programs, such as wine64')
  parser.add_argument('--output', help='keep the capture and logs here')
  parser.add_argument('--timeout', type=float, default=60)
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after --')
  args = parser.parse_args()
  if args.world and not args.producer:
    parser.error('--world needs --producer')
  if args.section and args.producer:
    parser.error('--section and --producer do not go together')

  output = Path(args.output or tempfile.mkdtemp(prefix='lg-win-smoke-'))
  output.mkdir(parents=True, exist_ok=True)
  capture = output / 'capture.lgcapture'
  log     = output / 'client.log'
  capture.unlink(missing_ok=True)
  log.unlink(missing_ok=True)

  status = run(args, output, capture, log)
  text   = log.read_text(errors='replace') if log.exists() else ''

  if args.world:
    if status is None or status == 0 or REFUSAL not in text or \
        capture.exists():
      print(text)
      print('the client did not refuse a section every account can open')
      return 1

    print('The client refused a shared memory section every account can '
        'open')
    return 0

  if status != 0:
    print(text)
    print('the client timed out' if status is None else
        f'the client exited with status {status}')
    return 1

  if not capture.exists():
    print(text)
    print('the client did not write a capture')
    return 1

  if args.section:
    errors, description = check_any_capture(capture)
    print(text)
    if errors:
      print('\n'.join(errors))
      return 1
    print(f'The client composed a frame from {args.section} in its window: '
        f'{description}')
    return 0

  errors = check_capture(capture)
  if errors:
    print(text)
    print('\n'.join(errors))
    return 1

  source = 'from a shared memory section over LGMP' if args.producer else \
    'from the test transport'
  print(f'The client composed the {WIDTH}x{HEIGHT} test frame {SERIAL} '
      f'{source} in its window with the expected pixels')
  return 0


if __name__ == '__main__':
  sys.exit(main())
