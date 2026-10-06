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

#include "vblank.h"

#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/thread.h"
#include "common/time.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// the last intervals, which the period is measured from
#define RING_SIZE         256

// the period is the measured one once this many intervals are in the ring,
// and is measured again every PUBLISH_INTERVAL blanks
#define MIN_INTERVALS     64
#define PUBLISH_INTERVAL  8

// the statistics count how often a time was in each bucket, which says the
// percentiles of the whole run and not of the last few. The last bucket also
// takes whatever is longer than the others reach, and the greatest time is
// kept apart
#define BUCKET_NS         5000
#define BUCKETS           4000

// how long destroy waits for the thread, which a source can keep in a wait
#define STOP_WAIT_MS      200

// the period to wake by when no source can say and the display mode's is not
// known either
#define DEFAULT_PERIOD    UINT64_C(16666667)

// no blank for the longer of this and STALL_PERIODS periods is a source that
// is not giving any
#define STALL_MIN         UINT64_C(50000000)
#define STALL_PERIODS     8

// how long a source that is not the first is used before the ones that failed
// are asked again
#define RETRY_DELAY       UINT64_C(5000000000)

struct Histogram
{
  uint32_t bucket[BUCKETS];
  uint64_t count, sum, max;
};

struct Win32VBlank
{
  Win32VBlankParams params;

  LGEvent  * event;   // a blank, or an interruption
  LGEvent  * exited;  // the thread is finished
  LGThread * thread;

  atomic_bool       stop;
  atomic_uint       flags;
  _Atomic(uint64_t) nominal;
  _Atomic(uint64_t) measured;  // zero until there is a measured period
  _Atomic(unsigned) source;    // the one in use, or the count when none
  _Atomic(uint64_t) tickTime;  // when the last blank was seen, by the clock
  _Atomic(uint64_t) tickReal;  // and by the real one, for a stall, which is
                               // time that passes and not time that is made up

  uint64_t createdReal;        // for the first blank to be late for
  uint64_t wakeAt;             // the render thread: its blank, or zero
  bool     stalled;            // the render thread: no blank for too long

  struct Win32VBlank * abandoned;  // the next of those left by destroy

  LG_Lock  lock;               // guards everything below
  uint64_t lastTick;
  uint64_t ring[RING_SIZE];
  unsigned ringCount;
  unsigned ringNext;
  unsigned sinceUpdate;
  unsigned long ticks, missed, early;
  unsigned long frames, late, stalls;
  struct Histogram intervals;  // between blanks
  struct Histogram wakes;      // from a blank to the render thread
  struct Histogram submits;    // from a blank to its frame submitted
};

// what destroy left because its thread did not return, which the thread still
// uses. They are listed so that they stay reachable, and are never freed
static _Atomic(struct Win32VBlank *) g_abandoned;

static uint64_t clockNow(const struct Win32VBlank * v)
{
  return v->params.now ? v->params.now() : nanotime();
}

static void histogramAdd(struct Histogram * h, uint64_t ns)
{
  const uint64_t index = ns / BUCKET_NS;
  ++h->bucket[index < BUCKETS ? index : BUCKETS - 1];
  ++h->count;
  h->sum += ns;
  if (ns > h->max)
    h->max = ns;
}

// the middle of the bucket that holds the value at percent of the sorted
// ones, and never more than the greatest. Zero when there is none
static uint64_t histogramPercentile(const struct Histogram * h,
    unsigned percent)
{
  if (!h->count)
    return 0;

  uint64_t rank = (h->count * percent + 99) / 100;
  if (rank < 1)
    rank = 1;

  uint64_t seen = 0;
  for (unsigned i = 0; i < BUCKETS; ++i)
  {
    seen += h->bucket[i];
    if (seen >= rank)
    {
      const uint64_t middle = (uint64_t)i * BUCKET_NS + BUCKET_NS / 2;
      return middle < h->max ? middle : h->max;
    }
  }
  return h->max;
}

static int compareInterval(const void * a, const void * b)
{
  const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}

// the period from the intervals in the ring that are near the one that the
// display should have: the display mode's, or if that is not known, the
// middle one. An interval of two periods is a blank that was missed, and is
// not the period. Called with the lock
static void updateMeasured(struct Win32VBlank * v)
{
  if (v->ringCount < MIN_INTERVALS)
    return;

  uint64_t reference = atomic_load_explicit(&v->nominal, memory_order_relaxed);
  if (!reference)
  {
    uint64_t sorted[RING_SIZE];
    memcpy(sorted, v->ring, sizeof(*sorted) * v->ringCount);
    qsort(sorted, v->ringCount, sizeof(*sorted), compareInterval);
    reference = sorted[v->ringCount / 2];
  }
  if (!reference)
    return;

  const uint64_t low  = reference - reference / 4;
  const uint64_t high = reference + reference / 4;
  uint64_t       sum  = 0;
  unsigned       used = 0;
  for (unsigned i = 0; i < v->ringCount; ++i)
    if (v->ring[i] >= low && v->ring[i] <= high)
    {
      sum += v->ring[i];
      ++used;
    }

  // too few near it for the display to be running at a rate that can be said
  if (used < MIN_INTERVALS / 2)
    return;

  atomic_store_explicit(&v->measured, sum / used, memory_order_release);
}

