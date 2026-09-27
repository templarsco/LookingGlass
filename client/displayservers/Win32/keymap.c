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

#include "keymap.h"
#include "../../src/kb.h"

#include <stdint.h>

// indexed by (extended << 8) | make code
static uint16_t scanToLinux[0x200];

void win32KeymapInit(void)
{
  for (int key = 1; key < KEY_MAX; ++key)
  {
    const uint32_t ps2 = linux_to_ps2[key];
    unsigned int index;
    if (ps2 >= 0x01 && ps2 <= 0xFF)
      index = ps2;
    else if ((ps2 & ~0xFFU) == 0xE000 && (ps2 & 0xFF))
      index = 0x100 | (ps2 & 0xFF);
    else
      continue;

    if (!scanToLinux[index])
      scanToLinux[index] = key;
  }

  /* the table sends KEY_PRINT as E0 37, but evdev reports the Print Screen
   * key as KEY_SYSRQ, which is what the other display servers produce */
  scanToLinux[0x100 | 0x37] = KEY_SYSRQ;
}

int win32KeymapToLinux(unsigned int vk, unsigned int scanCode, bool extended)
{
  /* Windows reports Pause as the plain 0x45 that the table gives Num Lock,
   * and Num Lock as an extended 0x45 */
  if (vk == WIN32_KEYMAP_VK_PAUSE)
    return KEY_PAUSE;

  if (vk == WIN32_KEYMAP_VK_NUMLOCK)
    return KEY_NUMLOCK;

  if (scanCode == 0 || scanCode > 0xFF)
    return 0;

  return scanToLinux[(extended ? 0x100 : 0) | scanCode];
}

bool win32KeymapFromLinux(int key, unsigned int * scanCode, bool * extended)
{
  if (key <= 0 || key >= KEY_MAX)
    return false;

  const uint32_t ps2 = linux_to_ps2[key];
  if (ps2 >= 0x01 && ps2 <= 0xFF)
  {
    *scanCode = ps2;
    *extended = false;
    return true;
  }

  if ((ps2 & ~0xFFU) == 0xE000 && (ps2 & 0xFF))
  {
    *scanCode = ps2 & 0xFF;
    *extended = true;
    return true;
  }

  return false;
}
