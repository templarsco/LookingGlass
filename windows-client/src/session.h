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

#ifndef _H_LG_WINDOWS_CLIENT_SESSION_
#define _H_LG_WINDOWS_CLIENT_SESSION_

#include <stdbool.h>
#include <stdint.h>

struct SessionData
{
  uint8_t  data[512];
  uint32_t size;
};

/* Builds the KVMFR session a capture host publishes, with the same layout as
 * newKVMFRData() in host/src/app.c. The strings go in the host version, the
 * VM information's capture name and CPU model, and the OS name. */
bool sessionBuild(struct SessionData * session, const char * hostver,
    const char * capture, const char * name);

#endif