// forgets what was measured, and counts from here. Called with the lock
static void resetMeasurement(struct Win32VBlank * v)
{
  v->lastTick     = 0;
  v->ringCount    = 0;
  v->ringNext     = 0;
  v->sinceUpdate  = 0;
  v->ticks        = 0;
  v->missed       = 0;
  v->early        = 0;
  v->frames       = 0;
  v->late         = 0;
  v->stalls       = 0;
  memset(&v->intervals, 0, sizeof(v->intervals));
  memset(&v->wakes    , 0, sizeof(v->wakes    ));
  memset(&v->submits  , 0, sizeof(v->submits  ));
  atomic_store_explicit(&v->measured, 0, memory_order_release);
}

static void recordTick(struct Win32VBlank * v, uint64_t now)
{
  LG_LOCK(v->lock);

  ++v->ticks;
  const uint64_t last = v->lastTick;
  v->lastTick = now;
  if (last && now > last)
  {
    const uint64_t interval  = now - last;
    uint64_t       reference = atomic_load_explicit(&v->measured,
        memory_order_relaxed);
    if (!reference)
      reference = atomic_load_explicit(&v->nominal, memory_order_relaxed);

    if (reference)
    {
      if (interval > reference + reference / 2)
        ++v->missed;
      else if (interval < reference / 2)
        ++v->early;
    }

    histogramAdd(&v->intervals, interval);

    v->ring[v->ringNext] = interval;
    v->ringNext          = (v->ringNext + 1) % RING_SIZE;
    if (v->ringCount < RING_SIZE)
      ++v->ringCount;

    if (++v->sinceUpdate >= PUBLISH_INTERVAL)
    {
      v->sinceUpdate = 0;
      updateMeasured(v);
    }
  }

  LG_UNLOCK(v->lock);
}

// a blank of the source in use
static void blankSeen(struct Win32VBlank * v)
{
  const uint64_t tick = clockNow(v);
  recordTick(v, tick);
  atomic_store_explicit(&v->tickTime, tick, memory_order_release);
  atomic_store_explicit(&v->tickReal, nanotime(), memory_order_release);
  atomic_fetch_or_explicit(&v->flags, LG_DS_WAIT_FRAME_CADENCE,
      memory_order_release);
  lgSignalEvent(v->event);
}

// the source in use is another now, which measures from the start
static void sourceChanged(struct Win32VBlank * v, unsigned current)
{
  atomic_store_explicit(&v->source, current, memory_order_release);

  LG_LOCK(v->lock);
  resetMeasurement(v);
  LG_UNLOCK(v->lock);
}

static int vblankThread(void * opaque)
{
  struct Win32VBlank * v = opaque;

  if (v->params.threadStart)
    v->params.threadStart();

  const uint64_t retryDelay = v->params.retryDelay ?
    v->params.retryDelay : RETRY_DELAY;

  unsigned current = 0;
  uint64_t retryAt = 0;  // real time to ask the sources that failed again
  bool     failed[WIN32_VBLANK_MAX_SOURCES] = { false };
  while (!atomic_load_explicit(&v->stop, memory_order_acquire))
  {
    if (current > 0 && nanotime() >= retryAt)
    {
      // what failed may work again, once the display is back or the driver
      // is. It is asked in turn, and if none of them can say yet, the source
      // in use stays, and the measuring that it has made with it
      retryAt = nanotime() + retryDelay;

      const unsigned count = current < v->params.sourceCount ?
        current : v->params.sourceCount;
      for (unsigned i = 0; i < count; ++i)
      {
        const Win32VBlankSource * source = &v->params.sources[i];
        const bool ok = source->wait(source->opaque);
        if (atomic_load_explicit(&v->stop, memory_order_acquire))
          goto done;

        if (!ok)
          continue;

        DEBUG_INFO("The %s source of the vertical blank works again",
            source->name ? source->name : "unnamed");
        failed[i] = false;
        current   = i;
        sourceChanged(v, current);
        blankSeen(v);
        break;
      }
    }

    if (current >= v->params.sourceCount)
    {
      // nothing can say when the blank is: wake at the display's rate, and
      // do not claim that it is the cadence of the display
      uint64_t period = atomic_load_explicit(&v->nominal,
          memory_order_relaxed);
      nsleep(period ? period : DEFAULT_PERIOD);
      if (atomic_load_explicit(&v->stop, memory_order_acquire))
        break;

      lgSignalEvent(v->event);
      continue;
    }

    const Win32VBlankSource * source = &v->params.sources[current];
    const bool ok = source->wait(source->opaque);
    if (atomic_load_explicit(&v->stop, memory_order_acquire))
      break;

    if (!ok)
    {
      // once for as long as it does not work, and not each time that it is
      // asked again
      if (!failed[current])
        DEBUG_WARN("The %s source of the vertical blank failed, %s",
            source->name ? source->name : "unnamed",
            current + 1 < v->params.sourceCount ?
              "trying the next one" : "waking by the clock");
      failed[current] = true;

      ++current;
      retryAt = nanotime() + retryDelay;
      sourceChanged(v, current);
      continue;
    }

    blankSeen(v);
  }

done:
  lgSignalEvent(v->exited);
  return 0;
}

