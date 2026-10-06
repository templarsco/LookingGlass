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

#ifndef _H_LG_CLIENT_DISPLAYSERVER_WIN32_CLIPBOARD_WIC_
#define _H_LG_CLIENT_DISPLAYSERVER_WIN32_CLIPBOARD_WIC_

/* A picture that the guest copied as a PNG, as the bitmap that most programs
 * read from the clipboard. Windows' own codec decodes it, so the client has no
 * decoder of its own to keep up to date, and one that is given a picture by
 * a guest is not one of the client's. */

#include <stddef.h>
#include <stdint.h>

/* The packed device independent bitmap of a picture that Windows' imaging
 * component can decode, a PNG in particular: a BITMAPV5HEADER, from the top
 * down, with 32 bits for each pixel in BGRA with an alpha channel that is not
 * premultiplied, which free() takes. NULL if the data is not a picture that it
 * decodes, or is one of more than 256 MiB of pixels. It starts COM for the
 * thread that calls it if the thread has not, and leaves it as it found it. */
uint8_t * lgClipboardDibFromImage(const uint8_t * data, size_t size,
    size_t * dibSize);

#endif
