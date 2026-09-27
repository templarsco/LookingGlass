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

/* The Windows counterpart of the fontconfig UI font code in util.c. The
 * requested family becomes the primary face, and DirectWrite's system font
 * fallback picks the faces for any glyphs the primary face lacks, as
 * FcFontSort does on Linux. */

#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dwrite_2.h>

#include "util.h"
#include "font.h"
#include "cimgui.h"

#include "common/debug.h"
#include "common/locking.h"
#include "common/windebug.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CP_WORDS ((IM_UNICODE_CODEPOINT_MAX + 32) / 32)

struct UIFontFace
{
  char * file;
  int index;
  ImVector_ImWchar ranges;
};

static IDWriteFactory        * DWFactory    = NULL;
static IDWriteFontCollection * DWFonts      = NULL;
static IDWriteFontFallback   * DWFallback   = NULL;
static IDWriteFont           * UIFontPrimary = NULL;
static wchar_t               * UIFontFamily  = NULL;
static wchar_t UIFontLocale[LOCALE_NAME_MAX_LENGTH];

static uint32_t * UIFontChars     = NULL;
static uint32_t * UIFontRequested = NULL;
static struct UIFontFace * UIFontFaces = NULL;
static unsigned int UIFontFaceCount = 0;
static LG_Lock UIFontLock;
static bool UIFontLockInitialized = false;

static inline bool cpHas(const uint32_t * set, uint32_t c)
{
  return set[c >> 5] & (UINT32_C(1) << (c & 31));
}

static inline bool cpAdd(uint32_t * set, uint32_t c)
{
  const uint32_t bit = UINT32_C(1) << (c & 31);
  if (set[c >> 5] & bit)
    return false;
  set[c >> 5] |= bit;
  return true;
}

static inline void cpDel(uint32_t * set, uint32_t c)
{
  set[c >> 5] &= ~(UINT32_C(1) << (c & 31));
}

static size_t cpCount(const uint32_t * set)
{
  size_t count = 0;
  for (size_t i = 0; i < CP_WORDS; ++i)
    count += __builtin_popcount(set[i]);
  return count;
}

// iterates over the code points in set, in ascending order
#define CP_FOREACH(set, c) \
  for (size_t cpWord_ = 0; cpWord_ < CP_WORDS; ++cpWord_) \
    for (uint32_t cpBits_ = (set)[cpWord_], c; \
         cpBits_ && (c = cpWord_ * 32 + __builtin_ctz(cpBits_), true); \
         cpBits_ &= cpBits_ - 1)

static wchar_t * utf8ToWide(const char * str)
{
  const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str, -1,
      NULL, 0);
  if (len <= 0)
    return NULL;

  wchar_t * out = malloc(len * sizeof(*out));
  if (out && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str, -1,
        out, len))
  {
    free(out);
    return NULL;
  }
  return out;
}

static char * wideToUtf8(const wchar_t * str)
{
  const int len = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL,
      NULL);
  if (len <= 0)
    return NULL;

  char * out = malloc(len);
  if (out && !WideCharToMultiByte(CP_UTF8, 0, str, -1, out, len, NULL, NULL))
  {
    free(out);
    return NULL;
  }
  return out;
}

/* A minimal IDWriteTextAnalysisSource over a fixed string, which is all that
 * IDWriteFontFallback_MapCharacters needs. It lives on the stack, so it does
 * no reference counting. */
struct UIFontText
{
  IDWriteTextAnalysisSource iface;
  const WCHAR * text;
  UINT32 length;
};

