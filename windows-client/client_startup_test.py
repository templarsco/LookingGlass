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

"""Starts the Windows client the way that Explorer starts a program, with no
console and no standard handles, and the way that a test starts it, with its
output redirected, and checks where its messages go:

  log       started as Explorer does, it opens a window and writes
            %LOCALAPPDATA%\\looking-glass\\client.log, and keeps the one of the
            run before as client.log.1
  box       started as Explorer does, with an option that it refuses, it stops
            with a message box that says why, with the last lines of the log
            and the path of the log, and exits with a status that is not 0
  redirect  started with its output in a file, it writes there, opens no log
            of its own, shows no box, and exits with a status that is not 0

The box is on the desktop until the test closes it, which is a second or so,
and takes the focus. Each of the other windows is up for a second or so and
does not (win:showInactive). It needs Windows, and does not run elsewhere."""

import argparse
import ctypes
import os
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes
from pathlib import Path

WINDOW_CLASS = 'LookingGlassClient'
DIALOG_CLASS = '#32770'

STARTF_USESTDHANDLES   = 0x00000100
DETACHED_PROCESS       = 0x00000008
CREATE_UNICODE_ENVIRONMENT = 0x00000400
WM_CLOSE               = 0x0010
WM_GETTEXT             = 0x000D

# an option that the client refuses to start with
BAD_OPTION = 'app:renderer=NoSuchRenderer'


class STARTUPINFOW(ctypes.Structure):
  _fields_ = [('cb', wintypes.DWORD), ('lpReserved', wintypes.LPWSTR),
              ('lpDesktop', wintypes.LPWSTR), ('lpTitle', wintypes.LPWSTR),
              ('dwX', wintypes.DWORD), ('dwY', wintypes.DWORD),
              ('dwXSize', wintypes.DWORD), ('dwYSize', wintypes.DWORD),
              ('dwXCountChars', wintypes.DWORD),
              ('dwYCountChars', wintypes.DWORD),
              ('dwFillAttribute', wintypes.DWORD), ('dwFlags', wintypes.DWORD),
              ('wShowWindow', wintypes.WORD), ('cbReserved2', wintypes.WORD),
              ('lpReserved2', ctypes.c_void_p), ('hStdInput', wintypes.HANDLE),
              ('hStdOutput', wintypes.HANDLE), ('hStdError', wintypes.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
  _fields_ = [('hProcess', wintypes.HANDLE), ('hThread', wintypes.HANDLE),
              ('dwProcessId', wintypes.DWORD), ('dwThreadId', wintypes.DWORD)]


def load_api():
  kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
  user32   = ctypes.WinDLL('user32', use_last_error=True)

  window_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND,
      wintypes.LPARAM)

  kernel32.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR,
      ctypes.c_void_p, ctypes.c_void_p, wintypes.BOOL, wintypes.DWORD,
      ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(STARTUPINFOW),
      ctypes.POINTER(PROCESS_INFORMATION)]
  kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
  kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE,
      ctypes.POINTER(wintypes.DWORD)]
  kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
  kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

  user32.EnumWindows.argtypes = [window_proc, wintypes.LPARAM]
  user32.EnumChildWindows.argtypes = [wintypes.HWND, window_proc,
      wintypes.LPARAM]
  user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND,
      ctypes.POINTER(wintypes.DWORD)]
  user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
  user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
  user32.IsWindowVisible.argtypes = [wintypes.HWND]
  user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT,
      wintypes.WPARAM, wintypes.LPARAM]
  user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT,
      wintypes.WPARAM, ctypes.c_void_p]
  user32.SendMessageW.restype = ctypes.c_ssize_t
  return kernel32, user32, window_proc


