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

#ifndef _H_LG_WIN32_INPUT_EVENT_
#define _H_LG_WIN32_INPUT_EVENT_

/* Filters Windows input into the display server input sink. Only the window
 * thread calls these, except for the captured flag. The code has no Windows
 * dependencies so that the tests can run on Linux. */

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "../input.h"

// one wheel notch in the units WM_MOUSEWHEEL reports (WHEEL_DELTA)
#define WIN32_INPUT_WHEEL_STEP 120

// covers every Linux key code below KEY_MAX (0x2ff)
#define WIN32_INPUT_KEY_WORDS (0x300 / 64)

enum Win32InputButton
{
  WIN32_BUTTON_LEFT,
  WIN32_BUTTON_MIDDLE,
  WIN32_BUTTON_RIGHT,
  WIN32_BUTTON_X1,
  WIN32_BUTTON_X2,
};

typedef struct Win32Input
{
  const LG_DSInputSink * sink;
  void                 * opaque;
  _Atomic(bool)          captured;
  uint32_t               buttons;
  int                    wheel;
  bool                   entered;
  bool                   leavePending;
  bool                   focused;
  uint64_t               keys[WIN32_INPUT_KEY_WORDS];
}
Win32Input;

void win32InputInit(Win32Input * input, const LG_DSInputSink * sink,
    void * opaque);

// relative motion is only forwarded while the pointer is captured
void win32InputSetCaptured(Win32Input * input, bool captured);
bool win32InputIsCaptured(const Win32Input * input);

/* Reports keyboard focus. keys are the Linux codes of the keys held when
 * focus arrives. */
bool win32InputFocus(Win32Input * input, bool focused,
    const int * keys, size_t count);

/* The pointer entered or left the client area. Leaving is deferred while a
 * button is held and ignored while the pointer is captured. */
bool win32InputPointerEnter(Win32Input * input, double x, double y);
bool win32InputPointerLeave(Win32Input * input);
void win32InputPointerMotion(Win32Input * input, double x, double y);
void win32InputPointerButton(Win32Input * input,
    enum Win32InputButton button, bool pressed);

// releases the held buttons, for when Windows takes the mouse capture away
void win32InputReleaseButtons(Win32Input * input);

// delta is in WHEEL_DELTA units, positive when turned away from the user
void win32InputPointerWheel(Win32Input * input, int delta);
void win32InputRelativeMotion(Win32Input * input, double x, double y);

/* Reports a Linux key. Repeats, releases of keys that are not held and
 * invalid codes are dropped. Returns true if the key was forwarded. */
bool win32InputKey(Win32Input * input, int key, bool pressed);
bool win32InputKeyHeld(const Win32Input * input, int key);
void win32InputText(Win32Input * input, const char * text);

// reports the modifiers from the held keys, then the lock key LEDs
void win32InputKeyboardState(Win32Input * input,
    bool numLock, bool capsLock, bool scrollLock);

#endif