static HRESULT STDMETHODCALLTYPE textQueryInterface(
    IDWriteTextAnalysisSource * iface, REFIID riid, void ** out)
{
  if (IsEqualIID(riid, &IID_IUnknown) ||
      IsEqualIID(riid, &IID_IDWriteTextAnalysisSource))
  {
    *out = iface;
    return S_OK;
  }

  *out = NULL;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE textAddRef(IDWriteTextAnalysisSource * iface)
{
  return 1;
}

static ULONG STDMETHODCALLTYPE textRelease(IDWriteTextAnalysisSource * iface)
{
  return 1;
}

static HRESULT STDMETHODCALLTYPE textGetTextAtPosition(
    IDWriteTextAnalysisSource * iface, UINT32 position, const WCHAR ** text,
    UINT32 * length)
{
  const struct UIFontText * source = (const struct UIFontText *)iface;
  if (position >= source->length)
  {
    *text   = NULL;
    *length = 0;
    return S_OK;
  }

  *text   = source->text   + position;
  *length = source->length - position;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE textGetTextBeforePosition(
    IDWriteTextAnalysisSource * iface, UINT32 position, const WCHAR ** text,
    UINT32 * length)
{
  const struct UIFontText * source = (const struct UIFontText *)iface;
  if (position == 0 || position > source->length)
  {
    *text   = NULL;
    *length = 0;
    return S_OK;
  }

  *text   = source->text;
  *length = position;
  return S_OK;
}

static DWRITE_READING_DIRECTION STDMETHODCALLTYPE
  textGetParagraphReadingDirection(IDWriteTextAnalysisSource * iface)
{
  return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
}

static HRESULT STDMETHODCALLTYPE textGetLocaleName(
    IDWriteTextAnalysisSource * iface, UINT32 position, UINT32 * length,
    const WCHAR ** locale)
{
  const struct UIFontText * source = (const struct UIFontText *)iface;
  *length = position < source->length ? source->length - position : 0;
  *locale = UIFontLocale;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE textGetNumberSubstitution(
    IDWriteTextAnalysisSource * iface, UINT32 position, UINT32 * length,
    IDWriteNumberSubstitution ** substitution)
{
  const struct UIFontText * source = (const struct UIFontText *)iface;
  *length = position < source->length ? source->length - position : 0;
  *substitution = NULL;
  return S_OK;
}

static IDWriteTextAnalysisSourceVtbl UIFontTextVtbl =
{
  .QueryInterface               = textQueryInterface,
  .AddRef                       = textAddRef,
  .Release                      = textRelease,
  .GetTextAtPosition            = textGetTextAtPosition,
  .GetTextBeforePosition        = textGetTextBeforePosition,
  .GetParagraphReadingDirection = textGetParagraphReadingDirection,
  .GetLocaleName                = textGetLocaleName,
  .GetNumberSubstitution        = textGetNumberSubstitution,
};

static IDWriteFont * findFamily(const wchar_t * name)
{
  UINT32 index;
  BOOL exists = FALSE;
  if (FAILED(IDWriteFontCollection_FindFamilyName(DWFonts, name, &index,
          &exists)) || !exists)
    return NULL;

  IDWriteFontFamily * family;
  if (FAILED(IDWriteFontCollection_GetFontFamily(DWFonts, index, &family)))
    return NULL;

  IDWriteFont * font = NULL;
  if (FAILED(IDWriteFontFamily_GetFirstMatchingFont(family,
          DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, &font)))
    font = NULL;

  IDWriteFontFamily_Release(family);
  return font;
}

// resolves a font to the local file that holds it and its index in that file
static bool fontLocation(IDWriteFont * font, char ** path, int * index)
{
  IDWriteFontFace            * face   = NULL;
  IDWriteFontFile            * file   = NULL;
  IDWriteFontFileLoader      * loader = NULL;
  IDWriteLocalFontFileLoader * local  = NULL;
  wchar_t * wpath = NULL;
  bool ret = false;

  if (FAILED(IDWriteFont_CreateFontFace(font, &face)))
    goto done;

  UINT32 count = 0;
  if (FAILED(IDWriteFontFace_GetFiles(face, &count, NULL)) || count != 1 ||
      FAILED(IDWriteFontFace_GetFiles(face, &count, &file)))
    goto done;

  const void * key;
  UINT32 keySize;
  if (FAILED(IDWriteFontFile_GetReferenceKey(file, &key, &keySize)) ||
      FAILED(IDWriteFontFile_GetLoader(file, &loader)))
    goto done;

  // fonts that are not plain local files, such as downloadable ones, are
  // skipped because ImGui loads faces by path
  if (FAILED(IDWriteFontFileLoader_QueryInterface(loader,
          &IID_IDWriteLocalFontFileLoader, (void **)&local)))
    goto done;

  UINT32 length;
  if (FAILED(IDWriteLocalFontFileLoader_GetFilePathLengthFromKey(local, key,
          keySize, &length)))
    goto done;

  wpath = malloc((length + 1) * sizeof(*wpath));
  if (!wpath ||
      FAILED(IDWriteLocalFontFileLoader_GetFilePathFromKey(local, key,
          keySize, wpath, length + 1)))
    goto done;

  *path = wideToUtf8(wpath);
  *index = IDWriteFontFace_GetIndex(face);
  ret = *path != NULL;

done:
  free(wpath);
  if (local)
    IDWriteLocalFontFileLoader_Release(local);
  if (loader)
    IDWriteFontFileLoader_Release(loader);
  if (file)
    IDWriteFontFile_Release(file);
  if (face)
    IDWriteFontFace_Release(face);
  return ret;
}

static void uiFontClearFaces(void)
{
  for (unsigned int i = 0; i < UIFontFaceCount; ++i)
  {
    free(UIFontFaces[i].file);
    ImVector_ImWchar_UnInit(&UIFontFaces[i].ranges);
  }

  free(UIFontFaces);
  UIFontFaces = NULL;
  UIFontFaceCount = 0;
}

static void uiFontAddRanges(const ImWchar * ranges)
{
  for (; ranges[0]; ranges += 2)
    for (uint32_t c = ranges[0]; c <= (uint32_t)ranges[1]; ++c)
      cpAdd(UIFontChars, c);
}

static bool uiFontBuildRanges(const uint32_t * chars, size_t count,
    ImVector_ImWchar * ranges)
{
  ImFontGlyphRangesBuilder * builder =
    ImFontGlyphRangesBuilder_ImFontGlyphRangesBuilder();
  if (!builder)
    return false;

  for (size_t i = 0; i < count; ++i)
    ImFontGlyphRangesBuilder_AddChar(builder, (ImWchar)chars[i]);

  ImVector_ImWchar_Init(ranges);
  ImFontGlyphRangesBuilder_BuildRanges(builder, ranges);
  ImFontGlyphRangesBuilder_destroy(builder);
  return ranges->Size > 1;
}

/* Adds the face for font if it covers any of the remaining code points and
 * removes those from remaining. Returns false only on allocation failure. */
static bool uiFontAddFace(IDWriteFont * font, uint32_t * remaining)
{
  const size_t total = cpCount(remaining);
  if (total == 0)
    return true;

  uint32_t * covered = malloc(total * sizeof(*covered));
  if (!covered)
    return false;

  size_t count = 0;
  CP_FOREACH(remaining, c)
  {
    BOOL exists = FALSE;
    if (SUCCEEDED(IDWriteFont_HasCharacter(font, c, &exists)) && exists)
      covered[count++] = c;
  }

  char * file = NULL;
  int index = 0;
  bool ret = true;

  if (count == 0 || !fontLocation(font, &file, &index))
    goto done;

  size_t fontDataSize;
  void * fontData = igImFileLoadToMemory(file, "rb", &fontDataSize, 0);
  if (!fontData)
  {
    DEBUG_WARN("Failed to read UI font face: %s", file);
    goto done;
  }

  const bool compatible = font_isStbTruetypeCompatible(
      fontData, fontDataSize, index);
  igMemFree(fontData);
  if (!compatible)
  {
    DEBUG_WARN("Skipping stb-incompatible UI font face: %s (index %d)",
        file, index);
    goto done;
  }

  struct UIFontFace * faces = realloc(UIFontFaces,
      sizeof(*UIFontFaces) * (UIFontFaceCount + 1));
  if (!faces)
  {
    ret = false;
    goto done;
  }
  UIFontFaces = faces;

  struct UIFontFace * face = &UIFontFaces[UIFontFaceCount];
  memset(face, 0, sizeof(*face));
  if (!uiFontBuildRanges(covered, count, &face->ranges))
  {
    ImVector_ImWchar_UnInit(&face->ranges);
    ret = false;
    goto done;
  }

  face->file  = file;
  face->index = index;
  file = NULL;
  ++UIFontFaceCount;

  for (size_t i = 0; i < count; ++i)
    cpDel(remaining, covered[i]);

done:
  free(file);
  free(covered);
  return ret;
}

/* Asks the system font fallback for faces that cover what is still
 * remaining. Code points no font can serve are removed from remaining and
 * counted in missing when they were requested by UI text. */
static bool uiFontAddFallbackFaces(uint32_t * remaining, size_t * missing)
{
  while (DWFallback)
  {
    const size_t total = cpCount(remaining);
    if (total == 0)
      return true;

    // each code point takes at most two UTF-16 units
    WCHAR * text = malloc(total * 2 * sizeof(*text));
    if (!text)
      return false;

    UINT32 length = 0;
    CP_FOREACH(remaining, c)
    {
      if (c >= 0x10000)
      {
        text[length++] = (WCHAR)(0xD800 + ((c - 0x10000) >> 10));
        text[length++] = (WCHAR)(0xDC00 + ((c - 0x10000) & 0x3FF));
      }
      else
        text[length++] = (WCHAR)c;
    }

    struct UIFontText source =
    {
      .iface  = { .lpVtbl = &UIFontTextVtbl },
      .text   = text,
      .length = length,
    };

    UINT32 mappedLength = 0;
    IDWriteFont * mapped = NULL;
    FLOAT scale;
    const HRESULT hr = IDWriteFontFallback_MapCharacters(DWFallback,
        &source.iface, 0, length, DWFonts, UIFontFamily,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, &mappedLength, &mapped, &scale);

    if (FAILED(hr) || mappedLength == 0 || mappedLength > length)
    {
      if (FAILED(hr))
        DEBUG_WINERROR("IDWriteFontFallback_MapCharacters failed", hr);
      if (mapped)
        IDWriteFont_Release(mapped);
      free(text);
      return true;
    }

    const size_t before = total;
    if (mapped)
    {
      const bool added = uiFontAddFace(mapped, remaining);
      IDWriteFont_Release(mapped);
      if (!added)
      {
        free(text);
        return false;
      }
    }

    // if nothing was added drop the mapped run so that the loop advances
    if (cpCount(remaining) == before)
      for (UINT32 i = 0; i < mappedLength; ++i)
      {
        uint32_t c = text[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < length)
          c = 0x10000 + ((c - 0xD800) << 10) + (text[++i] - 0xDC00);

        cpDel(remaining, c);
        if (cpHas(UIFontRequested, c))
          ++*missing;
      }

    free(text);
  }

  return true;
}

static bool uiFontBuildFaces(ImFontAtlas * atlas)
{
  uiFontClearFaces();

  uiFontAddRanges(ImFontAtlas_GetGlyphRangesDefault(atlas));
  uiFontAddRanges((ImWchar[]) {
    0x2190, 0x2193, // four directional arrows
    0,
  });

  uint32_t * remaining = malloc(CP_WORDS * sizeof(*remaining));
  if (!remaining)
    return false;
  memcpy(remaining, UIFontChars, CP_WORDS * sizeof(*remaining));

  size_t missing = 0;
  if (!uiFontAddFace(UIFontPrimary, remaining) ||
      !uiFontAddFallbackFaces(remaining, &missing))
  {
    free(remaining);
    uiFontClearFaces();
    return false;
  }

  CP_FOREACH(remaining, c)
    if (cpHas(UIFontRequested, c))
      ++missing;

  if (missing > 0)
    DEBUG_WARN("%zu requested UI glyphs have no usable outline font",
        missing);

  free(remaining);
  return UIFontFaceCount > 0;
}

static ImFont * uiFontAddSize(ImFontAtlas * atlas, float size)
{
  ImFont * result = NULL;

  for (unsigned int i = 0; i < UIFontFaceCount; ++i)
  {
    ImFontConfig * config = ImFontConfig_ImFontConfig();
    if (!config)
      return NULL;

    config->MergeMode = result != NULL;
    config->FontNo = UIFontFaces[i].index;

    ImFont * font = ImFontAtlas_AddFontFromFileTTF(atlas,
        UIFontFaces[i].file, size, config, UIFontFaces[i].ranges.Data);
    ImFontConfig_destroy(config);

    if (!font)
    {
      DEBUG_WARN("Failed to add UI font face: %s", UIFontFaces[i].file);
      continue;
    }

    if (!result)
      result = font;
  }

  return result;
}

static void uiFontRelease(void)
{
  if (UIFontPrimary)
    IDWriteFont_Release(UIFontPrimary);
  if (DWFallback)
    IDWriteFontFallback_Release(DWFallback);
  if (DWFonts)
    IDWriteFontCollection_Release(DWFonts);
  if (DWFactory)
    IDWriteFactory_Release(DWFactory);

  free(UIFontFamily);
  free(UIFontChars);
  free(UIFontRequested);

  UIFontPrimary   = NULL;
  DWFallback      = NULL;
  DWFonts         = NULL;
  DWFactory       = NULL;
  UIFontFamily    = NULL;
  UIFontChars     = NULL;
  UIFontRequested = NULL;
}

bool util_initUIFonts(void)
{
  if (DWFactory)
    return true;

  HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
      &IID_IDWriteFactory, (IUnknown **)&DWFactory);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("DWriteCreateFactory failed", hr);
    DWFactory = NULL;
    return false;
  }

  hr = IDWriteFactory_GetSystemFontCollection(DWFactory, &DWFonts, FALSE);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("GetSystemFontCollection failed", hr);
    DWFonts = NULL;
    goto fail;
  }

  // the system font fallback needs Windows 8.1 or later
  IDWriteFactory2 * factory2;
  if (SUCCEEDED(IDWriteFactory_QueryInterface(DWFactory, &IID_IDWriteFactory2,
          (void **)&factory2)))
  {
    if (FAILED(IDWriteFactory2_GetSystemFontFallback(factory2, &DWFallback)))
      DWFallback = NULL;
    IDWriteFactory2_Release(factory2);
  }

  if (!DWFallback)
    DEBUG_WARN("No system font fallback, UI text is limited to one font");

  if (!GetUserDefaultLocaleName(UIFontLocale, ARRAYSIZE(UIFontLocale)))
    wcscpy(UIFontLocale, L"en-US");

  UIFontChars     = calloc(CP_WORDS, sizeof(*UIFontChars));
  UIFontRequested = calloc(CP_WORDS, sizeof(*UIFontRequested));
  if (!UIFontChars || !UIFontRequested)
  {
    DEBUG_ERROR("Out of memory");
    goto fail;
  }

  LG_LOCK_INIT(UIFontLock);
  UIFontLockInitialized = true;
  return true;

fail:
  uiFontRelease();
  return false;
}

