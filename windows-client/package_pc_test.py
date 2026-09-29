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

"""Bundles the PC test in a folder: hcs_probe_pc.ps1 and what it runs, which
is the disk script, the HCS probe, lg-hyperv-ivshmem, the Looking Glass IDD's
driver package and the client with test capture, with the README, the GPL
and the licenses of the code built into the executables."""

import argparse
import re
import sys
from pathlib import Path

from package import SOURCE_LICENSES, TOP, read, render, text_file

# what hcs_probe_pc.ps1 needs of the IDD's package
IDD_FILES = ['LGIddInstall.exe', 'LGIdd.inf', 'LGIdd.cat', 'LGIdd.dll']


def script_file(path):
  # Windows PowerShell reads a script without a byte order mark in the
  # ANSI code page
  return b'\xef\xbb\xbf' + text_file(read(path))


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--hcs-probe', required=True,
      help='path to lg-windows-client-hcs-probe.exe')
  parser.add_argument('--ivshmem-tool', required=True,
      help='path to lg-hyperv-ivshmem.exe')
  parser.add_argument('--idd', required=True, type=Path,
      help="folder of the Looking Glass IDD's driver package")
  parser.add_argument('--commit', required=True)
  parser.add_argument('--output', required=True, type=Path)
  parser.add_argument('--notice', action='append', default=[],
      metavar='NAME=PATH',
      help='a toolchain license to ship as licenses/NAME.txt')
  args = parser.parse_args()

  if not re.fullmatch(r'[0-9a-f]{7,40}', args.commit):
    sys.exit(f'invalid commit: {args.commit}')
  missing = [name for name in IDD_FILES if not (args.idd / name).is_file()]
  if missing:
    sys.exit(f'the IDD package in {args.idd} lacks {", ".join(missing)}')
  if args.output.exists():
    sys.exit(f'{args.output} exists already')

  readme = render(read(TOP / 'windows-client/release/PC-TEST.md'), '',
      args.commit)
  files = {
    'hcs_probe_pc.ps1'               :
      script_file(TOP / 'windows-client/hcs_probe_pc.ps1'),
    'hcs_probe_windows_disk.ps1'     :
      script_file(TOP / 'windows-client/hcs_probe_windows_disk.ps1'),
    'lg-windows-client-hcs-probe.exe': Path(args.hcs_probe).read_bytes(),
    'lg-hyperv-ivshmem.exe'          : Path(args.ivshmem_tool).read_bytes(),
    'client/looking-glass-client.exe': Path(args.client).read_bytes(),
    'README.txt'                     : text_file(readme),
    'LICENSE.txt'                    : text_file(read(TOP / 'LICENSE')),
  }

  # the whole package, as its INF and catalog name its files
  for path in sorted(args.idd.iterdir()):
    if path.is_file():
      files[f'idd/{path.name}'] = path.read_bytes()

  for name, path in SOURCE_LICENSES.items():
    files['licenses/' + name] = text_file(read(TOP / path))

  for notice in args.notice:
    name, sep, path = notice.partition('=')
    if not sep or not re.fullmatch(r'[\w.-]+', name):
      sys.exit(f'invalid notice: {notice}')
    files[f'licenses/{name}.txt'] = text_file(read(path))

  for name in sorted(files):
    path = args.output / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(files[name])
    print(path)
  return 0


if __name__ == '__main__':
  sys.exit(main())
