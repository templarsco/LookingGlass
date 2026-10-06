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

#include "clipboard_format.h"

#include <stdlib.h>
#include <string.h>

/* text */

static void putUtf8(uint8_t * out, size_t * n, uint32_t c)
{
  if (c < 0x80)
    out[(*n)++] = (uint8_t)c;
  else if (c < 0x800)
  {
    out[(*n)++] = (uint8_t)(0xC0 | (c >> 6));
    out[(*n)++] = (uint8_t)(0x80 | (c & 0x3F));
  }
  else if (c < 0x10000)
  {
    out[(*n)++] = (uint8_t)(0xE0 | (c >> 12));
    out[(*n)++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
    out[(*n)++] = (uint8_t)(0x80 | (c & 0x3F));
  }
  else
  {
    out[(*n)++] = (uint8_t)(0xF0 | (c >> 18));
    out[(*n)++] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
    out[(*n)++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
    out[(*n)++] = (uint8_t)(0x80 | (c & 0x3F));
  }
}

uint8_t * lgClipboardTextFromWindows(const uint16_t * text, size_t count,
    size_t * size)
{
  if (!text || !size)
    return NULL;

  size_t units = 0;
  while (units < count && text[units])
    ++units;

  // three bytes for each unit is the most that UTF-8 needs for one
  if (units > LG_CLIPBOARD_FORMAT_MAX / 3)
    return NULL;

  uint8_t * out = malloc(units * 3 + 1);
  if (!out)
    return NULL;

  size_t n = 0;
  for (size_t i = 0; i < units; ++i)
  {
    uint32_t c = text[i];

    // an end of line is an LF
    if (c == '\r' && i + 1 < units && text[i + 1] == '\n')
      continue;

    if (c >= 0xD800 && c <= 0xDBFF)
    {
      if (i + 1 < units && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
      {
        c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00u);
        ++i;
      }
      else
        c = 0xFFFD;
    }
    else if (c >= 0xDC00 && c <= 0xDFFF)
      c = 0xFFFD;

    putUtf8(out, &n, c);
  }

  out[n] = '\0';
  *size  = n;
  return out;
}

/* The character that starts at text[i], or U+FFFD and one byte if what is
 * there is not one. Returns the length of the character. */
static size_t getUtf8(const uint8_t * text, size_t size, size_t i,
    uint32_t * c)
{
  const uint8_t b = text[i];
  if (b < 0x80)
  {
    *c = b;
    return 1;
  }

  size_t   need;
  uint32_t value;
  uint8_t  low = 0x80, high = 0xBF;
  if (b >= 0xC2 && b <= 0xDF)
  {
    need  = 1;
    value = b & 0x1F;
  }
  else if (b >= 0xE0 && b <= 0xEF)
  {
    need  = 2;
    value = b & 0x0F;
    if (b == 0xE0)
      low = 0xA0;   // no overlong form
    else if (b == 0xED)
      high = 0x9F;  // no surrogate
  }
  else if (b >= 0xF0 && b <= 0xF4)
  {
    need  = 3;
    value = b & 0x07;
    if (b == 0xF0)
      low = 0x90;   // no overlong form
    else if (b == 0xF4)
      high = 0x8F;  // nothing above U+10FFFF
  }
  else
  {
    *c = 0xFFFD;
    return 1;
  }

  // the text may end before the character does
  if (i + need >= size)
  {
    *c = 0xFFFD;
    return 1;
  }

  for (size_t k = 1; k <= need; ++k)
  {
    const uint8_t next = text[i + k];
    const uint8_t from = k == 1 ? low  : 0x80;
    const uint8_t to   = k == 1 ? high : 0xBF;
    if (next < from || next > to)
    {
      *c = 0xFFFD;
      return 1;
    }
    value = (value << 6) | (next & 0x3F);
  }

  *c = value;
  return need + 1;
}

uint16_t * lgClipboardTextToWindows(const uint8_t * text, size_t size,
    size_t * units)
{
  if (!text || !units)
    return NULL;

  size_t length = 0;
  while (length < size && text[length])
    ++length;

  // an LF becomes two units, which is the most that a byte makes
  if (length > LG_CLIPBOARD_FORMAT_MAX / 2)
    return NULL;

  uint16_t * out = malloc((length * 2 + 1) * sizeof(*out));
  if (!out)
    return NULL;

  size_t   n    = 0;
  uint16_t last = 0;
  for (size_t i = 0; i < length;)
  {
    uint32_t c;
    i += getUtf8(text, length, i, &c);

    if (c == '\n' && last != '\r')
      out[n++] = '\r';

    if (c >= 0x10000)
    {
      c -= 0x10000;
      out[n++] = (uint16_t)(0xD800 + (c >> 10));
      out[n++] = (uint16_t)(0xDC00 + (c & 0x3FF));
    }
    else
      out[n++] = (uint16_t)c;
    last = out[n - 1];
  }

  out[n++] = 0;
  *units   = n;
  return out;
}

/* bitmaps */

#define FILE_HEADER 14

enum
{
  BI_RGB            = 0,
  BI_BITFIELDS      = 3,
  BI_ALPHABITFIELDS = 6,
};

struct DibInfo
{
  uint32_t headerSize;  // the BITMAPINFOHEADER and what its version adds
  uint64_t tableBytes;  // masks that follow a header of 40 bytes, and the palette
  uint64_t pixelBytes;  // what the pixels need
  bool     v5;
};

static uint32_t le16(const uint8_t * p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t * p)
{
  return le16(p) | (le16(p + 2) << 16);
}

static void putLe32(uint8_t * p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* What the header of a packed bitmap says of the bitmap, which is all that
 * is known of it: the sizes that the rest has to have. Returns false for a
 * bitmap that is not one of a size that fits its header. */
static bool parseDib(const uint8_t * dib, size_t size, struct DibInfo * info)
{
  if (size < 40)
    return false;

  const uint32_t headerSize = le32(dib);
  switch (headerSize)
  {
    case 40: case 52: case 56: case 108: case 124:
      break;
    default:
      return false;
  }
  if (size < headerSize)
    return false;

  const int32_t  width       = (int32_t)le32(dib + 4);
  const int32_t  height      = (int32_t)le32(dib + 8);
  const uint32_t planes      = le16(dib + 12);
  const uint32_t bits        = le16(dib + 14);
  const uint32_t compression = le32(dib + 16);
  const uint32_t colors      = le32(dib + 32);

  if (planes != 1 || width <= 0 || height == 0 ||
      width > (1 << 20) || height > (1 << 20) || height < -(1 << 20))
    return false;

  switch (bits)
  {
    case 1: case 4: case 8: case 16: case 24: case 32:
      break;
    default:
      return false;
  }

  if (compression != BI_RGB && compression != BI_BITFIELDS &&
      compression != BI_ALPHABITFIELDS)
    return false;
  if (compression != BI_RGB && bits != 16 && bits != 32)
    return false;

  uint64_t table = 0;

  // a header of 40 bytes has its masks after it, the others in it
  if (headerSize == 40)
  {
    if (compression == BI_BITFIELDS)
      table += 12;
    else if (compression == BI_ALPHABITFIELDS)
      table += 16;
  }

  // a palette has as many colors as the header says, or all of them
  uint64_t palette = colors;
  if (bits <= 8)
  {
    if (!palette)
      palette = (uint64_t)1 << bits;
    if (palette > ((uint64_t)1 << bits))
      return false;
  }
  else if (palette > 256)
    return false;
  table += palette * 4;

  const uint64_t stride = (((uint64_t)width * bits + 31) / 32) * 4;
  const uint64_t rows   = height < 0 ? (uint64_t)-(int64_t)height :
    (uint64_t)height;

  info->headerSize = headerSize;
  info->tableBytes = table;
  info->pixelBytes = stride * rows;
  info->v5         = headerSize == 124;

  return headerSize + table + info->pixelBytes <= LG_CLIPBOARD_FORMAT_MAX;
}

uint8_t * lgClipboardBmpFromDib(const uint8_t * dib, size_t size,
    size_t * bmpSize)
{
  struct DibInfo info;
  if (!dib || !bmpSize || !parseDib(dib, size, &info))
    return NULL;

  const uint64_t offset = (uint64_t)info.headerSize + info.tableBytes;
  if (offset > size || size - offset < info.pixelBytes)
    return NULL;

  const uint64_t total = (uint64_t)FILE_HEADER + size;
  if (total > LG_CLIPBOARD_FORMAT_MAX)
    return NULL;

  uint8_t * out = malloc((size_t)total);
  if (!out)
    return NULL;

  out[0] = 'B';
  out[1] = 'M';
  putLe32(out + 2, (uint32_t)total);
  putLe32(out + 6, 0);
  putLe32(out + 10, (uint32_t)(FILE_HEADER + offset));
  memcpy(out + FILE_HEADER, dib, size);

  *bmpSize = (size_t)total;
  return out;
}

uint8_t * lgClipboardDibFromBmp(const uint8_t * bmp, size_t size,
    size_t * dibSize, bool * v5)
{
  if (!bmp || !dibSize || size < FILE_HEADER + 40 || bmp[0] != 'B' ||
      bmp[1] != 'M')
    return NULL;

  struct DibInfo info;
  if (!parseDib(bmp + FILE_HEADER, size - FILE_HEADER, &info))
    return NULL;

  // the pixels are where the file says, after the header and its tables
  const uint64_t tables = (uint64_t)info.headerSize + info.tableBytes;
  const uint64_t pixels = le32(bmp + 10);
  if (pixels < FILE_HEADER + tables || pixels > size ||
      size - pixels < info.pixelBytes)
    return NULL;

  const uint64_t total = tables + (size - pixels);
  if (total > LG_CLIPBOARD_FORMAT_MAX)
    return NULL;

  uint8_t * out = malloc((size_t)total);
  if (!out)
    return NULL;

  memcpy(out, bmp + FILE_HEADER, (size_t)tables);
  memcpy(out + tables, bmp + pixels, size - (size_t)pixels);

  *dibSize = (size_t)total;
  if (v5)
    *v5 = info.v5;
  return out;
}

/* PNG files */

size_t lgClipboardPngLength(const uint8_t * data, size_t size)
{
  static const uint8_t signature[8] =
    { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

  if (!data || size < sizeof(signature) ||
      memcmp(data, signature, sizeof(signature)) != 0)
    return size;

  // chunks of a length, a name, that many bytes and a CRC, the last of them IEND
  size_t at = sizeof(signature);
  while (size - at >= 12)
  {
    const uint64_t length = ((uint64_t)data[at] << 24) |
      ((uint64_t)data[at + 1] << 16) | ((uint64_t)data[at + 2] << 8) |
      data[at + 3];
    if (length > size - at - 12)
      return size;

    const size_t end = at + 12 + (size_t)length;
    if (memcmp(data + at + 4, "IEND", 4) == 0)
      return end;
    at = end;
  }

  return size;
}
