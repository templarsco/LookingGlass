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

"""Runs the HCS shared memory probe on a Windows CI runner and checks that it
ends cleanly: every attempt leaves no VM behind, as far as the Host Compute
Service can tell, and ends up in a well-formed report. By default the probe
stands in for the kernel, so no VM can boot. With --kernel, the VMs boot a
real kernel where the runner can run them, and the report's findings, the
guests' replies and their kernels' messages about devices are printed; what
works there is reported, not required. With --vm, the probe checks an
existing VM instead, and what the HCS let it do is printed, not required.
With --windows-disk, it boots a Windows guest from that disk, and what the
guest's IVSHMEM driver did is printed, not required."""

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


# what only identifies an attempt or repeats its setup
QUIET = {'vm_id', 'device_instance', 'config', 'serial_log', 'guest',
    'grant_kernel', 'grant_initrd', 'revoke_kernel', 'revoke_initrd',
    'cleanup_enumerate', 'still_listed', 'cleanup_verified'}

# kernel messages about the devices the guests were offered, and trouble
NOTABLE = ('hv_pci', 'pci', 'bar', 'vmbus', 'error', 'fail', 'warn', 'panic',
    'call trace')


def print_value(key, value, indent):
  if isinstance(value, (dict, list)):
    value = json.dumps(value)[:2000]
  print(f'{indent}{key}: {value}')


def check_vm(args):
  """The probe's check of an existing VM, which only the HCS's answers show."""
  result = run([args.probe, '--vm', args.vm, '--out', args.output] +
      (['--size-mib', str(args.size_mib)] if args.size_mib else []), 600)
  out = result.stdout + result.stderr
  if result.returncode == 2 and 'computecore.dll is missing' in out:
    print('This machine has no Host Compute Service, nothing more to check')
    return 0
  if result.returncode not in (0, 1):
    sys.exit(f'the probe exited with {result.returncode}')

  report = json.loads((args.output / 'report.json').read_text('utf-8'))
  if report['aborted']:
    sys.exit('the report says the probe was aborted')
  cases = {case['case']: case for case in report['cases']}
  if set(cases) != {'vm'}:
    sys.exit(f'unexpected cases: {sorted(cases)}')

  print(f'Windows {report["windows"]}, enumerate {report["enumerate"]}')
  print_value('compute_systems', report['compute_systems'], '')
  print('vm:')
  for key, value in cases['vm'].items():
    print_value(key, value, '  ')
  print('The probe ended cleanly and wrote a well-formed report')
  return 0


def check_windows(args):
  """The probe's Windows guest, which may take many minutes to set itself up
  on its first boot."""
  result = run([args.probe, '--windows-disk', args.windows_disk, '--out',
      args.output], 3600)
  out = result.stdout + result.stderr
  if result.returncode == 2 and 'computecore.dll is missing' in out:
    print('This machine has no Host Compute Service, nothing more to check')
    return 0
  if result.returncode not in (0, 1):
    sys.exit(f'the probe exited with {result.returncode}')

  report = json.loads((args.output / 'report.json').read_text('utf-8'))
  if report['aborted']:
    sys.exit('the report says the probe was aborted')
  cases = {case['case']: case for case in report['cases']}
  if set(cases) != {'windows'}:
    sys.exit(f'unexpected cases: {sorted(cases)}')

  case = cases['windows']
  if case.get('still_listed'):
    sys.exit(f'the HCS still lists VM {case["vm_id"]}')

  print(f'Windows {report["windows"]}')
  print('windows:')
  for key, value in case.items():
    if key in ('config', 'guest'):
      continue
    if key.endswith('_gpa') and isinstance(value, int):
      value = hex(value)
    print_value(key, value, '  ')
  for line in (case.get('guest') or '').splitlines()[:200]:
    print(f'  guest: {line}')
  print('The probe ended cleanly and wrote a well-formed report')
  return 0


def print_findings(report, output):
  print(f'Windows {report["windows"]}, newest configuration schema '
      f'2.{report["newest_schema_minor"]}')
  for case in report['cases']:
    print(f'\n{case["case"]}: passed {case["passed"]}')
    for attempt in case['attempts']:
      print('  attempt')
      # in the order the probe got there
      for key, value in attempt.items():
        if key in QUIET:
          continue
        if key == 'device_log':
          for line in value:
            print(f'    device: {line}')
        elif isinstance(value, (dict, list)):
          print_value(key, value, '    ')
        elif isinstance(value, int) and not isinstance(value, bool) and \
            (key.endswith('_address') or key.endswith('_gpa')):
          print(f'    {key}: {value:#x}')
        else:
          print(f'    {key}: {value}')
      for line in (attempt.get('guest') or '').splitlines()[:80]:
        print(f'    guest: {line}')

  for log in sorted(output.glob('*.serial.log')):
    lines = log.read_bytes().decode(errors='replace').splitlines()
    notable = [line for line in lines
        if any(word in line.lower() for word in NOTABLE)]
    print(f'\n{log.name}, {len(notable)} of {len(lines)} lines about '
        f'devices or trouble, the first 80:')
    for line in notable[:80]:
      print(f'  {line}')


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('probe', type=Path,
      help='path to lg-windows-client-hcs-probe.exe')
  parser.add_argument('--output', required=True, type=Path,
      help='where the probe writes its report')
  parser.add_argument('--kernel', type=Path,
      help='an x86_64 Linux kernel with Hyper-V support for the VMs to boot')
  parser.add_argument('--vm',
      help='the ID of an existing VM for the probe to check instead')
  parser.add_argument('--size-mib', type=int,
      help='the size of the shared memory for --vm')
  parser.add_argument('--windows-disk', type=Path,
      help='a disk with Windows that runs lg-hyperv-ivshmem on COM1, for '
      'the probe to boot instead')
  args = parser.parse_args()
  args.probe  = args.probe.resolve()
  args.output = args.output.resolve()
  kernel = args.kernel.resolve() if args.kernel else args.probe

  if run([args.probe, '--help'], 60).returncode != 0:
    sys.exit('--help failed')
  if run([args.probe, '--only', 'nothing'], 60).returncode != 2:
    sys.exit('an invalid option was accepted')

  args.output.parent.mkdir(parents=True, exist_ok=True)
  if args.vm:
    return check_vm(args)
  if args.windows_disk:
    args.windows_disk = args.windows_disk.resolve()
    return check_windows(args)

  # without a kernel, any existing file stands in for one and no guest can
  # answer, so a short wait does; with one, the probe waits as it does on a PC
  result = run([args.probe, '--kernel', kernel, '--out', args.output] +
      ([] if args.kernel else ['--timeout', '30']), 3000)
  out = result.stdout + result.stderr

  if result.returncode == 2 and 'computecore.dll is missing' in out:
    print('This machine has no Host Compute Service, nothing more to check')
    return 0
  if result.returncode not in (0, 1):
    sys.exit(f'the probe exited with {result.returncode}')

  check_initrd(args.output / 'initrd.cpio')
  if (args.output / 'kernel').read_bytes() != kernel.read_bytes():
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

  if args.kernel:
    print_findings(report, args.output)
    print('The probe ended cleanly and wrote a well-formed report')
  else:
    print('The probe failed cleanly and wrote a well-formed report')
  return 0


if __name__ == '__main__':
  sys.exit(main())
