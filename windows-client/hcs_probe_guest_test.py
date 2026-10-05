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

"""Boots the HCS probe's guest init under QEMU with QEMU's ivshmem-plain
device, backed by a file this test fills with the probe's pattern, and plays
the probe's side of the serial protocol. The guest reaches the device the
way it reaches the IVSHMEM device the probe emulates on Hyper-V, then maps
the same memory again by physical address, as for a SharedMemory region.
With --connect-after, the test opens the serial port only after the guest
has booted, as the probe may on Hyper-V, where earlier output is lost."""

import argparse
import mmap
import os
import random
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

MAGIC  = 0x4c47534850524f42
GOLDEN = 0x9E3779B97F4A7C15
MASK   = (1 << 64) - 1
PAGE   = 4096


def cpio(entries):
  """An uncompressed newc archive, as the probe writes it."""
  out = bytearray()
  for ino, (name, mode, data, rdev) in enumerate(
      entries + [('TRAILER!!!', 0, b'', (0, 0))], 1):
    name = name.encode() + b'\0'
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1],
        len(name), 0)
    out += b'070701' + ''.join(f'{v:08X}' for v in fields).encode() + name
    out += b'\0' * (-len(out) % 4) + data
    out += b'\0' * (-len(out) % 4)
  return bytes(out)


