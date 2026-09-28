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

"""Runs the HCS shared memory probe on a Windows machine that cannot boot its
VMs, such as a CI runner without Hyper-V, and checks that it fails cleanly:
every attempt leaves no VM behind, as far as the Host Compute Service can
tell, and ends up in a well-formed report."""

import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path


def run(args, timeout):
  result = subprocess.run(args, capture_output=True, text=True,
      timeout=timeout)
  print(f'$ {" ".join(str(a) for a in args)}  (exit {result.returncode})')
  print(result.stdout + result.stderr)
  return result


def check_initrd(path):
  """The newc archive the probe boots: the directories, the console and the
  init, an x86_64 Linux executable."""
  data = path.read_bytes()
  entries, pos = {}, 0
  while True:
    if data[pos:pos + 6] != b'070701':
      sys.exit(f'{path} is not a newc archive at byte {pos}')
    fields = [int(data[pos + 6 + 8 * i:pos + 14 + 8 * i], 16)
        for i in range(13)]
    name = data[pos + 110:pos + 110 + fields[11] - 1].decode()
    pos += 110 + fields[11]
    pos += -pos % 4
    entries[name] = (fields[1], data[pos:pos + fields[6]])
    pos += fields[6]
    pos += -pos % 4
    if name == 'TRAILER!!!':
      break

  expected = {'dev': 0o040755, 'dev/console': 0o020600, 'proc': 0o040555,
      'sys': 0o040555, 'init': 0o100755, 'TRAILER!!!': 0}
  if {name: mode for name, (mode, _) in entries.items()} != expected:
    sys.exit(f'unexpected initrd entries: {entries.keys()}')
  init = entries['init'][1]
  if init[:4] != b'\x7fELF' or init[4] != 2 or \
      struct.unpack_from('<H', init, 18)[0] != 62:
    sys.exit('the init is not an x86_64 ELF executable')
  print(f'{path.name}: {len(entries) - 1} entries, a {len(init)} byte init')


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('probe', type=Path,
      help='path to lg-windows-client-hcs-probe.exe')
  parser.add_argument('--output', required=True, type=Path,
      help='where the probe writes its report')
  args = parser.parse_args()
  args.probe  = args.probe.resolve()
  args.output = args.output.resolve()

  if run([args.probe, '--help'], 60).returncode != 0:
    sys.exit('--help failed')
  if run([args.probe, '--only', 'nothing'], 60).returncode != 2:
    sys.exit('an invalid option was accepted')

  # any existing file stands in for the kernel, the VMs cannot boot here
  args.output.parent.mkdir(parents=True, exist_ok=True)
  result = run([args.probe, '--kernel', args.probe, '--out', args.output,
      '--timeout', '30'], 900)
  out = result.stdout + result.stderr

  if result.returncode == 2 and 'computecore.dll is missing' in out:
    print('This machine has no Host Compute Service, nothing more to check')
    return 0
  if result.returncode not in (0, 1):
    sys.exit(f'the probe exited with {result.returncode}')

  check_initrd(args.output / 'initrd.cpio')
  if (args.output / 'kernel').read_bytes() != args.probe.read_bytes():
    sys.exit('the VMs do not boot a copy of the given kernel')
  report = json.loads((args.output / 'report.json').read_text('utf-8'))
  if report['aborted']:
    sys.exit('the report says the probe was aborted')

  # without a reachable HCS, a VM that was never created cannot be looked up
  reachable = report['enumerate'] == '0x00000000'
  print(f'The HCS is {"" if reachable else "not "}reachable: enumerate '
      f'{report["enumerate"]}')

  cases = {case['case']: case for case in report['cases']}
  if set(cases) != {'hdv', 'shm'}:
    sys.exit(f'unexpected cases: {sorted(cases)}')
  if cases['hdv']['available'] == (not cases['hdv']['attempts']):
    sys.exit('the hdv case ran while its API was missing, or the reverse')
  if not cases['shm']['attempts']:
    sys.exit('the shm case made no attempt')

  for name, case in cases.items():
    for attempt in case['attempts']:
      print(f'{name}: create {attempt["create"]}, start '
          f'{attempt.get("start", "not reached")}, passed '
          f'{attempt["passed"]}, cleanup enumerate '
          f'{attempt["cleanup_enumerate"]}')
      if attempt['still_listed']:
        sys.exit(f'{name}: the HCS still lists VM {attempt["vm_id"]}')
      if reachable and not attempt['cleanup_verified']:
        sys.exit(f'{name}: no check that VM {attempt["vm_id"]} is gone')

  print('The probe failed cleanly and wrote a well-formed report')
  return 0


if __name__ == '__main__':
  sys.exit(main())