bool win32VBlank_create(const Win32VBlankParams * params,
    Win32VBlank ** result)
{
  if (!params || !result || params->sourceCount > WIN32_VBLANK_MAX_SOURCES)
    return false;

  for (unsigned i = 0; i < params->sourceCount; ++i)
    if (!params->sources[i].wait)
      return false;

  struct Win32VBlank * v = calloc(1, sizeof(*v));
  if (!v)
  {
    DEBUG_ERROR("Out of memory");
    return false;
  }

  v->params = *params;
  atomic_init(&v->stop, false);
  atomic_init(&v->flags, LG_DS_WAIT_FRAME_NONE);
  atomic_init(&v->nominal, params->nominalPeriod);
  atomic_init(&v->measured, 0);
  atomic_init(&v->source, 0);
  atomic_init(&v->tickTime, 0);
  atomic_init(&v->tickReal, 0);
  v->createdReal = nanotime();
  LG_LOCK_INIT(v->lock);

  v->event  = lgCreateEvent(true, 0);
  v->exited = lgCreateEvent(false, 0);
  if (!v->event || !v->exited)
  {
    DEBUG_ERROR("Failed to create the events of the vertical blank");
    goto fail;
  }

  if (!lgCreateThread("Win32VBlank", vblankThread, v, &v->thread))
  {
    DEBUG_ERROR("Failed to create the thread of the vertical blank");
    goto fail;
  }

  *result = v;
  return true;

fail:
  if (v->event)
    lgFreeEvent(v->event);
  if (v->exited)
    lgFreeEvent(v->exited);
  free(v);
  return false;
}

bool win32VBlank_destroy(Win32VBlank ** vblank)
{
  if (!vblank || !*vblank)
    return true;

  struct Win32VBlank * v = *vblank;
  *vblank = NULL;

  atomic_store_explicit(&v->stop, true, memory_order_release);
  lgSignalEvent(v->event);

  if (!lgWaitEvent(v->exited, STOP_WAIT_MS))
  {
    // a display that is off can keep a blank from ever coming, and the thread
    // still uses what it was given: leave both, as the program is ending
    DEBUG_WARN("The vertical blank source did not return, leaving its thread");

    struct Win32VBlank * head = atomic_load_explicit(&g_abandoned,
        memory_order_relaxed);
    do
      v->abandoned = head;
    while (!atomic_compare_exchange_weak_explicit(&g_abandoned, &head, v,
        memory_order_release, memory_order_relaxed));
    return false;
  }

  int code;
  lgJoinThread(v->thread, &code);
  lgFreeEvent(v->event);
  lgFreeEvent(v->exited);
  LG_LOCK_FREE(v->lock);
  free(v);
  return true;
}

