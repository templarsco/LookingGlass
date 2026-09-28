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

#include "session.h"

#include "common/debug.h"

#include <LGProtocol/KVMFR.h>

#include <string.h>

static bool sessionAppend(struct SessionData * session, const void * src,
    size_t size)
{
  if (size > sizeof(session->data) - session->size)
  {
    DEBUG_ERROR("Session data overflow");
    return false;
  }

  memcpy(session->data + session->size, src, size);
  session->size += (uint32_t)size;
  return true;
}

bool sessionBuild(struct SessionData * session, const char * hostver,
    const char * capture, const char * name)
{
  session->size = 0;

  KVMFR kvmfr =
  {
    .version  = KVMFR_VERSION,
    .features = 0
  };
  if (strlen(hostver) >= sizeof(kvmfr.hostver))
  {
    DEBUG_ERROR("The host version does not fit in KVMFR.hostver");
    return false;
  }
  memcpy(kvmfr.magic, KVMFR_MAGIC, sizeof(kvmfr.magic));
  strcpy(kvmfr.hostver, hostver);
  if (!sessionAppend(session, &kvmfr, sizeof(kvmfr)))
    return false;

  const size_t nameSize = strlen(name) + 1;

  {
    KVMFRRecord_VMInfo vmInfo =
    {
      .cpus    = 1,
      .cores   = 1,
      .sockets = 1
    };
    if (strlen(capture) >= sizeof(vmInfo.capture))
    {
      DEBUG_ERROR("The capture name does not fit in KVMFRRecord_VMInfo");
      return false;
    }
    strcpy(vmInfo.capture, capture);

    const KVMFRRecord record =
    {
      .type = KVMFR_RECORD_VMINFO,
      .size = sizeof(vmInfo) + nameSize
    };

    if (!sessionAppend(session, &record, sizeof(record)) ||
        !sessionAppend(session, &vmInfo, sizeof(vmInfo)) ||
        !sessionAppend(session, name   , nameSize      ))
      return false;
  }

  {
    KVMFRRecord_OSInfo osInfo =
    {
#ifdef _WIN32
      .os = KVMFR_OS_WINDOWS
#else
      .os = KVMFR_OS_LINUX
#endif
    };

    const KVMFRRecord record =
    {
      .type = KVMFR_RECORD_OSINFO,
      .size = sizeof(osInfo) + nameSize
    };

    if (!sessionAppend(session, &record, sizeof(record)) ||
        !sessionAppend(session, &osInfo, sizeof(osInfo)) ||
        !sessionAppend(session, name   , nameSize      ))
      return false;
  }

  return true;
}