class Process:
  """A process that the test started, with the handles that it was given."""

  def __init__(self, api, command, env, detached):
    self.kernel32, self.user32, self.window_proc = api
    self.command = command

    block = ''.join(f'{k}={v}\0' for k, v in sorted(env.items())) + '\0'
    self.env = (ctypes.c_wchar * len(block))(*block)

    # no handles at all, as Explorer starts a program, and no console
    info = STARTUPINFOW(cb=ctypes.sizeof(STARTUPINFOW),
        dwFlags=STARTF_USESTDHANDLES)
    self.info = PROCESS_INFORMATION()
    line = ctypes.create_unicode_buffer(subprocess.list2cmdline(command))
    if not self.kernel32.CreateProcessW(command[0], line, None, None, False,
        DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT, self.env, None,
        ctypes.byref(info), ctypes.byref(self.info)):
      raise OSError(ctypes.get_last_error(), 'CreateProcessW failed')
    self.pid = self.info.dwProcessId

  def windows(self, window_class):
    found = []

    def callback(hwnd, param):
      owner = wintypes.DWORD()
      self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
      if owner.value != self.pid or not self.user32.IsWindowVisible(hwnd):
        return True
      name = ctypes.create_unicode_buffer(256)
      self.user32.GetClassNameW(hwnd, name, len(name))
      if name.value == window_class:
        found.append(hwnd)
      return True

    self.user32.EnumWindows(self.window_proc(callback), 0)
    return found

  def wait_window(self, window_class, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
      found = self.windows(window_class)
      if found:
        return found[0]
      if self.exited():
        return None
      time.sleep(0.05)
    return None

  def text_of(self, hwnd):
    """The title of a window, and the text of each control in it."""
    texts = []
    title = ctypes.create_unicode_buffer(4096)
    self.user32.GetWindowTextW(hwnd, title, len(title))
    texts.append(title.value)

    def callback(child, param):
      # GetWindowText does not read the text of a control of another process
      text = ctypes.create_unicode_buffer(8192)
      self.user32.SendMessageW(child, WM_GETTEXT, len(text), text)
      if text.value:
        texts.append(text.value)
      return True

    self.user32.EnumChildWindows(hwnd, self.window_proc(callback), 0)
    return texts

  def exited(self):
    return self.kernel32.WaitForSingleObject(self.info.hProcess, 0) == 0

  def wait(self, timeout):
    """The exit code, or None if the process is still running."""
    if self.kernel32.WaitForSingleObject(self.info.hProcess,
        int(timeout * 1000)) != 0:
      return None
    code = wintypes.DWORD()
    self.kernel32.GetExitCodeProcess(self.info.hProcess, ctypes.byref(code))
    return code.value

  def kill(self):
    if not self.exited():
      self.kernel32.TerminateProcess(self.info.hProcess, 99)
      self.wait(5)

  def close(self):
    self.kill()
    self.kernel32.CloseHandle(self.info.hThread)
    self.kernel32.CloseHandle(self.info.hProcess)


def client_command(args, extra):
  return [args.client,
    'app:renderer=OpenGL',
    'win:size=256x160',
    'win:borderless=yes',
    'win:autoResize=no',
    'win:allowResize=no',
    'win:quickSplash=yes',
    'win:alerts=no',
    'win:noScreensaver=no',
    'win:showInactive=yes',
    'input:grabKeyboard=no',
    'opengl:vsync=no',
  ] + extra + args.client_args


def read(path):
  return Path(path).read_text(encoding='utf-8', errors='replace')


def test_log(args, api, env, local):
  errors = []
  folder = local / 'looking-glass'

  # twice, so that the first run's log is the one that the second keeps
  for run in (1, 2):
    process = Process(api, client_command(args, []), env, True)
    try:
      if process.wait_window(WINDOW_CLASS, args.timeout) is None:
        errors.append(f'run {run}: the client did not show a window')
        continue

      # a program of the GUI subsystem that has a console is a console program
      for console in ('ConsoleWindowClass', 'PseudoConsoleWindow'):
        if process.windows(console):
          errors.append(f'run {run}: the client has a console window')

      time.sleep(0.3)
      log = folder / 'client.log'
      if not log.exists():
        errors.append(f'run {run}: there is no {log}')
      else:
        text = read(log)
        if 'Looking Glass (' not in text or '[I]' not in text:
          errors.append(f'run {run}: the log has no Looking Glass lines:\n' +
                        text[:500])
    finally:
      process.close()

  if not (folder / 'client.log.1').exists():
    errors.append('the log of the first run was not kept as client.log.1')
  elif 'Looking Glass (' not in read(folder / 'client.log.1'):
    errors.append('client.log.1 is not the log of a run')
  return errors


def test_box(args, api, env, local):
  errors = []
  process = Process(api, client_command(args, [BAD_OPTION]), env, True)
  try:
    box = process.wait_window(DIALOG_CLASS, args.timeout)
    if box is None:
      code = process.wait(0.1)
      errors.append('the client showed no message box' +
                    (f' and exited with status {code}' if code is not None
                     else ''))
      return errors

    texts = process.text_of(box)
    (local / 'box.txt').write_text('\n--\n'.join(texts), encoding='utf-8')
    joined = '\n'.join(texts)
    for wanted in ('Looking Glass', 'stopped with an error',
                   'Invalid value provided to the option',
                   'client.log'):
      if wanted not in joined:
        errors.append(f'the box does not say "{wanted}": {texts!r}')

    process.user32.PostMessageW(box, WM_CLOSE, 0, 0)
    code = process.wait(args.timeout)
    if code is None:
      errors.append('the client did not exit when the box was closed')
    elif code == 0:
      errors.append('the client exited with status 0 after it failed')
  finally:
    process.close()

  log = local / 'looking-glass' / 'client.log'
  if not log.exists() or 'Invalid value provided to the option' not in read(log):
    errors.append('the log does not have the reason')
  return errors


def test_redirect(args, env, local):
  errors = []
  # nothing of the earlier cases is left to be mistaken for this one's
  folder = local / 'looking-glass'
  for name in ('client.log', 'client.log.1'):
    (folder / name).unlink(missing_ok=True)

  output = local / 'redirected.txt'
  with open(output, 'wb') as stream:
    try:
      result = subprocess.run(client_command(args, [BAD_OPTION]), env=env,
          stdout=stream, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
          timeout=args.timeout)
    except subprocess.TimeoutExpired:
      errors.append('the client did not exit: a box is probably waiting')
      return errors

  if result.returncode == 0:
    errors.append('the client exited with status 0 after it failed')
  if 'Invalid value provided to the option' not in read(output):
    errors.append('the redirected output does not have the reason')
  for name in ('client.log', 'client.log.1'):
    if (folder / name).exists():
      errors.append(f'the client made {name} though its output was redirected')
  return errors


def console_attached():
  """Is this process attached to a console?"""
  kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
  processes = (wintypes.DWORD * 4)()
  return kernel32.GetConsoleProcessList(processes, len(processes)) > 0


def run_detached():
  """Runs this test again in a process that has no console, and returns its
  exit code once it has printed what the other said."""
  with tempfile.TemporaryFile() as stream:
    environment = dict(os.environ, LG_STARTUP_TEST_DETACHED='1')
    result = subprocess.run([sys.executable] + sys.argv,
        creationflags=DETACHED_PROCESS, env=environment,
        stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT)
    stream.seek(0)
    sys.stdout.write(stream.read().decode('utf-8', errors='replace'))
    return result.returncode


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('client', help='path to looking-glass-client.exe')
  parser.add_argument('--timeout', type=float, default=30,
      help='seconds to wait for each window or exit')
  parser.add_argument('--output', help='keep the logs here')
  parser.add_argument('client_args', nargs='*',
      help='extra client options, after --')
  args = parser.parse_args()

  if sys.platform != 'win32':
    print('skipped: this test needs Windows')
    return 0

  # Explorer has no console, and a program that it starts has none to attach
  # to: the client attaches to the console of the process that starts it. So
  # a test that has a console runs again without one, and prints what it said
  if console_attached() and not os.environ.get('LG_STARTUP_TEST_DETACHED'):
    return run_detached()

  api = load_api()
  base = Path(args.output or tempfile.mkdtemp(prefix='lg-win-startup-'))
  base.mkdir(parents=True, exist_ok=True)

  cases = [
    ('log', lambda env, local: test_log(args, api, env, local)),
    ('box', lambda env, local: test_box(args, api, env, local)),
    ('redirect', lambda env, local: test_redirect(args, env, local)),
  ]

  failed = 0
  for name, run in cases:
    # a home of its own for each case, and none of the person who runs it
    local = base / name
    local.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, APPDATA=str(local), LOCALAPPDATA=str(local))

    errors = run(env, local)
    if errors:
      failed += 1
      print(f'FAIL {name}:')
      for error in errors:
        print('  ' + error)
    else:
      print(f'pass {name}')

  return 1 if failed else 0


if __name__ == '__main__':
  sys.exit(main())