LG_DSWaitFrameResult win32VBlank_wait(Win32VBlank * v)
{
  for (;;)
  {
    uint64_t period = 0;
    win32VBlank_getPeriod(v, &period);
    if (!period)
      period = DEFAULT_PERIOD;

    // no source is in use, and the thread wakes this one by the clock
    if (atomic_load_explicit(&v->source, memory_order_acquire) >=
        v->params.sourceCount)
    {
      lgWaitEvent(v->event, TIMEOUT_INFINITE);
      break;
    }

    const uint64_t least = v->params.stallMin ?
      v->params.stallMin : STALL_MIN;
    uint64_t limit = period * STALL_PERIODS;
    if (limit < least)
      limit = least;

    // how long it is since there was a blank, which is since the start if there
    // was none
    uint64_t last = atomic_load_explicit(&v->tickReal, memory_order_acquire);
    if (!last)
      last = v->createdReal;
    const uint64_t now     = nanotime();
    const uint64_t silence = now > last ? now - last : 0;
    const bool     stalled = silence > limit;

    if (stalled != v->stalled)
    {
      v->stalled = stalled;
      if (stalled)
      {
        LG_LOCK(v->lock);
        ++v->stalls;
        LG_UNLOCK(v->lock);
        DEBUG_WARN("No vertical blank for %.0f ms, so rendering is paced by "
            "the clock until one comes", (double)silence / 1e6);
      }
      else
        DEBUG_INFO("The vertical blank is back");
    }

    // while the blanks come, wait for the next one, until it would be late
    // enough to be a stall; while they do not, wake by the clock
    uint64_t timeout = stalled ? period : limit - silence + 1;
    unsigned timeoutMs = (unsigned)((timeout + 999999) / 1000000);
    if (!timeoutMs)
      timeoutMs = 1;

    if (lgWaitEvent(v->event, timeoutMs))
      break;

    if (stalled)
      return LG_DS_WAIT_FRAME_NONE;
  }

  const LG_DSWaitFrameResult result =
    (LG_DSWaitFrameResult)atomic_exchange_explicit(&v->flags,
      LG_DS_WAIT_FRAME_NONE, memory_order_acquire);

  // the blank that let this thread go, and how long that took
  v->wakeAt = 0;
  if (result & LG_DS_WAIT_FRAME_CADENCE)
  {
    const uint64_t tick = atomic_load_explicit(&v->tickTime,
        memory_order_acquire);
    const uint64_t now  = clockNow(v);
    if (tick && now >= tick)
    {
      v->wakeAt = tick;

      LG_LOCK(v->lock);
      histogramAdd(&v->wakes, now - tick);
      LG_UNLOCK(v->lock);
    }
  }

  return result;
}

void win32VBlank_interrupt(Win32VBlank * v)
{
  atomic_fetch_or_explicit(&v->flags, LG_DS_WAIT_FRAME_INTERRUPTED,
      memory_order_release);
  lgSignalEvent(v->event);
}

void win32VBlank_frameSubmitted(Win32VBlank * v)
{
  if (!v->wakeAt)
    return;

  const uint64_t now  = clockNow(v);
  const uint64_t tick = v->wakeAt;
  v->wakeAt           = 0;
  if (now < tick)
    return;

  uint64_t period = 0;
  win32VBlank_getPeriod(v, &period);

  LG_LOCK(v->lock);
  histogramAdd(&v->submits, now - tick);
  ++v->frames;
  if (period && now - tick > period)
    ++v->late;
  LG_UNLOCK(v->lock);
}

bool win32VBlank_getPeriod(Win32VBlank * v, uint64_t * period)
{
  uint64_t value = atomic_load_explicit(&v->measured, memory_order_acquire);
  if (!value)
    value = atomic_load_explicit(&v->nominal, memory_order_relaxed);
  if (!value)
    return false;

  *period = value;
  return true;
}

void win32VBlank_setNominalPeriod(Win32VBlank * v, uint64_t period)
{
  LG_LOCK(v->lock);
  atomic_store_explicit(&v->nominal, period, memory_order_relaxed);
  resetMeasurement(v);
  LG_UNLOCK(v->lock);
}

void win32VBlank_getStats(Win32VBlank * v, Win32VBlankStats * stats)
{
  memset(stats, 0, sizeof(*stats));

  const unsigned current = atomic_load_explicit(&v->source,
      memory_order_acquire);
  stats->source = current < v->params.sourceCount ?
    v->params.sources[current].name : NULL;

  stats->measured = atomic_load_explicit(&v->measured,
      memory_order_acquire) != 0;
  win32VBlank_getPeriod(v, &stats->period);

  LG_LOCK(v->lock);
  stats->ticks  = v->ticks;
  stats->missed = v->missed;
  stats->early  = v->early;
  stats->frames = v->frames;
  stats->late   = v->late;
  stats->stalls = v->stalls;

  stats->mean = v->intervals.count ?
    v->intervals.sum / v->intervals.count : 0;
  stats->p50  = histogramPercentile(&v->intervals, 50);
  stats->p95  = histogramPercentile(&v->intervals, 95);
  stats->p99  = histogramPercentile(&v->intervals, 99);
  stats->max  = v->intervals.max;

  stats->wakeP50 = histogramPercentile(&v->wakes, 50);
  stats->wakeP95 = histogramPercentile(&v->wakes, 95);
  stats->wakeP99 = histogramPercentile(&v->wakes, 99);
  stats->wakeMax = v->wakes.max;

  stats->submitP50 = histogramPercentile(&v->submits, 50);
  stats->submitP95 = histogramPercentile(&v->submits, 95);
  stats->submitP99 = histogramPercentile(&v->submits, 99);
  stats->submitMax = v->submits.max;
  LG_UNLOCK(v->lock);
}
