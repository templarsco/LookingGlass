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

// Checks that the summary of the HCS's compute systems has what the probe's
// report needs, and none of what identifies a PC's virtual machines

#include "hcs_systems.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) \
  do \
  { \
    if (!(x)) \
    { \
      fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #x); \
      exit(EXIT_FAILURE); \
    } \
  } \
  while (0)

static char * summary(const char * doc)
{
  char * result = hcsSystemsSummary(doc);
  CHECK(result);
  return result;
}

static void expectEqual(const char * doc, const char * wanted)
{
  char * result = summary(doc);
  if (strcmp(result, wanted))
  {
    fprintf(stderr, "for %s\n got    %s\n wanted %s\n",
        doc ? doc : "NULL", result, wanted);
    exit(EXIT_FAILURE);
  }
  free(result);
}

// what the HCS returns: ids, names, and the like for each system
static void testNoIdentifiers(void)
{
  const char * doc =
    "[{\"Id\":\"A1B2C3D4-0000-1111-2222-333344445555\","
      "\"SystemType\":\"VirtualMachine\",\"Name\":\"My-Private-VM\","
      "\"Owner\":\"VMMS\",\"RuntimeId\":\"deadbeef-1111-2222-3333-444455556666\","
      "\"State\":\"Running\"},"
    " {\"Id\":\"FEDCBA98-7654-3210-FEDC-BA9876543210\","
      "\"SystemType\":\"VirtualMachine\",\"Owner\":\"LookingGlass.HcsProbe\","
      "\"State\":\"Stopped\"},\n"
    " {\"Id\":\"plain-container\",\"SystemType\":\"Container\"}]";

  char * result = summary(doc);
  CHECK(strstr(result, "\"count\":3"));
  CHECK(strstr(result, "\"unread\":0"));
  CHECK(strstr(result, "\"owner\":\"VMMS\""));
  CHECK(strstr(result, "\"state\":\"Running\""));
  CHECK(strstr(result, "\"owner\":\"LookingGlass.HcsProbe\""));
  CHECK(strstr(result, "\"system_type\":\"Container\""));

  CHECK(!strstr(result, "A1B2C3D4"));
  CHECK(!strstr(result, "FEDCBA98"));
  CHECK(!strstr(result, "My-Private-VM"));
  CHECK(!strstr(result, "deadbeef"));
  CHECK(!strstr(result, "plain-container"));
  CHECK(!strstr(result, "RuntimeId"));
  CHECK(!strstr(result, "\"Id\""));
  free(result);
}

static void testShapes(void)
{
  expectEqual(NULL, "{\"systems\":[],\"count\":0,\"unread\":0}");
  expectEqual("", "{\"systems\":[],\"count\":0,\"unread\":0}");
  expectEqual("  [ ]  ", "{\"systems\":[],\"count\":0,\"unread\":0}");
  expectEqual("[{}]", "{\"systems\":[{\"system_type\":\"\",\"owner\":\"\","
      "\"state\":\"\"}],\"count\":1,\"unread\":0}");

  // the HCS's error documents, or anything else that is not a list
  expectEqual("{\"Error\":-2147024891}",
      "{\"systems\":[],\"count\":0,\"unread\":1}");
  expectEqual("not json", "{\"systems\":[],\"count\":0,\"unread\":1}");
}

static void testValues(void)
{
  // escapes in a kept value, and members of other types where it would
  // keep one: nothing is kept that is not a string
  char * result = summary(
    "[{\"Owner\":\"a\\\"b\\\\c\\u00e9d\",\"State\":7,"
      "\"SystemType\":[\"x\",{\"y\":1}]}]");
  CHECK(strstr(result, "\"owner\":\"a\\\"b\\\\c?d\""));
  CHECK(strstr(result, "\"state\":\"\""));
  CHECK(strstr(result, "\"system_type\":\"\""));
  CHECK(strstr(result, "\"count\":1"));
  free(result);

  // a value is cut, and what is not printable ASCII goes
  char longValue[200];
  memset(longValue, 'x', sizeof(longValue));
  longValue[sizeof(longValue) - 1] = '\0';
  char doc[400];
  snprintf(doc, sizeof(doc), "[{\"Owner\":\"%s\"}]", longValue);
  result = summary(doc);
  const char * owner = strstr(result, "\"owner\":\"");
  CHECK(owner);
  owner += strlen("\"owner\":\"");
  CHECK(strchr(owner, '"') - owner == 63);
  free(result);

  result = summary("[{\"Owner\":\"caf\xc3\xa9\x01-vm\"}]");
  CHECK(strstr(result, "\"owner\":\"caf-vm\""));
  free(result);

  // a key that is not one of the three, and that holds the name of one
  result = summary("[{\"Name\":\"Owner\",\"X\":{\"Owner\":\"nested\"},"
      "\"Owner\":\"top\"}]");
  CHECK(strstr(result, "\"owner\":\"top\""));
  CHECK(!strstr(result, "nested"));
  free(result);
}

static void testMalformed(void)
{
  // what cannot be read is counted, and none of it is kept
  char * result = summary("[{\"SystemType\":\"VirtualMachine\","
      "\"Owner\":\"VMMS\",\"State\":\"Running\"},"
      "{\"SystemType\":\"VirtualMachine\",\"Owner\":\"secret-owner\",");
  CHECK(strstr(result, "\"count\":1"));
  CHECK(strstr(result, "\"unread\":1"));
  CHECK(!strstr(result, "secret-owner"));
  free(result);

  result = summary("[\"a string\",12,null,{\"Owner\":\"VMMS\"}]");
  CHECK(strstr(result, "\"count\":1"));
  CHECK(strstr(result, "\"unread\":3"));
  CHECK(strstr(result, "\"owner\":\"VMMS\""));
  CHECK(!strstr(result, "a string"));
  free(result);

  // a string that does not end, or an escape that is cut short
  result = summary("[{\"Owner\":\"never ends");
  CHECK(strstr(result, "\"count\":0"));
  CHECK(!strstr(result, "never ends"));
  free(result);

  result = summary("[{\"Owner\":\"cut\\");
  CHECK(strstr(result, "\"count\":0"));
  free(result);

  result = summary("[{\"Owner\":\"cut\\u12");
  CHECK(strstr(result, "\"count\":0"));
  free(result);
}

static void testDeep(void)
{
  // a document nested far deeper than a list of systems is, which must not
  // exhaust the stack
  enum { DEEP = 5000 };
  char * doc = malloc(2 * DEEP + 64);
  CHECK(doc);
  size_t len = 0;
  doc[len++] = '[';
  doc[len++] = '{';
  len += (size_t)sprintf(doc + len, "\"Id\":");
  for (int i = 0; i < DEEP; ++i)
    doc[len++] = '[';
  for (int i = 0; i < DEEP; ++i)
    doc[len++] = ']';
  doc[len++] = '}';
  doc[len++] = ']';
  doc[len] = '\0';

  char * result = summary(doc);
  CHECK(strstr(result, "\"unread\":1"));
  free(result);
  free(doc);
}

int main(void)
{
  testNoIdentifiers();
  testShapes();
  testValues();
  testMalformed();
  testDeep();
  return EXIT_SUCCESS;
}