char * util_getUIFont(const char * fontName)
{
  // families to try when the configured one is not installed
  static const wchar_t * fallbacks[] =
  {
    L"Consolas",
    L"Segoe UI",
    L"Tahoma",
    L"Arial",
  };

  if (!DWFonts)
    return NULL;

  wchar_t * family = utf8ToWide(fontName);
  IDWriteFont * font = family ? findFamily(family) : NULL;

  for (unsigned int i = 0; !font && i < ARRAYSIZE(fallbacks); ++i)
  {
    free(family);
    family = _wcsdup(fallbacks[i]);
    font = family ? findFamily(family) : NULL;
    if (font)
      DEBUG_WARN("UI font \"%s\" is not installed, using \"%ls\"",
          fontName, family);
  }

  if (!font)
  {
    DEBUG_ERROR("Failed to locate the requested font: %s", fontName);
    free(family);
    return NULL;
  }

  char * file;
  int index;
  if (!fontLocation(font, &file, &index))
  {
    DEBUG_ERROR("The UI font \"%ls\" is not a local font file", family);
    IDWriteFont_Release(font);
    free(family);
    return NULL;
  }

  if (UIFontPrimary)
    IDWriteFont_Release(UIFontPrimary);
  free(UIFontFamily);
  UIFontPrimary = font;
  UIFontFamily  = family;
  return file;
}

