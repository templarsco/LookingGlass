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

"""Reads the Windows client's executable, which is a file and needs no
Windows to read, and checks what a PC that is not this one needs from it:

  imports     every DLL it loads is one that Windows has, so that it runs on a
              PC without MSYS2, MinGW or a Visual C++ redistributable, and
              does not ask for the network, which would make the firewall ask
              the user a question
  manifest    UTF-8 as the code page, per-monitor DPI awareness version 2,
              Windows 10 and 11, and no request to run as an administrator
  version     a version resource whose language and code page are those of
              its strings, so that Explorer and installers can read them
  security    ASLR with high entropy, DEP, and a subsystem that is the one
              that is expected

It reads the program; it does not run it, and says nothing of how it behaves
on a PC. Run it on the build of any host:

  client_binary_test.py looking-glass-client.exe [--subsystem gui|console]"""

import argparse
import re
import struct
import sys

# the DLLs that Windows 10 and 11 have, which the client may import: the
# system's own, and none of the toolchain's. dbghelp, dwmapi, dwrite and
# opengl32 are not in the list of DLLs that Windows loads only from the system
# folder, which is what a program ought to know of them
SYSTEM_DLLS = {
  'advapi32.dll', 'bcrypt.dll', 'comctl32.dll', 'comdlg32.dll',
  'dbghelp.dll', 'dwmapi.dll', 'dwrite.dll', 'gdi32.dll', 'imm32.dll',
  'kernel32.dll', 'msvcrt.dll', 'ntdll.dll', 'ole32.dll', 'oleaut32.dll',
  'opengl32.dll', 'setupapi.dll', 'shell32.dll', 'shlwapi.dll', 'user32.dll',
  'version.dll', 'winmm.dll', 'cfgmgr32.dll', 'shcore.dll', 'uxtheme.dll',
}

# what no build of the client has any use for, and would hide a toolchain
# runtime that was linked as a DLL
FORBIDDEN_DLLS = {
  'ws2_32.dll': 'a network library: the firewall would ask the user',
  'wsock32.dll': 'a network library: the firewall would ask the user',
  'mswsock.dll': 'a network library: the firewall would ask the user',
  'winhttp.dll': 'a network library: the firewall would ask the user',
  'wininet.dll': 'a network library: the firewall would ask the user',
}

SUBSYSTEMS = {2: 'gui', 3: 'console'}

IMAGE_DLLCHAR_HIGH_ENTROPY_VA = 0x0020
IMAGE_DLLCHAR_DYNAMIC_BASE    = 0x0040
IMAGE_DLLCHAR_NX_COMPAT       = 0x0100
IMAGE_FILE_RELOCS_STRIPPED    = 0x0001

RT_VERSION  = 16
RT_MANIFEST = 24

WINDOWS_10 = '{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}'


