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

/* The bitmap that Windows' own codec makes of a PNG that the guest copied. */

#include "test.h"

#include "../displayservers/Win32/clipboard_wic.h"
#include "clipboard_png_sample.h"

#include <windows.h>
#include <objbase.h>

#include <stdlib.h>
#include <string.h>

static void testDecode(void)
{
  size_t size = 0;
  uint8_t * dib = lgClipboardDibFromImage(g_samplePng, sizeof(g_samplePng),
      &size);
  CHECK(dib);
  CHECK(size == sizeof(BITMAPV5HEADER) + sizeof(g_samplePngBgra));

  BITMAPV5HEADER header;
  memcpy(&header, dib, sizeof(header));
  CHECK(header.bV5Size == sizeof(BITMAPV5HEADER));
  CHECK(header.bV5Width == 3);
  CHECK(header.bV5Height == -2);               // from the top down
  CHECK(header.bV5Planes == 1 && header.bV5BitCount == 32);
  CHECK(header.bV5Compression == BI_BITFIELDS);
  CHECK(header.bV5AlphaMask == 0xFF000000u);
  CHECK(header.bV5RedMask == 0x00FF0000u);

  CHECK(memcmp(dib + sizeof(header), g_samplePngBgra,
        sizeof(g_samplePngBgra)) == 0);
  free(dib);
}

static void testRefused(void)
{
  size_t size = 123;

  CHECK(!lgClipboardDibFromImage(NULL, 10, &size));
  CHECK(!lgClipboardDibFromImage(g_samplePng, 0, &size));
  CHECK(!lgClipboardDibFromImage(g_samplePng, sizeof(g_samplePng), NULL));

  // not a picture
  static const uint8_t text[] = "this is not a picture of anything at all";
  CHECK(!lgClipboardDibFromImage(text, sizeof(text), &size));

  // cut off at every length: no crash, and no bitmap that has not all its
  // pixels, as there is nothing of the last row
  for (size_t length = 1; length < sizeof(g_samplePng); ++length)
  {
    uint8_t * dib = lgClipboardDibFromImage(g_samplePng, length, &size);
    if (dib)
    {
      CHECK(size == sizeof(BITMAPV5HEADER) + sizeof(g_samplePngBgra));
      free(dib);
    }
  }

  // bytes changed one at a time
  for (size_t at = 0; at < sizeof(g_samplePng); ++at)
  {
    uint8_t copy[sizeof(g_samplePng)];
    memcpy(copy, g_samplePng, sizeof(copy));
    copy[at] ^= 0xFF;
    uint8_t * dib = lgClipboardDibFromImage(copy, sizeof(copy), &size);
    free(dib);
  }
}

static void testWithCom(void)
{
  // a thread that has COM in the mode of a window, as another part of the client
  // may have it, is left as it was
  CHECK(SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)));

  size_t size;
  uint8_t * dib = lgClipboardDibFromImage(g_samplePng, sizeof(g_samplePng),
      &size);
  CHECK(dib);
  free(dib);

  // and it is still in that mode: asking for the other says so
  CHECK(CoInitializeEx(NULL, COINIT_MULTITHREADED) == RPC_E_CHANGED_MODE);
  CoUninitialize();

  // many in a row, which count what they start
  for (unsigned i = 0; i < 50; ++i)
  {
    dib = lgClipboardDibFromImage(g_samplePng, sizeof(g_samplePng), &size);
    CHECK(dib);
    free(dib);
  }
}

int main(void)
{
  testDecode();
  testRefused();
  testWithCom();
  puts("clipboard wic tests passed");
  return EXIT_SUCCESS;
}
