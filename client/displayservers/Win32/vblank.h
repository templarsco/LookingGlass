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

#ifndef _H_LG_CLIENT_DISPLAYSERVER_WIN32_VBLANK_
#define _H_LG_CLIENT_DISPLAYSERVER_WIN32_VBLANK_

/* The pacing behind the Win32 display server's waitFrame. A thread waits for
 * the display's vertical blank, and the render thread waits for that thread,
 * as the Wayland one waits for the compositor's frame callback. The thread
 * also measures the time between the blanks, which is the display's refresh
 * period to a precision that the integer rate of the display mode is not.
 *
 * Nothing here is specific to Windows: where the blank comes from is a source
 * that the caller gives, and so is the clock, which lets the tests run it
 * with ticks they make up. */

#include "interface/displayserver.h"

#include <stdbool.h>
#include <stdint.h>

#define WIN32_VBLANK_MAX_SOURCES 4

typedef struct Win32VBlankSource
{
  const char * name;

  /* Blocks until the next vertical blank of the display, and returns true. It
   * returns false if this source cannot say, such as when the display went
   * away or the session cannot ask, and is asked again after a while, when
   * the display is back or the session can ask. It may block for as long as
   * the display is off, so the pacing does not wait for it to return when it
   * stops. */
  bool   (*wait)(void * opaque);
  void * opaque;
}
Win32VBlankSource;

typedef struct Win32VBlankParams
{
  /* the sources, in the order to try them */
  Win32VBlankSource sources[WIN32_VBLANK_MAX_SOURCES];
  unsigned          sourceCount;

  /* the display mode's period in nanoseconds, or zero if it is not known. It
   * is the period until enough blanks have been measured */
  uint64_t          nominalPeriod;

  /* the clock, in nanoseconds: nanotime() when it is NULL */
  uint64_t        (*now)(void);

  /* the least time without a blank that is a stall, in nanoseconds: 50 ms
   * when zero. Eight periods is the other least. Real time, and not that of
   * the clock, which is for the time of a blank */
  uint64_t          stallMin;

  /* how long to use a source that is not the first before asking those that
   * could not say again, in nanoseconds of real time: 5 s when zero. A
   * display that is unplugged, a driver that is reset and a session that
   * is locked make a source fail, and none of them is for good */
  uint64_t          retryDelay;

  /* called on the thread that waits, before its first wait */
  void            (*threadStart)(void);
}
Win32VBlankParams;

typedef struct Win32VBlankStats
{
  /* the source in use, or NULL when none can say and the pacing wakes by
   * the clock */
  const char * source;

  unsigned long ticks;     // blanks seen, with this source
  unsigned long missed;    // intervals of over one and a half periods
  unsigned long early;     // intervals of under half a period
  uint64_t      period;    // nanoseconds; the measured one when there is one
  bool          measured;
  /* the intervals between blanks, nanoseconds, over the run so far. A
   * percentile is the middle of a bucket of 5 microseconds, and never more
   * than the greatest */
  uint64_t      mean, p50, p95, p99, max;

  /* the render thread, from win32VBlank_wait to win32VBlank_frameSubmitted,
   * in nanoseconds: how long after a blank it was let go, and how long
   * after it the frame had been submitted */
  uint64_t      wakeP50, wakeP95, wakeP99, wakeMax;
  uint64_t      submitP50, submitP95, submitP99, submitMax;
  unsigned long frames;   // frames submitted after a blank
  unsigned long late;     // of those, the ones that took over a period
  unsigned long stalls;  // times that no blank came for a long while
}
Win32VBlankStats;

typedef struct Win32VBlank Win32VBlank;

bool win32VBlank_create(const Win32VBlankParams * params,
    Win32VBlank ** result);

/* Stops the thread and frees what it made. A source that is blocked is not
 * waited for for long: the thread is left to finish, and its memory with it,
 * and the caller must leave the sources alone too. Returns false when that
 * happened, and true when nothing is left. */
bool win32VBlank_destroy(Win32VBlank ** vblank);

/* Waits for the next blank and returns LG_DS_WAIT_FRAME_CADENCE, or
 * LG_DS_WAIT_FRAME_INTERRUPTED if win32VBlank_interrupt was called, or both.
 * Blanks that arrive while the caller is busy come as one.
 *
 * It does not wait for a blank that does not come. When no source can say
 * when the blank is, or none has come for the longer of 50 ms and eight
 * periods, as a display that is off or an adapter that has no blanks do not
 * give any, it returns at the display's rate without
 * LG_DS_WAIT_FRAME_CADENCE, and the caller keeps rendering. It is the
 * blank again once one comes. */
LG_DSWaitFrameResult win32VBlank_wait(Win32VBlank * vblank);

/* Makes the waiting call return, or the next one if none is waiting */
void win32VBlank_interrupt(Win32VBlank * vblank);

/* Called by the thread that waited, when it has submitted the frame that it
 * woke for to be shown: how long that took from the blank is what a render
 * that has to be in time for the next blank is up against. Only the first
 * call after a wait that returned with LG_DS_WAIT_FRAME_CADENCE counts. */
void win32VBlank_frameSubmitted(Win32VBlank * vblank);

/* The period, from the blanks that were measured when there are enough, else
 * the nominal one. Returns false if neither is known. */
bool win32VBlank_getPeriod(Win32VBlank * vblank, uint64_t * period);

/* The display mode changed, or the window went to another display: forgets
 * what was measured */
void win32VBlank_setNominalPeriod(Win32VBlank * vblank, uint64_t period);

void win32VBlank_getStats(Win32VBlank * vblank, Win32VBlankStats * stats);

#endif