class Guest:
  def __init__(self, sock, log):
    self.sock    = sock
    self.log     = log
    self.pending = b''

  def wait(self, prefix, timeout):
    deadline = time.monotonic() + timeout
    while True:
      while b'\n' in self.pending:
        line, self.pending = self.pending.split(b'\n', 1)
        text = line.decode(errors='replace').rstrip('\r')
        at = text.find('LGSHM ')
        if at >= 0 and text[at + 6:].startswith(prefix):
          return text[at + 6:]
      if time.monotonic() > deadline:
        sys.exit(f'timed out waiting for "{prefix}"')
      try:
        data = self.sock.recv(65536)
      except socket.timeout:
        continue
      if not data:
        sys.exit(f'the serial port closed while waiting for "{prefix}"')
      self.log.write(data)
      self.log.flush()
      self.pending += data

  def command(self, line, expect, timeout=60):
    self.sock.sendall(line.encode() + b'\n')
    reply = self.wait(expect.split()[0], timeout)
    print(f'{line} -> {reply}')
    if not reply.startswith(expect):
      sys.exit(f'expected "{expect}"')
    return reply


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('init', type=Path, help='the built guest init')
  parser.add_argument('kernel', type=Path, help='an x86_64 Linux kernel')
  parser.add_argument('--size-mib', type=int, default=32)
  parser.add_argument('--log', type=Path, help='where to write the serial log')
  parser.add_argument('--connect-after', type=float, default=0,
      help='seconds to leave the serial port unopened after QEMU starts')
  args = parser.parse_args()

  size  = args.size_mib << 20
  pages = size // PAGE
  tmp   = Path(tempfile.mkdtemp(prefix='lg-hcs-guest-'))

  initrd = tmp / 'initrd.cpio'
  initrd.write_bytes(cpio([
    ('dev'        , 0o040755, b'', (0, 0)),
    ('dev/console', 0o020600, b'', (5, 1)),
    ('proc'       , 0o040555, b'', (0, 0)),
    ('sys'        , 0o040555, b'', (0, 0)),
    ('init'       , 0o100755, args.init.read_bytes(), (0, 0)),
  ]))

  shared = tmp / 'shared'
  with open(shared, 'wb') as f:
    f.truncate(size)
  fd   = os.open(shared, os.O_RDWR)
  view = mmap.mmap(fd, size)
  nonce = random.getrandbits(64)
  for i in range(pages):
    struct.pack_into('<QQ', view, i * PAGE, MAGIC ^ i,
        nonce ^ ((i * GOLDEN) & MASK))

  accel = 'kvm' if os.access('/dev/kvm', os.R_OK | os.W_OK) else 'tcg'
  serial = tmp / 'serial.sock'
  qemu = subprocess.Popen([
    'qemu-system-x86_64', '-accel', accel, '-m', '512', '-smp', '2',
    '-display', 'none', '-monitor', 'none', '-no-reboot',
    '-kernel', args.kernel, '-initrd', initrd,
    '-append', 'console=ttyS0,115200 panic=-1 rdinit=/init',
    '-object', f'memory-backend-file,id=shared,share=on,mem-path={shared},'
               f'size={size}',
    '-device', 'ivshmem-plain,memdev=shared',
    '-chardev', f'socket,id=serial,path={serial},server=on,'
                f'wait={"off" if args.connect_after else "on"}',
    '-serial', 'chardev:serial',
  ])

  try:
    start = time.monotonic()
    for _ in range(100):
      if serial.exists():
        break
      time.sleep(0.1)
    # QEMU drops what the guest writes while nothing is connected
    time.sleep(args.connect_after)
    sock = socket.socket(socket.AF_UNIX)
    sock.connect(str(serial))
    sock.settimeout(0.5)

    log   = open(args.log or tmp / 'serial.log', 'wb')
    guest = Guest(sock, log)
    ready = guest.wait('ready', 300)
    print(f'{ready} after {time.monotonic() - start:.1f}s with {accel}')
    greeting = int(dict(f.split('=') for f in ready.split()[1:])['greeting'],
        16)
    if not args.connect_after and greeting != 1:
      sys.exit('the first greeting was lost although the port was open')
    if args.connect_after and greeting == 1:
      print('The guest greeted only once the port was open, so this run '
          'did not check a late connection')

    guest.command('info', 'info end')

    reply = guest.command('map pci 0x1af4 0x1110 0x2', 'map ok', 60)
    fields = dict(f.split('=') for f in reply.split()[2:])
    bar = int(fields['addr'], 16)
    if int(fields['size'], 16) != size:
      sys.exit('BAR2 does not have the size of the shared memory')

    verify = f'verify 0x{MAGIC:x} 0x{nonce:x} 0x{pages:x}'
    guest.command(verify, f'verify ok pages=0x{pages:x}')

    # guest to host
    page  = pages // 2 + 1
    value = random.getrandbits(64) | 1
    guest.command(f'write 0x{page:x} 0x{value:x}', 'write ok')
    seen = struct.unpack_from('<Q', view, page * PAGE + 16)[0]
    if seen != value:
      sys.exit(f'the host reads 0x{seen:x}, the guest wrote 0x{value:x}')

    # host to guest
    page  = pages - 1
    value = random.getrandbits(64) | 1
    struct.pack_into('<Q', view, page * PAGE + 16, value)
    guest.command(f'read 0x{page:x}',
        f'read ok page=0x{page:x} value=0x{value:x}')

    # single reads, where the guest placed BAR2 and by physical address
    def peeked(page):
      return (f'peek ok addr=0x{bar + page * PAGE:x} w0=0x{MAGIC ^ page:x} '
          f'w1=0x{nonce ^ ((page * GOLDEN) & MASK):x}')
    guest.command('peek pci 0x1af4 0x1110 0x2 0x0', peeked(0))
    guest.command(f'peek phys 0x{bar + 3 * PAGE:x}', peeked(3))
    guest.command('peek pci 0x1af4 0x1110 0x0 0x0', 'peek ok')

    # memory decoding off and on again through the configuration space
    reply = guest.command('config 0x1af4 0x1110 0x4 0x2', 'config ok')
    command = int(reply.split('value=')[1], 16)
    if not command & 2:
      sys.exit('memory decoding is off after map pci')
    guest.command(f'config 0x1af4 0x1110 0x4 0x2 0x{command & ~2:x}',
        'config ok')
    reply = guest.command('peek pci 0x1af4 0x1110 0x2 0x0', 'peek ok')
    if reply == peeked(0):
      sys.exit('BAR2 still answers with memory decoding off')
    reply = guest.command(f'config 0x1af4 0x1110 0x4 0x2 0x{command:x}',
        'config ok')
    if int(reply.split('value=')[1], 16) != command:
      sys.exit('memory decoding did not come back on')
    guest.command('peek pci 0x1af4 0x1110 0x2 0x0', peeked(0))
    guest.command(verify, 'verify ok')

    # the same memory by physical address, as for a SharedMemory region
    guest.command(f'map phys 0x{bar:x} 0x{size:x}', 'map ok')
    guest.command(verify, 'verify ok')
    guest.command(f'read 0x{page:x}',
        f'read ok page=0x{page:x} value=0x{value:x}')

    # a wrong pattern must not pass
    guest.command(f'verify 0x{MAGIC:x} 0x{nonce ^ 1:x} 0x{pages:x}',
        f'verify bad count=0x{pages:x} first=0x0')

    guest.command('off', 'off')
    code = qemu.wait(timeout=60)
    if code != 0:
      sys.exit(f'QEMU exited with {code}')
    print(f'The guest checked the shared memory and powered off after '
        f'{time.monotonic() - start:.1f}s')
    return 0
  finally:
    if qemu.poll() is None:
      qemu.kill()


if __name__ == '__main__':
  sys.exit(main())
