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

/* The conversions between what the Windows clipboard holds and what is sent to
 * the guest, with the bytes of text and of bitmaps that a guest, which is not
 * to be trusted, can send. */

#include "test.h"

#include "../displayservers/Win32/clipboard_format.h"

#include <stdlib.h>
#include <string.h>

/* text */

static void expectUtf8(const uint16_t * text, size_t count,
    const char * expected)
{
  size_t size = 123456;
  uint8_t * out = lgClipboardTextFromWindows(text, count, &size);
  CHECK(out);
  CHECK(size == strlen(expected));
  CHECK(memcmp(out, expected, size) == 0);
  CHECK(out[size] == '\0');
  free(out);
}

#define U16(...) (const uint16_t[]) { __VA_ARGS__ }

static void testFromWindows(void)
{
  expectUtf8(U16('h', 'e', 'l', 'l', 'o'), 5, "hello");
  expectUtf8(U16('a', '\r', '\n', 'b'), 4, "a\nb");
  expectUtf8(U16('a', '\r', '\n', '\r', '\n', 'b'), 6, "a\n\nb");
  // a CR that is not at the end of a line, and an LF alone, stay
  expectUtf8(U16('a', '\r', 'b'), 3, "a\rb");
  expectUtf8(U16('a', '\n', 'b'), 3, "a\nb");
  expectUtf8(U16('a', '\r'), 2, "a\r");

  // an accent, a dash, a character of another script, and one of a pair
  expectUtf8(U16('M', 0x00E1, 'q'), 3, "M\xc3\xa1q");
  expectUtf8(U16(0x2013), 1, "\xe2\x80\x93");
  expectUtf8(U16(0x65E5, 0x672C), 2, "\xe6\x97\xa5\xe6\x9c\xac");
  expectUtf8(U16(0xD83D, 0xDDA5, ' ', 'd'), 4, "\xf0\x9f\x96\xa5 d");

  // what is not a pair is a replacement character
  expectUtf8(U16('a', 0xD800, 'b'), 3, "a\xef\xbf\xbd" "b");
  expectUtf8(U16('a', 0xDC00, 'b'), 3, "a\xef\xbf\xbd" "b");
  expectUtf8(U16(0xD800), 1, "\xef\xbf\xbd");
  expectUtf8(U16(0xD800, 0xD800, 0xDC00), 3, "\xef\xbf\xbd\xf0\x90\x80\x80");

  // it ends at a NUL, and at the count
  expectUtf8(U16('a', 0, 'b'), 3, "a");
  expectUtf8(U16('a', 'b', 'c'), 2, "ab");
  expectUtf8(U16(0), 1, "");
  expectUtf8(U16('x'), 0, "");

  size_t size;
  CHECK(!lgClipboardTextFromWindows(NULL, 1, &size));
  CHECK(!lgClipboardTextFromWindows(U16('a'), 1, NULL));
}

static void expectUnits(const uint8_t * text, size_t size,
    const uint16_t * expected, size_t count)
{
  size_t units = 0;
  uint16_t * out = lgClipboardTextToWindows(text, size, &units);
  CHECK(out);
  CHECK(units == count + 1);
  CHECK(memcmp(out, expected, count * sizeof(*out)) == 0);
  CHECK(out[count] == 0);
  free(out);
}

#define BYTES(...) (const uint8_t[]) { __VA_ARGS__ }