class Pe:
  def __init__(self, data):
    self.data = data
    if data[:2] != b'MZ':
      raise ValueError('not a PE file')
    self.pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[self.pe:self.pe + 4] != b'PE\0\0':
      raise ValueError('no PE signature')

    (self.machine, count, _, _, _, optional_size, self.characteristics) = \
        struct.unpack_from('<HHIIIHH', data, self.pe + 4)
    self.optional = self.pe + 24
    self.magic = struct.unpack_from('<H', data, self.optional)[0]
    if self.magic != 0x20b:
      raise ValueError('not a 64 bit PE file (magic 0x%x)' % self.magic)

    self.subsystem, self.dll_characteristics = \
        struct.unpack_from('<HH', data, self.optional + 68)
    directories = struct.unpack_from('<I', data, self.optional + 108)[0]
    self.directories = [
      struct.unpack_from('<II', data, self.optional + 112 + 8 * i)
      for i in range(directories)]

    table = self.optional + optional_size
    self.sections = []
    for i in range(count):
      name, vsize, vaddr, rawsize, rawptr = struct.unpack_from(
          '<8sIIII', data, table + 40 * i)
      self.sections.append((name.rstrip(b'\0').decode('ascii', 'replace'),
                            vsize, vaddr, rawsize, rawptr))

  def offset(self, rva):
    for _, vsize, vaddr, rawsize, rawptr in self.sections:
      if vaddr <= rva < vaddr + max(vsize, rawsize):
        return rawptr + rva - vaddr
    raise ValueError('RVA 0x%x is in no section' % rva)

  def cstring(self, rva):
    start = self.offset(rva)
    return self.data[start:self.data.index(b'\0', start)].decode('ascii')

  def directory(self, index):
    if index >= len(self.directories):
      return 0, 0
    return self.directories[index]

  def imports(self):
    """The names of the DLLs that are loaded when the program starts."""
    names = []
    rva, size = self.directory(1)
    if rva:
      at = self.offset(rva)
      while True:
        fields = struct.unpack_from('<IIIII', self.data, at)
        if not any(fields):
          break
        names.append(self.cstring(fields[3]))
        at += 20
    return names

  def delay_imports(self):
    """The names of the DLLs that are loaded the first time that they are
    used."""
    names = []
    rva, size = self.directory(13)
    if rva:
      at = self.offset(rva)
      while True:
        fields = struct.unpack_from('<8I', self.data, at)
        if not any(fields):
          break
        names.append(self.cstring(fields[1]))
        at += 32
    return names

  def resources(self, kind):
    """The data of every resource of a type, in every language."""
    rva, size = self.directory(2)
    if not rva:
      return []
    base = self.offset(rva)

    def entries(at):
      named, ids = struct.unpack_from('<HH', self.data, at + 12)
      for i in range(named + ids):
        name, target = struct.unpack_from('<II', self.data, at + 16 + 8 * i)
        yield name, target & 0x7fffffff, bool(target & 0x80000000)

    found = []

    def leaves(at):
      for _, target, directory in entries(at):
        if directory:
          yield from leaves(base + target)
        else:
          data_rva, length, _, _ = struct.unpack_from(
              '<IIII', self.data, base + target)
          start = self.offset(data_rva)
          yield self.data[start:start + length]

    for name, target, directory in entries(base):
      if name == kind and directory:
        found.extend(leaves(base + target))
    return found


def version_nodes(data, at, end):
  """The children of a version resource's node, as (key, value, children)
  with the value as the bytes it has and the children as another list."""
  nodes = []
  while at + 6 <= end:
    length, value_length, kind = struct.unpack_from('<HHH', data, at)
    if length == 0:
      break
    key_start = at + 6
    key_end   = key_start
    while data[key_end:key_end + 2] != b'\0\0':
      key_end += 2
    key = data[key_start:key_end].decode('utf-16-le')

    value_at = (key_end + 2 + 3) & ~3
    size = value_length * (2 if kind == 1 else 1)
    value = data[value_at:value_at + size]
    child_at = (value_at + size + 3) & ~3
    children = version_nodes(data, child_at, at + length)
    nodes.append((key, value, children))
    at = (at + length + 3) & ~3
  return nodes


def check_imports(pe, errors, notes):
  imports = [name.lower() for name in pe.imports()]
  delayed = [name.lower() for name in pe.delay_imports()]
  notes.append('imports: ' + ', '.join(sorted(set(imports))))
  if delayed:
    notes.append('delay imports: ' + ', '.join(sorted(set(delayed))))

  for name in sorted(set(imports + delayed)):
    if name in FORBIDDEN_DLLS:
      errors.append('it imports %s, %s' % (name, FORBIDDEN_DLLS[name]))
    elif name.startswith('api-ms-win-'):
      continue
    elif name not in SYSTEM_DLLS:
      errors.append('it imports %s, which a PC may not have: not one of the '
                    'system DLLs that the client is known to need' % name)


def check_manifest(pe, errors, notes):
  manifests = pe.resources(RT_MANIFEST)
  if len(manifests) != 1:
    errors.append('expected one manifest, found %d' % len(manifests))
    return
  text = manifests[0].decode('utf-8', 'replace')

  def has(pattern):
    return re.search(pattern, text, re.S) is not None

  if not has(r'<activeCodePage[^>]*>\s*UTF-8\s*</activeCodePage>'):
    errors.append('the manifest does not ask for UTF-8 as the code page')
  if not has(r'<dpiAwareness[^>]*>\s*PerMonitorV2'):
    errors.append('the manifest does not ask for per-monitor DPI awareness '
                  'version 2')
  if WINDOWS_10 not in text.lower():
    errors.append('the manifest does not list Windows 10 and 11 as supported')
  if not has(r'requestedExecutionLevel[^>]*level="asInvoker"'):
    errors.append('the manifest does not run the program as the user who '
                  'starts it')
  if has(r'level="(requireAdministrator|highestAvailable)"'):
    errors.append('the manifest asks to run as an administrator')
  notes.append('manifest: UTF-8, PerMonitorV2, asInvoker')


