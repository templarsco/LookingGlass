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

#include "hcs_systems.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the longest value that is kept, and how deep a value the scanner follows
#define VALUE_MAX 64
#define DEPTH_MAX 32

struct Out
{
  char * data;
  size_t len, cap;
  bool   failed;
};

static void outAdd(struct Out * out, const char * text, size_t len)
{
  if (out->failed)
    return;

  if (out->len + len + 1 > out->cap)
  {
    size_t cap = out->cap ? out->cap : 256;
    while (cap < out->len + len + 1)
      cap *= 2;

    char * data = realloc(out->data, cap);
    if (!data)
    {
      out->failed = true;
      return;
    }
    out->data = data;
    out->cap  = cap;
  }

  memcpy(out->data + out->len, text, len);
  out->len += len;
  out->data[out->len] = '\0';
}

// a JSON string of printable ASCII. The values of a summary have no use for
// anything else, and a name could be in it
static void outString(struct Out * out, const char * text)
{
  outAdd(out, "\"", 1);
  for (; *text; ++text)
  {
    const char c = *text;
    if (c == '"' || c == '\\')
    {
      const char escaped[2] = { '\\', c };
      outAdd(out, escaped, 2);
    }
    else if (c >= 0x20 && c < 0x7f)
      outAdd(out, &c, 1);
  }
  outAdd(out, "\"", 1);
}

static const char * skipSpace(const char * p, const char * end)
{
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
    ++p;
  return p;
}

// p is at the opening quote. Returns the place after the closing one, or NULL
// if the string does not end. The text, cut to size, goes to out unless it is
// NULL. An escape other than for a quote or a backslash is kept as a space
static const char * readString(const char * p, const char * end, char * out,
    size_t size)
{
  if (p >= end || *p != '"')
    return NULL;

  size_t len = 0;
  for (++p; p < end && *p != '"'; ++p)
  {
    char c = *p;
    if (c == '\\')
    {
      if (++p >= end)
        return NULL;

      c = *p;
      if (c == 'u')
      {
        if (end - p < 5)
          return NULL;
        p += 4;
        c = '?';
      }
      else if (c != '"' && c != '\\')
        c = ' ';
    }

    if (out && len + 1 < size)
      out[len++] = c;
  }

  if (p >= end)
    return NULL;
  if (out && size)
    out[len] = '\0';
  return p + 1;
}

// skips one JSON value of any kind, and returns the place after it, or NULL
// if it is not well formed
static const char * skipValue(const char * p, const char * end,
    unsigned depth)
{
  p = skipSpace(p, end);
  if (p >= end || depth > DEPTH_MAX)
    return NULL;

  if (*p == '"')
    return readString(p, end, NULL, 0);

  if (*p == '{' || *p == '[')
  {
    const char close = *p == '{' ? '}' : ']';
    p = skipSpace(p + 1, end);
    if (p < end && *p == close)
      return p + 1;

    for (;;)
    {
      if (close == '}')
      {
        p = readString(skipSpace(p, end), end, NULL, 0);
        if (!p)
          return NULL;
        p = skipSpace(p, end);
        if (p >= end || *p != ':')
          return NULL;
        ++p;
      }

      p = skipValue(p, end, depth + 1);
      if (!p)
        return NULL;

      p = skipSpace(p, end);
      if (p < end && *p == ',')
      {
        ++p;
        continue;
      }
      return p < end && *p == close ? p + 1 : NULL;
    }
  }

  // a number, true, false or null
  const char * start = p;
  while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
      *p != '\t' && *p != '\r' && *p != '\n')
    ++p;
  return p > start ? p : NULL;
}

struct System
{
  char type [VALUE_MAX];
  char owner[VALUE_MAX];
  char state[VALUE_MAX];
};

// p is at an object. Keeps the three members of it that a summary has, when
// they are strings, and skips the rest. Returns the place after the object,
// or NULL if p is not at one, or it is not well formed
static const char * readSystem(const char * p, const char * end,
    struct System * system)
{
  memset(system, 0, sizeof(*system));

  p = skipSpace(p, end);
  if (p >= end || *p != '{')
    return NULL;

  p = skipSpace(p + 1, end);
  if (p < end && *p == '}')
    return p + 1;

  for (;;)
  {
    char key[32];
    p = readString(skipSpace(p, end), end, key, sizeof(key));
    if (!p)
      return NULL;

    p = skipSpace(p, end);
    if (p >= end || *p != ':')
      return NULL;
    p = skipSpace(p + 1, end);

    char * keep = NULL;
    if (strcmp(key, "SystemType") == 0)
      keep = system->type;
    else if (strcmp(key, "Owner") == 0)
      keep = system->owner;
    else if (strcmp(key, "State") == 0)
      keep = system->state;

    if (keep && p < end && *p == '"')
      p = readString(p, end, keep, VALUE_MAX);
    else
      p = skipValue(p, end, 1);
    if (!p)
      return NULL;

    p = skipSpace(p, end);
    if (p < end && *p == ',')
    {
      ++p;
      continue;
    }
    return p < end && *p == '}' ? p + 1 : NULL;
  }
}

char * hcsSystemsSummary(const char * doc)
{
  struct Out out   = { 0 };
  unsigned long count = 0, unread = 0;

  outAdd(&out, "{\"systems\":[", 12);

  const char * end = doc ? doc + strlen(doc) : NULL;
  const char * p   = doc ? skipSpace(doc, end) : NULL;
  if (p && p < end && *p == '[')
  {
    p = skipSpace(p + 1, end);
    while (p && p < end && *p != ']')
    {
      struct System system;
      const char * next = readSystem(p, end, &system);
      if (next)
      {
        if (count)
          outAdd(&out, ",", 1);
        ++count;

        outAdd(&out, "{\"system_type\":", 15);
        outString(&out, system.type);
        outAdd(&out, ",\"owner\":", 9);
        outString(&out, system.owner);
        outAdd(&out, ",\"state\":", 9);
        outString(&out, system.state);
        outAdd(&out, "}", 1);
      }
      else
      {
        // not an object, or not well formed: none of it is kept
        ++unread;
        next = skipValue(p, end, 0);
      }

      p = next ? skipSpace(next, end) : NULL;
      if (p && p < end && *p == ',')
        p = skipSpace(p + 1, end);
    }
  }
  else if (p && p < end)
    unread = 1;

  char tail[64];
  snprintf(tail, sizeof(tail), "],\"count\":%lu,\"unread\":%lu}", count,
      unread);
  outAdd(&out, tail, strlen(tail));

  if (out.failed)
  {
    free(out.data);
    return NULL;
  }
  return out.data;
}