bool util_uiFontAddText(const char * text)
{
  if (!UIFontChars || !text)
    return false;

  bool changed = false;
  LG_LOCK(UIFontLock);

  const char * pos = text;
  while (*pos)
  {
    unsigned int c;
    const int length = igImTextCharFromUtf8(&c, pos, NULL);
    if (length <= 0)
      break;
    pos += length;

    if (c < 0x20 || c > IM_UNICODE_CODEPOINT_MAX)
      continue;

    cpAdd(UIFontRequested, c);
    if (cpAdd(UIFontChars, c))
      changed = true;
  }

  LG_UNLOCK(UIFontLock);
  return changed;
}

bool util_buildUIFontAtlas(
    ImFontAtlas * atlas, float size, ImFont ** large)
{
  if (!atlas || !UIFontPrimary || !UIFontChars)
    return false;

  bool result = false;
  LG_LOCK(UIFontLock);

  ImFontAtlas_Clear(atlas);
  if (!uiFontBuildFaces(atlas))
  {
    DEBUG_ERROR("Failed to resolve UI fonts");
    goto done;
  }

  if (!uiFontAddSize(atlas, size))
  {
    DEBUG_ERROR("Failed to add the primary UI font");
    goto done;
  }

  *large = uiFontAddSize(atlas, size * 1.3f);
  if (!*large)
  {
    DEBUG_ERROR("Failed to add the large UI font");
    goto done;
  }

  result = true;

done:
  LG_UNLOCK(UIFontLock);
  return result;
}

void util_freeUIFonts(void)
{
  if (!DWFactory)
    return;

  if (UIFontLockInitialized)
    LG_LOCK(UIFontLock);

  uiFontClearFaces();
  uiFontRelease();

  if (UIFontLockInitialized)
  {
    LG_UNLOCK(UIFontLock);
    LG_LOCK_FREE(UIFontLock);
    UIFontLockInitialized = false;
  }
}
