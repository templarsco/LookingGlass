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

/* The list lookup that the renderers ask about the extensions of the graphics
 * driver with. */

#include "test.h"

#include "common/stringutils.h"

static void testContains(void)
{
  const char * exts = "GL_ARB_sync GL_AMD_pinned_memory  GL_EXT_x ";

  CHECK(str_containsValue(exts, ' ', "GL_ARB_sync"));
  CHECK(str_containsValue(exts, ' ', "GL_AMD_pinned_memory"));
  CHECK(str_containsValue(exts, ' ', "GL_EXT_x"));

  // a name is a whole item, not the start or the end of one
  CHECK(!str_containsValue(exts, ' ', "GL_ARB"));
  CHECK(!str_containsValue(exts, ' ', "GL_ARB_sync2"));
  CHECK(!str_containsValue(exts, ' ', "ARB_sync"));
  CHECK(!str_containsValue(exts, ' ', "pinned_memory"));
  CHECK(!str_containsValue(exts, ' ', "GL_EXT_x GL_ARB_sync"));

  CHECK(str_containsValue("a,b,,c", ',', "c"));
  CHECK(str_containsValue(",,a", ',', "a"));
  CHECK(!str_containsValue("a,b", ' ', "b"));

  CHECK(!str_containsValue("", ' ', "GL_ARB_sync"));
  CHECK(!str_containsValue(" ", ' ', "GL_ARB_sync"));
  CHECK(!str_containsValue(exts, ' ', ""));
}

static void testNoList(void)
{
  // glGetString(GL_EXTENSIONS) answers NULL without a current context, or
  // when the context's profile has no such string. That is no extension, and
  // not a reason to read the address zero
  CHECK(!str_containsValue(NULL, ' ', "GL_ARB_sync"));
  CHECK(!str_containsValue("GL_ARB_sync", ' ', NULL));
  CHECK(!str_containsValue(NULL, ' ', NULL));
}

int main(void)
{
  testContains();
  testNoList();
  puts("stringutils tests passed");
  return EXIT_SUCCESS;
}
