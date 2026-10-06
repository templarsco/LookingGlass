/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* Memory that a unit test hosts an LGMP session in, and the string that makes
 * the LGMP transport under test open the same memory as its lgmp:shmDevice.
 * On Linux that is a temporary file, as /dev/kvmfr0 and /dev/shm/looking-glass
 * are files. On Windows it is a named section in the user's session, as the
 * client opens a section by name. */

#ifndef LG_CLIENT_TESTS_SHM_TEST_H
#define LG_CLIENT_TESTS_SHM_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

struct TestShm
{
  void * memory;
  size_t size;
  char   device[128];
#ifdef _WIN32
  HANDLE section;
#else
  int    fd;
  bool   named;
#endif
};

/* prefix names the test, such as lgmp-frame-test */
static inline bool testShm_create(struct TestShm * shm, const char * prefix,
    size_t size)
{
  *shm = (struct TestShm) { .size = size };

#ifdef _WIN32
  static unsigned counter;
  snprintf(shm->device, sizeof(shm->device), "Local\\%s-%lu-%u", prefix,
      (unsigned long)GetCurrentProcessId(), counter++);

  wchar_t name[sizeof(shm->device)];
  for (size_t i = 0; i < sizeof(shm->device); ++i)
  {
    name[i] = (unsigned char)shm->device[i];
    if (!shm->device[i])
      break;
  }

  shm->section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
      PAGE_READWRITE, (DWORD)((unsigned long long)size >> 32),
      (DWORD)size, name);
  if (!shm->section)
    return false;

  // a section that was already there would be someone else's memory
  if (GetLastError() == ERROR_ALREADY_EXISTS)
  {
    CloseHandle(shm->section);
    shm->section = NULL;
    return false;
  }

  shm->memory = MapViewOfFile(shm->section, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!shm->memory)
  {
    CloseHandle(shm->section);
    shm->section = NULL;
    return false;
  }
  return true;
#else
  shm->fd = -1;
  snprintf(shm->device, sizeof(shm->device), "/tmp/%s-XXXXXX", prefix);
  shm->fd = mkstemp(shm->device);
  if (shm->fd < 0)
    return false;
  shm->named = true;

  if (ftruncate(shm->fd, size) != 0)
    return false;

  shm->memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
      shm->fd, 0);
  if (shm->memory == MAP_FAILED)
  {
    shm->memory = NULL;
    return false;
  }
  return true;
#endif
}

/* Removes the name once the transport has opened the memory, as a test can
 * check that the transport does not open it again. A Windows section keeps its
 * name for as long as anything holds it, so there it only releases the test's
 * claim on the name. */
static inline bool testShm_unname(struct TestShm * shm)
{
#ifndef _WIN32
  if (shm->named)
  {
    shm->named = false;
    return unlink(shm->device) == 0;
  }
#endif
  return true;
}

static inline void testShm_destroy(struct TestShm * shm)
{
  // never created
  if (!shm->size)
    return;

#ifdef _WIN32
  if (shm->memory)
    UnmapViewOfFile(shm->memory);
  if (shm->section)
    CloseHandle(shm->section);
  shm->section = NULL;
#else
  if (shm->memory)
    munmap(shm->memory, shm->size);
  if (shm->fd >= 0)
    close(shm->fd);
  testShm_unname(shm);
  shm->fd = -1;
#endif
  shm->memory = NULL;
}

#endif
