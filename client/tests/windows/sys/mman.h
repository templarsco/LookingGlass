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

/* Windows has no sys/mman.h. This is enough mmap for the unit tests that host
 * an LGMP session in anonymous memory. Memory that the client opens by name
 * goes through shm_test.h instead. */

#ifndef LG_CLIENT_TESTS_WINDOWS_SYS_MMAN_H
#define LG_CLIENT_TESTS_WINDOWS_SYS_MMAN_H

#ifndef _WIN32
#error "this is the Windows replacement for sys/mman.h"
#endif

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <windows.h>

#define PROT_NONE     0
#define PROT_READ     1
#define PROT_WRITE    2

#define MAP_SHARED    1
#define MAP_PRIVATE   2
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED    ((void *)-1)

static inline void * mmap(void * addr, size_t length, int prot, int flags,
    int fd, off_t offset)
{
  if (addr || fd != -1 || !(flags & MAP_ANONYMOUS))
  {
    errno = ENOSYS;
    return MAP_FAILED;
  }

  void * memory = VirtualAlloc(NULL, length, MEM_RESERVE | MEM_COMMIT,
      PAGE_READWRITE);
  if (!memory)
  {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  return memory;
}

static inline int munmap(void * addr, size_t length)
{
  if (!VirtualFree(addr, 0, MEM_RELEASE))
  {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

#endif
