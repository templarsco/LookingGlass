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

"""Starts the Windows client with a configuration, and reads the title of its
window, which is what an option can be seen by, to check where the client
reads its configuration from, what a file may be written in, and which of them
wins:

  user      client.ini in %APPDATA%\\looking-glass, in UTF-8 with a byte order
            mark and CR LF, as Notepad writes it, with a title that has an
            accent, a dash and a character of another script
  program   client.ini next to the program, in a copy that is carried about
  order     both: the one of the user wins over the one next to the program,
            and the command line wins over both
  file      app:configFile, with a name that has an accent
  utf16     a client.ini in UTF-16, as PowerShell 5.1 writes it: the client
            does not start, and says why

Each window is up for a second or so and does not take the focus
(win:showInactive). It needs Windows, and does not run elsewhere."""

import argparse
import ctypes
import os
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from client_startup_test import (Process, client_command,  # noqa: E402
                                 load_api, WINDOW_CLASS)

# an accent, a dash, and a character of another script
TITLES = {
  'user'   : 'Máquina Virtual – 日本',
  'program': 'Portátil – 日本',
  'line'   : 'Da linha – 日本',
  'file'   : 'Do arquivo – 日本',
}


def window_title(api, process, timeout):
  """The title of the client's window, once it is up."""
  hwnd = process.wait_window(WINDOW_CLASS, timeout)
  if hwnd is None:
    return None

  # the title is set as the window is made, and the loop that reads it is fast
  time.sleep(0.3)
  text = ctypes.create_unicode_buffer(512)
  api[1].GetWindowTextW(hwnd, text, len(text))
  return text.value


def write_ini(path, title, bom=True, newline='\r\n', encoding='utf-8'):
  path.parent.mkdir(parents=True, exist_ok=True)
  body = f'[win]{newline}title={title}{newline}'
  data = body.encode(encoding)
  if bom and encoding == 'utf-8':
    data = b'\xef\xbb\xbf' + data
  path.write_bytes(data)


def run(args, api, env, extra, exe=None):
  command = client_command(args, extra)
  if exe:
    command[0] = str(exe)
  process = Process(api, command, env, True)
  try:
    return window_title(api, process, args.timeout)
  finally:
    process.close()


def copy_program(args, folder):
  """A copy of the client, with what it needs next to it, in a folder of its
  own: the program is the same file, and its folder is another."""
  folder.mkdir(parents=True, exist_ok=True)
  source = Path(args.client)
  target = folder / source.name
  shutil.copy2(source, target)
  for name in ('opengl32.dll',):
    if (source.parent / name).exists():
      shutil.copy2(source.parent / name, folder / name)
  return target


def expect(errors, what, got, wanted):
  if got != wanted:
    errors.append(f'{what}: the title is {got!r}, expected {wanted!r}')


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--timeout', type=float, default=30,
      help='seconds to wait for each window or exit')
  parser.add_argument('--output', help='keep the folders here')
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after --')
  args = parser.parse_args()

  if sys.platform != 'win32':
    print('skipped: this test needs Windows')
    return 0

  api  = load_api()
  base = Path(args.output or tempfile.mkdtemp(prefix='lg-win-config-'))
  base = base.resolve()
  base.mkdir(parents=True, exist_ok=True)

  failed = 0

  def case(name, body):
    nonlocal failed
    errors = []
    try:
      body(errors)
    except Exception as error:  # a test that cannot run is a failure
      errors.append(f'{type(error).__name__}: {error}')
    if errors:
      failed += 1
      print(f'FAIL {name}:')
      for error in errors:
        print('  ' + error)
    else:
      print(f'pass {name}')

  def home(name):
    folder = base / name / 'home'
    folder.mkdir(parents=True, exist_ok=True)
    return dict(os.environ, APPDATA=str(folder), LOCALAPPDATA=str(folder)), folder

  def user(errors):
    env, folder = home('user')
    write_ini(folder / 'looking-glass' / 'client.ini', TITLES['user'])
    expect(errors, 'the file of the user', run(args, api, env, []),
           TITLES['user'])

  def program(errors):
    env, _ = home('program')
    exe = copy_program(args, base / 'program' / 'copy')
    write_ini(exe.parent / 'client.ini', TITLES['program'])
    expect(errors, 'the file next to the program',
           run(args, api, env, [], exe), TITLES['program'])

    # the same copy without the file has the default title
    (exe.parent / 'client.ini').unlink()
    got = run(args, api, env, [], exe)
    if got in (TITLES['program'], TITLES['user']):
      errors.append(f'the title is {got!r} without a file')

  def order(errors):
    env, folder = home('order')
    exe = copy_program(args, base / 'order' / 'copy')
    write_ini(exe.parent / 'client.ini', TITLES['program'])
    write_ini(folder / 'looking-glass' / 'client.ini', TITLES['user'])
    expect(errors, 'the user over the program', run(args, api, env, [], exe),
           TITLES['user'])
    expect(errors, 'the command line over both',
           run(args, api, env, [f'win:title={TITLES["line"]}'], exe),
           TITLES['line'])

  def named_file(errors):
    env, _ = home('file')
    path = base / 'file' / 'configuração' / 'ä.ini'
    write_ini(path, TITLES['file'])
    expect(errors, 'app:configFile',
           run(args, api, env, [f'app:configFile={path}']), TITLES['file'])

  def utf16(errors):
    env, folder = home('utf16')
    write_ini(folder / 'looking-glass' / 'client.ini', TITLES['user'],
              bom=False, encoding='utf-16')
    output = base / 'utf16' / 'output.txt'
    with open(output, 'wb') as stream:
      try:
        result = subprocess.run(client_command(args, []), env=env,
            stdout=stream, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
            timeout=args.timeout)
      except subprocess.TimeoutExpired:
        errors.append('the client did not exit')
        return
    text = output.read_text(errors='replace')
    if result.returncode == 0:
      errors.append('the client started with a file in UTF-16')
    if 'UTF-16' not in text:
      errors.append('the client does not say that the file is in UTF-16: ' +
                    text[-300:])

  case('user', user)
  case('program', program)
  case('order', order)
  case('file', named_file)
  case('utf16', utf16)
  return 1 if failed else 0


if __name__ == '__main__':
  sys.exit(main())