def check_version(pe, errors, notes):
  resources = pe.resources(RT_VERSION)
  if len(resources) != 1:
    errors.append('expected one version resource, found %d' % len(resources))
    return

  data  = resources[0]
  roots = version_nodes(data, 0, len(data))
  if not roots or roots[0][0] != 'VS_VERSION_INFO':
    errors.append('the version resource does not start with VS_VERSION_INFO')
    return

  blocks       = {}
  translations = []
  for key, value, children in roots[0][2]:
    if key == 'StringFileInfo':
      for block, _, strings in children:
        blocks[block.upper()] = {
          name: text.decode('utf-16-le').rstrip('\0')
          for name, text, _ in strings}
    elif key == 'VarFileInfo':
      for name, text, _ in children:
        if name == 'Translation':
          for i in range(0, len(text), 4):
            language, codepage = struct.unpack_from('<HH', text, i)
            translations.append('%04X%04X' % (language, codepage))

  if not blocks:
    errors.append('the version resource has no strings')
  if not translations:
    errors.append('the version resource lists no translation')
  for translation in translations:
    if translation not in blocks:
      errors.append('the version resource lists the translation %s, and its '
                    'strings are in %s' % (translation,
                    ', '.join(sorted(blocks)) or 'no block'))

  for block, strings in blocks.items():
    for name in ('FileDescription', 'FileVersion', 'ProductName',
                 'OriginalFilename'):
      if not strings.get(name):
        errors.append('the version block %s has no %s' % (block, name))
  if blocks:
    first = next(iter(blocks.values()))
    notes.append('version: %s %s' % (first.get('FileDescription', '?'),
                                     first.get('FileVersion', '?')))


def check_security(pe, expected, errors, notes):
  name = SUBSYSTEMS.get(pe.subsystem, 'subsystem %d' % pe.subsystem)
  if name != expected:
    errors.append('the subsystem is %s, expected %s' % (name, expected))
  notes.append('subsystem: ' + name)

  flags = pe.dll_characteristics
  if not flags & IMAGE_DLLCHAR_DYNAMIC_BASE:
    errors.append('the executable has no ASLR (DYNAMIC_BASE)')
  elif pe.characteristics & IMAGE_FILE_RELOCS_STRIPPED:
    errors.append('the executable asks for ASLR but has no relocations')
  if not flags & IMAGE_DLLCHAR_HIGH_ENTROPY_VA:
    errors.append('the executable has no high entropy ASLR (HIGH_ENTROPY_VA)')
  if not flags & IMAGE_DLLCHAR_NX_COMPAT:
    errors.append('the executable has no DEP (NX_COMPAT)')


def main():
  parser = argparse.ArgumentParser(description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('exe', help='path to looking-glass-client.exe')
  parser.add_argument('--subsystem', choices=('gui', 'console'),
      default='console', help='the subsystem that it is built for')
  args = parser.parse_args()

  with open(args.exe, 'rb') as exe:
    data = exe.read()

  try:
    pe = Pe(data)
  except (ValueError, struct.error) as error:
    print('FAIL %s: %s' % (args.exe, error))
    return 1

  errors, notes = [], []
  for check in (check_imports, check_manifest, check_version):
    try:
      check(pe, errors, notes)
    except (ValueError, struct.error, IndexError) as error:
      errors.append('%s: could not be read: %s' % (check.__name__, error))
  check_security(pe, args.subsystem, errors, notes)

  for note in notes:
    print(note)
  if errors:
    for error in errors:
      print('FAIL ' + error)
    return 1

  print('pass: %s needs only what Windows has, and declares what it should' %
        args.exe)
  return 0


if __name__ == '__main__':
  sys.exit(main())
