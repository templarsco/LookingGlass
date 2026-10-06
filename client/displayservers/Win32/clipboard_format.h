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

#ifndef _H_LG_CLIENT_DISPLAYSERVER_WIN32_CLIPBOARD_FORMAT_
#define _H_LG_CLIENT_DISPLAYSERVER_WIN32_CLIPBOARD_FORMAT_

/* What the Windows clipboard holds, and what the clipboard of the client
 * sends to the guest: the text of the clipboard is UTF-16 with a CR and an LF
 * at the end of a line, and the text that is sent is UTF-8 with an LF; an
 * image on the clipboard is a device independent bitmap, which is a BMP file
 * without its file header, and the one that is sent is the BMP file.
 *
 * Nothing here calls Windows, so that the unit tests run on Linux too, and
 * what comes from the guest is checked as what it is: untrusted. Every
 * function that returns a buffer returns one that the caller frees with
 * free(), or NULL, and sets errno nowhere. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the most that any of these makes: a bitmap of 256 MiB is of a screen
 * larger than 8K, and text of that size is not text that anyone pastes */
#define LG_CLIPBOARD_FORMAT_MAX (UINT64_C(256) * 1024 * 1024)

/* UTF-16 text to UTF-8. The text ends at the first NUL or after count units.
 * A CR followed by an LF is the LF, which is what a line ends in on the wire;
 * a CR with no LF stays. A unit of a pair that is not a pair is U+FFFD. */
uint8_t * lgClipboardTextFromWindows(const uint16_t * text, size_t count,
    size_t * size);

/* UTF-8 text to UTF-16 with a NUL at the end, which the size counts: an LF
 * that does not follow a CR is a CR and an LF. The text ends at the first NUL
 * or after size bytes. A byte that does not start or continue a character is
 * U+FFFD. */
uint16_t * lgClipboardTextToWindows(const uint8_t * text, size_t size,
    size_t * units);

/* The BMP file of a packed device independent bitmap (CF_DIB or CF_DIBV5):
 * the file header is added, with the offset of the pixels that the header, the
 * masks and the palette give. NULL if the bitmap is not one that can be told
 * from its own header: a compression that is not RGB or bit fields, a header
 * of a size that no version has, a palette or pixels that the size does not
 * hold. */
uint8_t * lgClipboardBmpFromDib(const uint8_t * dib, size_t size,
    size_t * bmpSize);

/* The packed device independent bitmap of a BMP file: the file header is
 * taken off, and so is any gap between the palette and the pixels. NULL if
 * the file is not a BMP that fits its own size. If v5 is not NULL it is set
 * to whether the header is a BITMAPV5HEADER, which carries an alpha channel
 * that CF_DIB's does not. */
uint8_t * lgClipboardDibFromBmp(const uint8_t * bmp, size_t size,
    size_t * dibSize, bool * v5);

/* The length of a PNG file in the buffer that holds it: up to the end of its
 * last chunk, as Windows gives a buffer that is larger than the data that was
 * put in it. A buffer that is not a PNG file, or whose chunks do not fit it, is
 * returned in full, as nothing here knows better. */
size_t lgClipboardPngLength(const uint8_t * data, size_t size);

#endif
