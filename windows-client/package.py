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

"""Packages the Windows client for a release: a zip with the executable, the
README, the GPL and the licenses of the code built into the executable, and
the release notes rendered from the same README."""

import argparse
import os
import re
import sys
import time
import zipfile
from pathlib import Path

TOP = Path(__file__).resolve().parents[1]

# the libraries from the source tree that the client links statically
SOURCE_LICENSES = {
  'cimgui.txt' : 'repos/gui/cimgui/LICENSE',
  'imgui.txt'  : 'repos/gui/cimgui/imgui/LICENSE.txt',
  'cimplot.txt': 'repos/gui/cimplot/LICENSE',
  'implot.txt' : 'repos/gui/cimplot/implot/LICENSE',
  'nanosvg.txt': 'repos/nanosvg/LICENSE.txt',
}


def render(template, version, commit):
  text = template.replace('@VERSION@', version).replace('@COMMIT@', commit)
  left = re.findall(r'@[A-Z]+@', text)
  if left:
    sys.exit(f'unknown placeholders in the README: {", ".join(left)}')
  return text


def read(path):
  return Path(path).read_text(encoding='utf-8')


def text_file(text):
  # Notepad before Windows 10 1809 only understands CRLF line endings
  return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8')


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--version', required=True)
  parser.add_argument('--commit', required=True)
  parser.add_argument('--output', required=True, type=Path)
  parser.add_argument('--notice', action='append', default=[],
      metavar='NAME=PATH',
      help='a toolchain license to ship as licenses/NAME.txt')
  args = parser.parse_args()

  if not re.fullmatch(r'[0-9A-Za-z][0-9A-Za-z._-]*', args.version):
    sys.exit(f'invalid version: {args.version}')

  readme = render(read(TOP / 'windows-client/release/README.md'),
      args.version, args.commit)

  files = {
    'looking-glass-client.exe': Path(args.client).read_bytes(),
    'README.txt'              : text_file(readme),
    'LICENSE.txt'             : text_file(read(TOP / 'LICENSE')),
  }

  for name, path in SOURCE_LICENSES.items():
    files['licenses/' + name] = text_file(read(TOP / path))

  for notice in args.notice:
    name, sep, path = notice.partition('=')
    if not sep or not re.fullmatch(r'[\w.-]+', name):
      sys.exit(f'invalid notice: {notice}')
    files[f'licenses/{name}.txt'] = text_file(read(path))

  # the files get the commit time, which keeps the archive reproducible
  stamp = time.gmtime(int(os.environ.get('SOURCE_DATE_EPOCH',
      time.time())))[:6]

  args.output.mkdir(parents=True, exist_ok=True)
  base = f'looking-glass-client-{args.version}-windows-x64'
  archive = args.output / f'{base}.zip'
  with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zf:
    for name in sorted(files):
      info = zipfile.ZipInfo(f'{base}/{name}', stamp)
      info.compress_type  = zipfile.ZIP_DEFLATED
      info.external_attr  = 0o644 << 16
      zf.writestr(info, files[name])

  (args.output / 'release-notes.md').write_text(readme, encoding='utf-8')
  print(archive)
  return 0


if __name__ == '__main__':
  sys.exit(main())