static void testToWindows(void)
{
  expectUnits(BYTES('h', 'e', 'l', 'l', 'o'), 5,
      U16('h', 'e', 'l', 'l', 'o'), 5);

  // an end of line is a CR and an LF, once
  expectUnits(BYTES('a', '\n', 'b'), 3, U16('a', '\r', '\n', 'b'), 4);
  expectUnits(BYTES('a', '\r', '\n', 'b'), 4, U16('a', '\r', '\n', 'b'), 4);
  expectUnits(BYTES('a', '\n', '\n', 'b'), 4,
      U16('a', '\r', '\n', '\r', '\n', 'b'), 6);
  expectUnits(BYTES('\n'), 1, U16('\r', '\n'), 2);
  expectUnits(BYTES('a', '\r', 'b'), 3, U16('a', '\r', 'b'), 3);

  expectUnits(BYTES('M', 0xC3, 0xA1), 3, U16('M', 0x00E1), 2);
  expectUnits(BYTES(0xE2, 0x80, 0x93), 3, U16(0x2013), 1);
  expectUnits(BYTES(0xF0, 0x9F, 0x96, 0xA5), 4, U16(0xD83D, 0xDDA5), 2);
  expectUnits(BYTES(0xF4, 0x8F, 0xBF, 0xBF), 4, U16(0xDBFF, 0xDFFF), 2);

  // what is not a character is a replacement character for each byte
  expectUnits(BYTES(0x80), 1, U16(0xFFFD), 1);
  expectUnits(BYTES('a', 0xFF, 'b'), 3, U16('a', 0xFFFD, 'b'), 3);
  expectUnits(BYTES(0xC0, 0x80), 2, U16(0xFFFD, 0xFFFD), 2);       // overlong
  expectUnits(BYTES(0xC1, 0xBF), 2, U16(0xFFFD, 0xFFFD), 2);
  expectUnits(BYTES(0xE0, 0x80, 0x80), 3, U16(0xFFFD, 0xFFFD, 0xFFFD), 3);
  expectUnits(BYTES(0xED, 0xA0, 0x80), 3, U16(0xFFFD, 0xFFFD, 0xFFFD), 3);
  expectUnits(BYTES(0xF4, 0x90, 0x80, 0x80), 4,
      U16(0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD), 4);                    // too large
  expectUnits(BYTES(0xE6, 0x97), 2, U16(0xFFFD, 0xFFFD), 2);      // cut short
  expectUnits(BYTES(0xE6, 0x97, 'a'), 3, U16(0xFFFD, 0xFFFD, 'a'), 3);

  // it ends at a NUL, and at the size
  expectUnits(BYTES('a', 0, 'b'), 3, U16('a'), 1);
  expectUnits(BYTES('a', 'b', 'c'), 2, U16('a', 'b'), 2);
  expectUnits(BYTES(0), 1, U16(0), 0);
  expectUnits(BYTES('x'), 0, U16(0), 0);
}

static uint64_t rng = UINT64_C(0x2545f4914f6cdd1d);

