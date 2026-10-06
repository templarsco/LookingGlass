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

#include "clipboard_wic.h"
#include "clipboard_format.h"

// the macros that call the methods of a COM object from C
#define COBJMACROS

#include <windows.h>
#include <initguid.h>
#include <wincodec.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

uint8_t * lgClipboardDibFromImage(const uint8_t * data, size_t size,
    size_t * dibSize)
{
  if (!data || !size || size > UINT32_MAX || !dibSize)
    return NULL;

  /* S_FALSE says that the thread had COM already, and is counted all the
   * same. A thread that has it in another mode has it, and gets
   * RPC_E_CHANGED_MODE, which is not counted */
  const HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  const bool counted = SUCCEEDED(init);

  IWICImagingFactory    * factory   = NULL;
  IWICStream            * stream    = NULL;
  IWICBitmapDecoder     * decoder   = NULL;
  IWICBitmapFrameDecode * frame     = NULL;
  IWICFormatConverter   * converter = NULL;
  uint8_t               * out       = NULL;

  if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
          CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory)))
    goto done;

  if (FAILED(IWICImagingFactory_CreateStream(factory, &stream)) ||
      FAILED(IWICStream_InitializeFromMemory(stream, (BYTE *)data,
          (DWORD)size)) ||
      FAILED(IWICImagingFactory_CreateDecoderFromStream(factory,
          (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand,
          &decoder)) ||
      FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)))
    goto done;

  UINT width = 0, height = 0;
  if (FAILED(IWICBitmapFrameDecode_GetSize(frame, &width, &height)) ||
      !width || !height || width > (1U << 16) || height > (1U << 16))
    goto done;

  const uint64_t pixelBytes = (uint64_t)width * height * 4;
  const uint64_t total      = sizeof(BITMAPV5HEADER) + pixelBytes;
  if (total > LG_CLIPBOARD_FORMAT_MAX)
    goto done;

  if (FAILED(IWICImagingFactory_CreateFormatConverter(factory, &converter)) ||
      FAILED(IWICFormatConverter_Initialize(converter,
          (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
          WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)))
    goto done;

  out = calloc(1, (size_t)total);
  if (!out)
    goto done;

  BITMAPV5HEADER header =
  {
    .bV5Size        = sizeof(header),
    .bV5Width       = (LONG)width,
    .bV5Height      = -(LONG)height,   // from the top down, as it is decoded
    .bV5Planes      = 1,
    .bV5BitCount    = 32,
    .bV5Compression = BI_BITFIELDS,
    .bV5SizeImage   = (DWORD)pixelBytes,
    .bV5RedMask     = 0x00FF0000,
    .bV5GreenMask   = 0x0000FF00,
    .bV5BlueMask    = 0x000000FF,
    .bV5AlphaMask   = 0xFF000000,
    .bV5CSType      = 0x73524742,      // LCS_sRGB, "sRGB", which GCC warns of
    .bV5Intent      = LCS_GM_IMAGES,
  };
  memcpy(out, &header, sizeof(header));

  if (FAILED(IWICFormatConverter_CopyPixels(converter, NULL, width * 4,
          (UINT)pixelBytes, out + sizeof(header))))
  {
    free(out);
    out = NULL;
    goto done;
  }

  *dibSize = (size_t)total;

done:
  if (converter)
    IWICFormatConverter_Release(converter);
  if (frame)
    IWICBitmapFrameDecode_Release(frame);
  if (decoder)
    IWICBitmapDecoder_Release(decoder);
  if (stream)
    IWICStream_Release(stream);
  if (factory)
    IWICImagingFactory_Release(factory);
  if (counted)
    CoUninitialize();
  return out;
}
