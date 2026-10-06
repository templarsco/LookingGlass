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

/* How a configuration file is read: what the editors of Windows make of it.
 * Notepad writes UTF-8 with a byte order mark, PowerShell 5.1 writes UTF-16,
 * and a title or a font that has an accent or a character of another script
 * is UTF-8 text in the value. */

#include "test.h"

#include "common/debug.h"
#include "common/option.h"

#include <stdbool.h>
#include <string.h>

static struct Option table[] =
{
  {
    .module      = "test",
    .name        = "title",
    .description = "a string",
    .type        = OPTION_TYPE_STRING,
    .value.x_string = NULL,
  },
  {
    .module      = "test",
    .name        = "font",
    .description = "another string",
    .type        = OPTION_TYPE_STRING,
    .value.x_string = NULL,
  },
  {
    .module      = "test",
    .name        = "count",
    .description = "an integer",
    .type        = OPTION_TYPE_INT,
    .value.x_int = 7,
  },
  { 0 }
};

static const char * path = "lg-option-test.ini";

static void fresh(void)
{
  option_free();
  CHECK(option_register(table));
}

// loads a file that has exactly these bytes, and returns what option_load did
static bool load(const void * bytes, size_t size)
{
  FILE * fp = fopen(path, "wb");
  CHECK(fp);
  CHECK(fwrite(bytes, 1, size, fp) == size);
  CHECK(fclose(fp) == 0);

  fresh();
  const bool result = option_load(path);
  CHECK(remove(path) == 0);
  return result;
}

static bool loadText(const char * text)
{
  return load(text, strlen(text));
}

static bool is(const char * name, const char * expected)
{
  const char * value = option_get_string("test", name);
  if (!expected)
    return value == NULL;
  return value && strcmp(value, expected) == 0;
}

static void testPlain(void)
{
  CHECK(loadText("[test]\ntitle=Looking Glass\ncount=42\n"));
  CHECK(is("title", "Looking Glass"));
  CHECK(option_get_int("test", "count") == 42);

  // CRLF, which Windows editors write, a comment, and a value that ends in
  // spaces
  CHECK(loadText("; the title\r\n[test]\r\ntitle = a b  \r\ncount=1\r\n"));
  CHECK(is("title", "a b"));
  CHECK(option_get_int("test", "count") == 1);

  // no line end at the end of the file
  CHECK(loadText("[test]\ntitle=end"));
  CHECK(is("title", "end"));

  // nothing at all
  CHECK(loadText(""));
  CHECK(is("title", NULL));
}

static void testUtf8Values(void)
{
  // an accent, a dash, a script that is not Latin, and a character that
  // UTF-8 has four bytes for: all of it is the value, as it is on the command
  // line
  const char * titles[] =
  {
    "M\xc3\xa1quina Virtual \xe2\x80\x93 Jo\xc3\xa3o",
    "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
    "\xf0\x9f\x96\xa5 desk",
    "caf\xc3\xa9",
  };
  for (size_t i = 0; i < sizeof(titles) / sizeof(*titles); ++i)
  {
    char text[256];
    CHECK(snprintf(text, sizeof(text), "[test]\ntitle=%s\ncount=3\n",
          titles[i]) < (int)sizeof(text));
    CHECK(loadText(text));
    CHECK(is("title", titles[i]));
    CHECK(option_get_int("test", "count") == 3);
  }

  // the end of the value is trimmed, with a character that is not ASCII in
  // front of the space
  CHECK(loadText("[test]\ntitle=caf\xc3\xa9 \n"));
  CHECK(is("title", "caf\xc3\xa9"));

  // as a font name in a path is
  CHECK(loadText("[test]\nfont=C:\\Users\\Jos\xc3\xa9\\fonts\\a.ttf\n"));
  CHECK(is("font", "C:\\Users\\Jos\xc3\xa9\\fonts\\a.ttf"));
}

static void testControlCharacters(void)
{
  // as before, a control character in a line is dropped: a tab, a bell
  CHECK(loadText("[test]\ntitle=a\tb\x07" "c\n"));
  CHECK(is("title", "abc"));
}

static void testUtf8Mark(void)
{
  // Notepad's "UTF-8 with BOM": the mark is not part of the first line,
  // whose section header would not be one with it
  CHECK(loadText("\xef\xbb\xbf[test]\r\ntitle=M\xc3\xa1\r\ncount=9\r\n"));
  CHECK(is("title", "M\xc3\xa1"));
  CHECK(option_get_int("test", "count") == 9);

  // only the mark, or nothing after it that is a line
  CHECK(loadText("\xef\xbb\xbf"));
  CHECK(is("title", NULL));
  CHECK(loadText("\xef\xbb\xbf; only a comment\n"));
  CHECK(is("title", NULL));

  // a mark that is not at the start is a character like another one
  CHECK(loadText("[test]\ntitle=\xef\xbb\xbfx\n"));
  CHECK(is("title", "\xef\xbb\xbfx"));
}

static void testUtf16(void)
{
  // what PowerShell 5.1's `>` and Out-File make of "[test]\ntitle=a": a mark
  // and every character with a NUL after it. It is not read as a file that
  // sets nothing
  static const unsigned char le[] =
  {
    0xff, 0xfe,
    '[', 0, 't', 0, 'e', 0, 's', 0, 't', 0, ']', 0, '\r', 0, '\n', 0,
    't', 0, 'i', 0, 't', 0, 'l', 0, 'e', 0, '=', 0, 'a', 0, '\r', 0,
    '\n', 0,
  };
  CHECK(!load(le, sizeof(le)));
  CHECK(is("title", NULL));

  static const unsigned char be[] =
  {
    0xfe, 0xff,
    0, '[', 0, 't', 0, 'e', 0, 's', 0, 't', 0, ']', 0, '\n',
  };
  CHECK(!load(be, sizeof(be)));
  CHECK(is("title", NULL));

  // just the mark
  static const unsigned char alone[] = { 0xff, 0xfe };
  CHECK(!load(alone, sizeof(alone)));
}

static void testMissing(void)
{
  fresh();
  CHECK(!option_load("lg-option-test-does-not-exist.ini"));
}

int main(void)
{
  debug_init();

  testPlain();
  testUtf8Values();
  testControlCharacters();
  testUtf8Mark();
  testUtf16();
  testMissing();
  option_free();
  puts("option tests passed");
  return EXIT_SUCCESS;
}