static uint64_t next(void)
{
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

static void testTextRoundTrip(void)
{
  // text with the ends of line that Windows has comes back as it was
  static const uint32_t points[] =
  {
    'a', 'Z', '0', ' ', '\t', 0x00E9, 0x00FF, 0x0100, 0x07FF, 0x0800, 0x2013,
    0x65E5, 0xD7FF, 0xE000, 0xFFFD, 0x10000, 0x1F5A5, 0x10FFFF,
  };

  for (unsigned round = 0; round < 2000; ++round)
  {
    uint16_t text[64];
    size_t n = 0;
    const unsigned length = next() % 20;
    for (unsigned i = 0; i < length; ++i)
    {
      if (next() % 5 == 0)
      {
        text[n++] = '\r';
        text[n++] = '\n';
        continue;
      }

      uint32_t c = points[next() % (sizeof(points) / sizeof(*points))];
      if (c >= 0x10000)
      {
        c -= 0x10000;
        text[n++] = (uint16_t)(0xD800 + (c >> 10));
        text[n++] = (uint16_t)(0xDC00 + (c & 0x3FF));
      }
      else
        text[n++] = (uint16_t)c;
    }

    size_t size;
    uint8_t * wire = lgClipboardTextFromWindows(text, n, &size);
    CHECK(wire);

    size_t units;
    uint16_t * back = lgClipboardTextToWindows(wire, size, &units);
    CHECK(back);
    CHECK(units == n + 1);
    CHECK(memcmp(back, text, n * sizeof(*text)) == 0);
    free(back);
    free(wire);
  }
}

// UTF-8 as the standard has it: no overlong form, no surrogate, nothing
// above U+10FFFF
static bool validUtf8(const uint8_t * p, size_t n)
{
  for (size_t i = 0; i < n;)
  {
    const uint8_t b = p[i];
    size_t need;
    uint8_t low = 0x80, high = 0xBF;
    if (b < 0x80)
    {
      ++i;
      continue;
    }
    else if (b >= 0xC2 && b <= 0xDF)
      need = 1;
    else if (b >= 0xE0 && b <= 0xEF)
    {
      need = 2;
      if (b == 0xE0) low = 0xA0;
      if (b == 0xED) high = 0x9F;
    }
    else if (b >= 0xF0 && b <= 0xF4)
    {
      need = 3;
      if (b == 0xF0) low = 0x90;
      if (b == 0xF4) high = 0x8F;
    }
    else
      return false;

    if (i + need >= n)
      return false;
    for (size_t k = 1; k <= need; ++k)
      if (p[i + k] < (k == 1 ? low : 0x80) || p[i + k] > (k == 1 ? high : 0xBF))
        return false;
    i += need + 1;
  }
  return true;
}

static void testTextNoCrash(void)
{
  // bytes of any kind come back as text that Windows can have: pairs of
  // surrogates only, a CR with every LF, a NUL at the end. What that goes back
  // to is text that UTF-8 can have, with no CR before an LF
  for (unsigned round = 0; round < 20000; ++round)
  {
    uint8_t bytes[48];
    const size_t length = next() % sizeof(bytes);
    for (size_t i = 0; i < length; ++i)
      bytes[i] = (uint8_t)next();

    size_t units;
    uint16_t * wide = lgClipboardTextToWindows(bytes, length, &units);
    CHECK(wide);
    CHECK(units >= 1 && wide[units - 1] == 0);

    for (size_t i = 0; i + 1 < units; ++i)
    {
      CHECK(wide[i] != 0);
      if (wide[i] >= 0xD800 && wide[i] <= 0xDBFF)
      {
        CHECK(i + 2 < units);
        CHECK(wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF);
        ++i;
      }
      else
        CHECK(wide[i] < 0xDC00 || wide[i] > 0xDFFF);

      if (wide[i] == '\n')
        CHECK(i > 0 && wide[i - 1] == '\r');
    }

    size_t size;
    uint8_t * wire = lgClipboardTextFromWindows(wide, units, &size);
    CHECK(wire);
    CHECK(validUtf8(wire, size));
    for (size_t i = 0; i + 1 < size; ++i)
      CHECK(!(wire[i] == '\r' && wire[i + 1] == '\n'));
    free(wire);
    free(wide);
  }
}

/* bitmaps */

static void put16(uint8_t * p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t * p, uint32_t v)
{
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t get32(const uint8_t * p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
    ((uint32_t)p[3] << 24);
}

struct Dib
{
  uint8_t * data;
  size_t    size;
};

/* a bitmap with a header of headerSize bytes, which is a version of it, and
 * masks and palette as the header's compression and colors say, and pixels
 * that count up from 1 */
static struct Dib makeDib(uint32_t headerSize, int32_t width, int32_t height,
    uint16_t bits, uint32_t compression, uint32_t colors)
{
  size_t table = 0;
  if (headerSize == 40 && compression == 3)
    table += 12;
  if (headerSize == 40 && compression == 6)
    table += 16;
  size_t palette = colors ? colors : (bits <= 8 ? (size_t)1 << bits : 0);
  table += palette * 4;

  const size_t stride = (((size_t)width * bits + 31) / 32) * 4;
  const size_t rows   = height < 0 ? (size_t)-height : (size_t)height;
  const size_t size   = headerSize + table + stride * rows;

  struct Dib dib = { calloc(1, size), size };
  CHECK(dib.data);
  put32(dib.data, headerSize);
  put32(dib.data + 4, (uint32_t)width);
  put32(dib.data + 8, (uint32_t)height);
  put16(dib.data + 12, 1);
  put16(dib.data + 14, bits);
  put32(dib.data + 16, compression);
  put32(dib.data + 32, colors);
  for (size_t i = headerSize; i < size; ++i)
    dib.data[i] = (uint8_t)(i - headerSize + 1);
  return dib;
}

static void expectRoundTrip(struct Dib dib, uint32_t offsetOfPixels, bool v5)
{
  size_t bmpSize;
  uint8_t * bmp = lgClipboardBmpFromDib(dib.data, dib.size, &bmpSize);
  CHECK(bmp);
  CHECK(bmpSize == dib.size + 14);
  CHECK(bmp[0] == 'B' && bmp[1] == 'M');
  CHECK(get32(bmp + 2) == bmpSize);
  CHECK(get32(bmp + 6) == 0);
  CHECK(get32(bmp + 10) == offsetOfPixels);
  CHECK(memcmp(bmp + 14, dib.data, dib.size) == 0);

  size_t size;
  bool   gotV5 = !v5;
  uint8_t * back = lgClipboardDibFromBmp(bmp, bmpSize, &size, &gotV5);
  CHECK(back);
  CHECK(size == dib.size);
  CHECK(memcmp(back, dib.data, size) == 0);
  CHECK(gotV5 == v5);

  // and a gap between the tables and the pixels is taken out
  uint8_t * gapped = malloc(bmpSize + 10);
  CHECK(gapped);
  memcpy(gapped, bmp, offsetOfPixels);
  memset(gapped + offsetOfPixels, 0xEE, 10);
  memcpy(gapped + offsetOfPixels + 10, bmp + offsetOfPixels,
      bmpSize - offsetOfPixels);
  put32(gapped + 10, offsetOfPixels + 10);
  free(back);
  back = lgClipboardDibFromBmp(gapped, bmpSize + 10, &size, NULL);
  CHECK(back);
  CHECK(size == dib.size);
  CHECK(memcmp(back, dib.data, size) == 0);

  free(gapped);
  free(back);
  free(bmp);
  free(dib.data);
}

static void testBitmaps(void)
{
  // 24 bits: 14 + 40 is where the pixels start
  expectRoundTrip(makeDib(40, 2, 2, 24, 0, 0), 54, false);
  expectRoundTrip(makeDib(40, 5, 3, 24, 0, 0), 54, false);
  // top down
  expectRoundTrip(makeDib(40, 4, -3, 32, 0, 0), 54, false);
  // a palette of all the colors, and of some of them
  expectRoundTrip(makeDib(40, 3, 1, 8, 0, 0), 54 + 1024, false);
  expectRoundTrip(makeDib(40, 9, 2, 4, 0, 0), 54 + 64, false);
  expectRoundTrip(makeDib(40, 9, 2, 1, 0, 0), 54 + 8, false);
  expectRoundTrip(makeDib(40, 3, 1, 8, 0, 10), 54 + 40, false);
  // masks after a header of 40 bytes: three, and four
  expectRoundTrip(makeDib(40, 3, 3, 32, 3, 0), 54 + 12, false);
  expectRoundTrip(makeDib(40, 3, 3, 16, 3, 0), 54 + 12, false);
  expectRoundTrip(makeDib(40, 3, 3, 32, 6, 0), 54 + 16, false);
  // masks in the header of a later version, and its alpha channel
  expectRoundTrip(makeDib(108, 3, 3, 32, 3, 0), 14 + 108, false);
  expectRoundTrip(makeDib(124, 3, 3, 32, 3, 0), 14 + 124, true);
  expectRoundTrip(makeDib(56, 3, 3, 32, 3, 0), 14 + 56, false);
  // a color table that a bitmap of 24 bits may have
  expectRoundTrip(makeDib(40, 3, 3, 24, 0, 16), 54 + 64, false);
}

static void expectRefused(struct Dib dib)
{
  size_t size;
  CHECK(!lgClipboardBmpFromDib(dib.data, dib.size, &size));

  // as a file, if it had the header of one
  uint8_t * file = malloc(dib.size + 14);
  CHECK(file);
  memset(file, 0, 14);
  file[0] = 'B';
  file[1] = 'M';
  put32(file + 10, 54);
  memcpy(file + 14, dib.data, dib.size);
  CHECK(!lgClipboardDibFromBmp(file, dib.size + 14, &size, NULL));
  free(file);
  free(dib.data);
}

static void testRefused(void)
{
  struct Dib dib;

  // pixels that are not all there
  dib = makeDib(40, 4, 4, 24, 0, 0);
  dib.size -= 1;
  expectRefused(dib);

  // headers of a size that no version has
  static const uint32_t badHeaderSizes[] = { 0, 12, 41, 64, 100, 125, 0xFFFFFFFFu };
  for (size_t i = 0; i < sizeof(badHeaderSizes) / sizeof(*badHeaderSizes); ++i)
  {
    dib = makeDib(40, 2, 2, 24, 0, 0);
    put32(dib.data, badHeaderSizes[i]);
    expectRefused(dib);
  }

  // a bitmap that is not one
  dib = makeDib(40, 2, 2, 24, 0, 0);
  put16(dib.data + 12, 2);                       // planes
  expectRefused(dib);
  dib = makeDib(40, 2, 2, 24, 0, 0);
  put32(dib.data + 4, 0);                        // width
  expectRefused(dib);
  dib = makeDib(40, 2, 2, 24, 0, 0);
  put32(dib.data + 4, (uint32_t)-5);
  expectRefused(dib);
  dib = makeDib(40, 2, 2, 24, 0, 0);
  put32(dib.data + 8, 0);                        // height
  expectRefused(dib);
  dib = makeDib(40, 2, 2, 24, 0, 0);
  put16(dib.data + 14, 12);                      // bits
  expectRefused(dib);
  dib = makeDib(40, 2, 2, 24, 0, 0);
  put32(dib.data + 16, 1);                       // compressed by runs
  expectRefused(dib);
  dib = makeDib(40, 2, 2, 24, 0, 0);
  put32(dib.data + 16, 5);                       // a PNG in it
  expectRefused(dib);
  dib = makeDib(40, 2, 2, 24, 3, 0);             // masks on 24 bits
  expectRefused(dib);
  dib = makeDib(40, 3, 1, 8, 0, 0);
  put32(dib.data + 32, 300);                     // more colors than 8 bits have
  expectRefused(dib);
  dib = makeDib(40, 2, 2, 24, 0, 0);
  put32(dib.data + 32, 1000);                    // and than a table of any
  expectRefused(dib);

  // a size that its header says and its bytes do not have
  dib = makeDib(40, 2, 2, 24, 0, 0);
  put32(dib.data + 4, 1 << 20);
  put32(dib.data + 8, 1 << 20);
  expectRefused(dib);
  dib = makeDib(40, 2, 2, 24, 0, 0);
  put32(dib.data + 4, 0x7FFFFFFF);
  put32(dib.data + 8, 0x7FFFFFFF);
  expectRefused(dib);

  size_t size;
  CHECK(!lgClipboardBmpFromDib(NULL, 100, &size));
  CHECK(!lgClipboardBmpFromDib((const uint8_t *)"x", 1, &size));
  CHECK(!lgClipboardDibFromBmp(NULL, 100, &size, NULL));
  CHECK(!lgClipboardDibFromBmp((const uint8_t *)"BM", 2, &size, NULL));
}

// whether a file is taken for a bitmap
static bool accepted(const uint8_t * bmp, size_t size)
{
  size_t    dibSize;
  uint8_t * dib = lgClipboardDibFromBmp(bmp, size, &dibSize, NULL);
  const bool taken = dib != NULL;
  free(dib);
  return taken;
}

static void testRefusedFiles(void)
{
  struct Dib dib = makeDib(40, 2, 2, 24, 0, 0);
  size_t bmpSize;
  uint8_t * bmp = lgClipboardBmpFromDib(dib.data, dib.size, &bmpSize);
  CHECK(bmp);
  size_t size;

  // not a BMP file
  bmp[0] = 'P';
  CHECK(!lgClipboardDibFromBmp(bmp, bmpSize, &size, NULL));
  bmp[0] = 'B';
  CHECK(accepted(bmp, bmpSize));

  // pixels that start inside the header, or after the end of the file
  const uint32_t offset = get32(bmp + 10);
  put32(bmp + 10, 20);
  CHECK(!lgClipboardDibFromBmp(bmp, bmpSize, &size, NULL));
  put32(bmp + 10, (uint32_t)bmpSize);
  CHECK(!lgClipboardDibFromBmp(bmp, bmpSize, &size, NULL));
  put32(bmp + 10, 0xFFFFFFFFu);
  CHECK(!lgClipboardDibFromBmp(bmp, bmpSize, &size, NULL));
  put32(bmp + 10, offset + 1);                  // one pixel byte short
  CHECK(!lgClipboardDibFromBmp(bmp, bmpSize, &size, NULL));
  put32(bmp + 10, offset);

  // the size in the file header is not trusted: it may be wrong
  put32(bmp + 2, 0);
  CHECK(accepted(bmp, bmpSize));
  put32(bmp + 2, 0xFFFFFFFFu);
  CHECK(accepted(bmp, bmpSize));

  free(bmp);
  free(dib.data);
}

static void testBitmapFuzz(void)
{
  // bytes that change what a good bitmap is: nothing that comes back is
  // longer than what went in plus the header of a file, or reads outside it,
  // which the sanitizers see
  struct Dib good = makeDib(40, 5, 4, 24, 0, 0);
  size_t goodBmpSize;
  uint8_t * goodBmp = lgClipboardBmpFromDib(good.data, good.size,
      &goodBmpSize);
  CHECK(goodBmp);

  for (unsigned round = 0; round < 40000; ++round)
  {
    uint8_t mutated[256];
    const size_t size = goodBmpSize < sizeof(mutated) ? goodBmpSize :
      sizeof(mutated);
    memcpy(mutated, goodBmp, size);

    const unsigned changes = 1 + next() % 4;
    for (unsigned i = 0; i < changes; ++i)
      mutated[next() % (size < 60 ? size : 60)] = (uint8_t)next();

    const size_t used = next() % 4 == 0 ? next() % (size + 1) : size;

    size_t out;
    uint8_t * dib = lgClipboardDibFromBmp(mutated, used, &out, NULL);
    if (dib)
    {
      CHECK(out <= used);
      free(dib);
    }

    uint8_t * bmp = lgClipboardBmpFromDib(mutated + 14,
        used > 14 ? used - 14 : 0, &out);
    if (bmp)
    {
      CHECK(out == (used - 14) + 14);
      free(bmp);
    }
  }

  free(goodBmp);
  free(good.data);
}

int main(void)
{
  testFromWindows();
  testToWindows();
  testTextRoundTrip();
  testTextNoCrash();
  testBitmaps();
  testRefused();
  testRefusedFiles();
  testBitmapFuzz();
  puts("clipboard format tests passed");
  return EXIT_SUCCESS;
}
