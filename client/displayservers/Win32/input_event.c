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

#include "input_event.h"

#include <linux/input-event-codes.h>
#include <string.h>

_Static_assert(KEY_MAX < WIN32_INPUT_KEY_WORDS * 64,
    "the key bitmap must cover every Linux key code");

/* the sink uses X11 button numbers; the side buttons match the 6 and 7 that
 * the X11 and Wayland backends report for back and forward */
static const unsigned int buttonMap[] =
{
  [WIN32_BUTTON_LEFT  ] = 1,
  [WIN32_BUTTON_MIDDLE] = 2,
  [WIN32_BUTTON_RIGHT ] = 3,
  [WIN32_BUTTON_X1    ] = 6,
  [WIN32_BUTTON_X2    ] = 7,
};

#define BUTTON_COUNT (sizeof(buttonMap) / sizeof(*buttonMap))

void win32InputInit(Win32Input * input, const LG_DSInputSink * sink,
    void * opaque)
{
  memset(input, 0, sizeof(*input));
  atomic_init(&input->captured, false);
  input->sink   = sink;
  input->opaque = opaque;
}

void win32InputSetCaptured(Win32Input * input, bool captured)
{
  atomic_store_explicit(&input->captured, captured, memory_order_release);
}

bool win32InputIsCaptured(const Win32Input * input)
{
  return atomic_load_explicit(&input->captured, memory_order_acquire);
}

bool win32InputFocus(Win32Input * input, bool focused,
    const int * keys, size_t count)
{
  if (input->focused == focused)
    return false;

  // the core releases the keys in the guest when focus is lost
  memset(input->keys, 0, sizeof(input->keys));
  input->focused = focused;
  input->sink->focus(input->opaque, focused);

  if (focused)
    for (size_t i = 0; i < count; ++i)
      win32InputKey(input, keys[i], true);

  return true;
}

bool win32InputPointerEnter(Win32Input * input, double x, double y)
{
  input->leavePending = false;
  if (input->entered)
    return false;

  input->entered = true;
  input->sink->enter(input->opaque, true);
  input->sink->position(input->opaque, x, y);
  return true;
}

static void pointerLeft(Win32Input * input)
{
  input->entered      = false;
  input->leavePending = false;
  input->sink->enter(input->opaque, false);
}

bool win32InputPointerLeave(Win32Input * input)
{
  if (!input->entered || win32InputIsCaptured(input))
    return false;

  if (input->buttons)
  {
    input->leavePending = true;
    return false;
  }

  pointerLeft(input);
  return true;
}

void win32InputPointerMotion(Win32Input * input, double x, double y)
{
  if (input->entered)
    input->sink->position(input->opaque, x, y);
}

void win32InputPointerButton(Win32Input * input,
    enum Win32InputButton button, bool pressed)
{
  if ((unsigned int)button >= BUTTON_COUNT)
    return;

  const unsigned int mapped = buttonMap[button];
  const uint32_t mask = UINT32_C(1) << mapped;
  if (pressed)
  {
    if (!input->entered || (input->buttons & mask))
      return;
    input->buttons |= mask;
  }
  else
  {
    if (!(input->buttons & mask))
      return;
    input->buttons &= ~mask;
  }

  input->sink->button(input->opaque, mapped, pressed);

  if (!input->buttons && input->leavePending)
    pointerLeft(input);
}

void win32InputReleaseButtons(Win32Input * input)
{
  for (unsigned int i = 0; i < BUTTON_COUNT; ++i)
    win32InputPointerButton(input, (enum Win32InputButton)i, false);
}

void win32InputPointerWheel(Win32Input * input, int delta)
{
  if (!input->entered || delta == 0)
    return;

  // the sink counts scrolling towards the user as positive
  const int down = -delta;
  input->wheel += down;

  while (input->wheel > WIN32_INPUT_WHEEL_STEP / 2)
  {
    input->sink->button(input->opaque, 5, true);
    input->sink->button(input->opaque, 5, false);
    input->wheel -= WIN32_INPUT_WHEEL_STEP;
  }

  while (input->wheel < -WIN32_INPUT_WHEEL_STEP / 2)
  {
    input->sink->button(input->opaque, 4, true);
    input->sink->button(input->opaque, 4, false);
    input->wheel += WIN32_INPUT_WHEEL_STEP;
  }

  input->sink->wheel(input->opaque, (double)down / WIN32_INPUT_WHEEL_STEP);
}

void win32InputRelativeMotion(Win32Input * input, double x, double y)
{
  // Windows raw input has no accelerated stream, so both are the raw deltas
  if (input->entered && win32InputIsCaptured(input))
    input->sink->relative(input->opaque, x, y, x, y);
}

bool win32InputKeyHeld(const Win32Input * input, int key)
{
  if (key <= 0 || key >= KEY_MAX)
    return false;

  return input->keys[key / 64] & (UINT64_C(1) << (key % 64));
}

bool win32InputKey(Win32Input * input, int key, bool pressed)
{
  if (!input->focused || key <= 0 || key >= KEY_MAX ||
      win32InputKeyHeld(input, key) == pressed)
    return false;

  const uint64_t bit = UINT64_C(1) << (key % 64);
  if (pressed)
    input->keys[key / 64] |= bit;
  else
    input->keys[key / 64] &= ~bit;

  input->sink->key(input->opaque, key, pressed);
  return true;
}

void win32InputText(Win32Input * input, const char * text)
{
  if (text && *text)
    input->sink->text(input->opaque, text);
}

void win32InputKeyboardState(Win32Input * input,
    bool numLock, bool capsLock, bool scrollLock)
{
  input->sink->modifiers(input->opaque,
      win32InputKeyHeld(input, KEY_LEFTCTRL ) ||
      win32InputKeyHeld(input, KEY_RIGHTCTRL ),
      win32InputKeyHeld(input, KEY_LEFTSHIFT) ||
      win32InputKeyHeld(input, KEY_RIGHTSHIFT),
      win32InputKeyHeld(input, KEY_LEFTALT  ) ||
      win32InputKeyHeld(input, KEY_RIGHTALT  ),
      win32InputKeyHeld(input, KEY_LEFTMETA ) ||
      win32InputKeyHeld(input, KEY_RIGHTMETA ));
  input->sink->leds(input->opaque, numLock, capsLock, scrollLock);
}
