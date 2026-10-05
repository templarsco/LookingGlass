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

// The pacing behind the Win32 display server's waitFrame, with sources of
// vertical blanks and a clock that the test makes up

#include "vblank.h"
#include "test.h"

#include "common/debug.h"
#include "common/event.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NS_MS(x) ((uint64_t)(x) * UINT64_C(1000000))
#define NS_US(x) ((uint64_t)(x) * UINT64_C(1000))

// 240 Hz as DWM reports it, which is not the 4166667 ns of a whole 240
#define PERIOD_240 UINT64_C(4167300)

// the clock that the code under test reads, which the test moves
static _Atomic(uint64_t) g_now;

static uint64_t fakeNow(void)
{
  return atomic_load(&g_now);
}

static void setNow(uint64_t now)
{
  atomic_store(&g_now, now);
}

struct Fake
{
  const char * name;
  LGEvent    * event;
  atomic_int   pending;    // blanks that come without a wait for them
  atomic_int   calls;
  atomic_int   returns;    // calls that have returned
  atomic_int   failAfter;  // the calls after this many fail, if not -1
  atomic_bool  block;      // does not return, until it is released
};

static void fakeInit(struct Fake * fake, const char * name)
{
  memset(fake, 0, sizeof(*fake));
  fake->name  = name;
  fake->event = lgCreateEvent(true, 0);
  CHECK(fake->event);
  atomic_init(&fake->pending, 0);
  atomic_init(&fake->calls, 0);
  atomic_init(&fake->returns, 0);
  atomic_init(&fake->failAfter, -1);
  atomic_init(&fake->block, false);
}

static void fakeFree(struct Fake * fake)
{
  lgFreeEvent(fake->event);
}

static bool fakeWaitFor(struct Fake * fake, int call)
{
  for (;;)
  {
    const int failAfter = atomic_load(&fake->failAfter);
    if (failAfter >= 0 && call > failAfter)
      return false;

    if (!atomic_load(&fake->block))
    {
      int pending = atomic_load(&fake->pending);
      while (pending > 0)
        if (atomic_compare_exchange_weak(&fake->pending, &pending,
              pending - 1))
          return true;
    }

    // a timeout, so that a release is seen
    lgWaitEvent(fake->event, 20);
  }
}

static bool fakeWait(void * opaque)
{
  struct Fake * fake = opaque;
  const bool result = fakeWaitFor(fake,
      atomic_fetch_add(&fake->calls, 1) + 1);

  // the last of what this does with the fake
  atomic_fetch_add(&fake->returns, 1);
  return result;
}

static void fakeBlanks(struct Fake * fake, int count)
{
  atomic_fetch_add(&fake->pending, count);
  lgSignalEvent(fake->event);
}

// makes the source return, as a real one does at the next blank, so that
// destroy does not wait for it
static void fakeRelease(struct Fake * fake)
{
  atomic_store(&fake->failAfter, 0);
  lgSignalEvent(fake->event);
}

static Win32VBlank * make(struct Fake ** fakes, unsigned count,
    uint64_t nominal)
{
  // a stall is for its own test, as these wait far longer than for one
  Win32VBlankParams params =
  {
    .nominalPeriod = nominal,
    .now           = fakeNow,
    .stallMin      = NS_MS(60000),
  };
  for (unsigned i = 0; i < count; ++i)
    params.sources[params.sourceCount++] = (Win32VBlankSource)
      { fakes[i]->name, fakeWait, fakes[i] };

  Win32VBlank * vblank = NULL;
  CHECK(win32VBlank_create(&params, &vblank));
  CHECK(vblank);
  return vblank;
}

static bool fakeReturned(void * opaque)
{
  const struct Fake * fake = opaque;
  return atomic_load(&fake->returns) == atomic_load(&fake->calls);
}

// polls for what the thread does on its own
static bool eventually(bool (*check)(void *), void * opaque)
{
  for (unsigned i = 0; i < 3000; ++i)
  {
    if (check(opaque))
      return true;
    usleep(1000);
  }
  return false;
}

struct TickCheck
{
  Win32VBlank  * vblank;
  unsigned long  ticks;
};

static bool ticksReached(void * opaque)
{
  struct TickCheck * check = opaque;
  Win32VBlankStats stats;
  win32VBlank_getStats(check->vblank, &stats);
  return stats.ticks >= check->ticks;
}

// a blank that the test waits for, at a time that it sets
static void blank(struct Fake * fake, Win32VBlank * vblank, uint64_t now)
{
  setNow(now);
  fakeBlanks(fake, 1);
  CHECK(win32VBlank_wait(vblank) == LG_DS_WAIT_FRAME_CADENCE);
}

static void testCadence(void)
{
  struct Fake fake;
  fakeInit(&fake, "fake");
  struct Fake * fakes[] = { &fake };
  Win32VBlank * vblank = make(fakes, 1, PERIOD_240);

  uint64_t now = NS_MS(1000);
  for (unsigned i = 0; i < 5; ++i)
  {
    blank(&fake, vblank, now);
    now += PERIOD_240;
  }

  Win32VBlankStats stats;
  win32VBlank_getStats(vblank, &stats);
  CHECK(stats.ticks == 5);
  CHECK(stats.source && strcmp(stats.source, "fake") == 0);
  CHECK(stats.missed == 0 && stats.early == 0);

  // the nominal period, as too few blanks are measured to say
  uint64_t period = 0;
  CHECK(win32VBlank_getPeriod(vblank, &period));
  CHECK(period == PERIOD_240);
  CHECK(!stats.measured);

  fakeRelease(&fake);
  CHECK(win32VBlank_destroy(&vblank));
  CHECK(!vblank);
  fakeFree(&fake);
}

// blanks that come while the thread that waits is busy are one
static void testCoalesce(void)
{
  struct Fake fake;
  fakeInit(&fake, "fake");
  struct Fake * fakes[] = { &fake };
  Win32VBlank * vblank = make(fakes, 1, PERIOD_240);

  setNow(NS_MS(500));
  fakeBlanks(&fake, 3);
  struct TickCheck check = { vblank, 3 };
  CHECK(eventually(ticksReached, &check));

  CHECK(win32VBlank_wait(vblank) == LG_DS_WAIT_FRAME_CADENCE);

  // none are left over: the next wait only returns for what is made now
  win32VBlank_interrupt(vblank);
  CHECK(win32VBlank_wait(vblank) == LG_DS_WAIT_FRAME_INTERRUPTED);

  fakeRelease(&fake);
  CHECK(win32VBlank_destroy(&vblank));
  fakeFree(&fake);
}

struct WaitTask
{
  Win32VBlank          * vblank;
  LG_DSWaitFrameResult   result;
  atomic_bool            done;
};

static void * waitThread(void * opaque)
{
  struct WaitTask * task = opaque;
  task->result = win32VBlank_wait(task->vblank);
  atomic_store(&task->done, true);
  return NULL;
}

static bool taskDone(void * opaque)
{
  return atomic_load(&((struct WaitTask *)opaque)->done);
}

static void testInterrupt(void)
{
  struct Fake fake;
  fakeInit(&fake, "fake");
  struct Fake * fakes[] = { &fake };
  Win32VBlank * vblank = make(fakes, 1, PERIOD_240);

  // an interrupt that comes before the wait is not lost
  win32VBlank_interrupt(vblank);
  CHECK(win32VBlank_wait(vblank) == LG_DS_WAIT_FRAME_INTERRUPTED);

  // and one that comes during it lets it go, without a cadence
  struct WaitTask task = { .vblank = vblank };
  atomic_init(&task.done, false);
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, waitThread, &task) == 0);
  usleep(50000);
  CHECK(!atomic_load(&task.done));
  win32VBlank_interrupt(vblank);
  CHECK(eventually(taskDone, &task));
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(task.result == LG_DS_WAIT_FRAME_INTERRUPTED);

  fakeRelease(&fake);
  CHECK(win32VBlank_destroy(&vblank));
  fakeFree(&fake);
}

// jitter as a display would have it: a few tens of microseconds either way
static int64_t jitter(unsigned i)
{
  const unsigned x = (i * 2654435761U) >> 8;
  return (int64_t)(x % 101) * 1000 - 50000;
}

static void testPeriod(void)
{
  struct Fake fake;
  fakeInit(&fake, "fake");
  struct Fake * fakes[] = { &fake };

  // the display mode's period is not known: the middle interval is
  // what the others are measured against
  Win32VBlank * vblank = make(fakes, 1, 0);

  uint64_t period = 0;
  CHECK(!win32VBlank_getPeriod(vblank, &period));

  uint64_t now = NS_MS(2000);
  unsigned outliers = 0;
  for (unsigned i = 0; i < 200; ++i)
  {
    blank(&fake, vblank, now);

    // the first blank has no interval, and the display skips two
    uint64_t interval = (uint64_t)((int64_t)PERIOD_240 + jitter(i));
    if (i == 100 || i == 150)
    {
      interval *= 2;
      ++outliers;
    }
    now += interval;
  }

  Win32VBlankStats stats;
  win32VBlank_getStats(vblank, &stats);
  CHECK(stats.ticks == 200);
  CHECK(stats.measured);
  CHECK(win32VBlank_getPeriod(vblank, &period));

  // the average of the jitter is not exactly zero, and the missed blanks
  // do not count: the period is within a tenth of a percent
  const int64_t error = (int64_t)period - (int64_t)PERIOD_240;
  CHECK(error > -(int64_t)(PERIOD_240 / 1000) &&
      error < (int64_t)(PERIOD_240 / 1000));

  CHECK(stats.missed == outliers);
  CHECK(stats.early == 0);

  // the statistics are of the whole run: the middle interval is a period, and
  // the greatest is a missed blank
  CHECK(stats.p50 > PERIOD_240 - NS_US(60) && stats.p50 < PERIOD_240 + NS_US(60));
  CHECK(stats.max > PERIOD_240 * 3 / 2);
  CHECK(stats.p95 < PERIOD_240 * 3 / 2);

  fakeRelease(&fake);
  CHECK(win32VBlank_destroy(&vblank));
  fakeFree(&fake);
}

// the display mode changed, or the window went to another display
static void testReset(void)
{
  struct Fake fake;
  fakeInit(&fake, "fake");
  struct Fake * fakes[] = { &fake };
  Win32VBlank * vblank = make(fakes, 1, PERIOD_240);

  uint64_t now = NS_MS(3000);
  for (unsigned i = 0; i < 100; ++i)
  {
    blank(&fake, vblank, now);
    now += PERIOD_240;
  }

  Win32VBlankStats stats;
  win32VBlank_getStats(vblank, &stats);
  CHECK(stats.measured);

  const uint64_t slower = UINT64_C(6950000);
  win32VBlank_setNominalPeriod(vblank, slower);

  uint64_t period = 0;
  CHECK(win32VBlank_getPeriod(vblank, &period));
  CHECK(period == slower);
  win32VBlank_getStats(vblank, &stats);
  CHECK(!stats.measured);
  CHECK(stats.ticks == 0);

  // and the new rate is measured, from the first interval of it
  for (unsigned i = 0; i < 100; ++i)
  {
    blank(&fake, vblank, now);
    now += slower;
  }
  win32VBlank_getStats(vblank, &stats);
  CHECK(stats.measured);
  CHECK(stats.period == slower);
  CHECK(stats.missed == 0 && stats.early == 0);

  fakeRelease(&fake);
  CHECK(win32VBlank_destroy(&vblank));
  fakeFree(&fake);
}

struct SourceCheck
{
  Win32VBlank * vblank;
  const char  * name;
};

static bool sourceIs(void * opaque)
{
  struct SourceCheck * check = opaque;
  Win32VBlankStats stats;
  win32VBlank_getStats(check->vblank, &stats);
  return stats.source && strcmp(stats.source, check->name) == 0;
}

// a source that cannot say is dropped, and the next one is used
static void testFallback(void)
{
  struct Fake first, second;
  fakeInit(&first, "first");
  fakeInit(&second, "second");
  struct Fake * fakes[] = { &first, &second };
  Win32VBlank * vblank = make(fakes, 2, PERIOD_240);

  blank(&first, vblank, NS_MS(4000));
  struct SourceCheck onFirst = { vblank, "first" };
  CHECK(sourceIs(&onFirst));

  // its next wait fails, which the thread sees within a few milliseconds
  atomic_store(&first.failAfter, 1);
  struct SourceCheck onSecond = { vblank, "second" };
  CHECK(eventually(sourceIs, &onSecond));
  CHECK(atomic_load(&first.calls) == 2);

  // and what the second says is the cadence, with the measuring started again
  Win32VBlankStats stats;
  win32VBlank_getStats(vblank, &stats);
  CHECK(stats.ticks == 0);
  uint64_t now = NS_MS(4100);
  blank(&second, vblank, now);
  now += PERIOD_240;
  blank(&second, vblank, now);
  win32VBlank_getStats(vblank, &stats);
  CHECK(stats.ticks == 2);

  // the first is not asked again
  CHECK(atomic_load(&first.calls) == 2);

  fakeRelease(&second);
  CHECK(win32VBlank_destroy(&vblank));
  fakeFree(&first);
  fakeFree(&second);
}

// no source can say: wake at the rate of the display, and not as its cadence
static void testClock(void)
{
  struct Fake fake;
  fakeInit(&fake, "fake");
  atomic_store(&fake.failAfter, 0);
  struct Fake * fakes[] = { &fake };
  Win32VBlank * vblank = make(fakes, 1, NS_MS(2));

  for (unsigned i = 0; i < 5; ++i)
    CHECK(win32VBlank_wait(vblank) == LG_DS_WAIT_FRAME_NONE);

  Win32VBlankStats stats;
  win32VBlank_getStats(vblank, &stats);
  CHECK(stats.source == NULL);
  CHECK(stats.ticks == 0);

  // an interrupt still gets through
  win32VBlank_interrupt(vblank);
  LG_DSWaitFrameResult result = win32VBlank_wait(vblank);
  CHECK(result & LG_DS_WAIT_FRAME_INTERRUPTED);
  CHECK(!(result & LG_DS_WAIT_FRAME_CADENCE));

  fakeRelease(&fake);
  CHECK(win32VBlank_destroy(&vblank));
  fakeFree(&fake);
}

// how long a frame took from the blank that it woke for
static void testSubmit(void)
{
  struct Fake fake;
  fakeInit(&fake, "fake");
  struct Fake * fakes[] = { &fake };
  Win32VBlank * vblank = make(fakes, 1, PERIOD_240);

  uint64_t now = NS_MS(5000);

  // a submit without a wake is not a frame
  win32VBlank_frameSubmitted(vblank);

  Win32VBlankStats stats;
  win32VBlank_getStats(vblank, &stats);
  CHECK(stats.frames == 0);

  // the render thread is let go 20 us after the blank, which is when the
  // clock reads it, and submits 300 us after
  for (unsigned i = 0; i < 99; ++i)
  {
    setNow(now);
    fakeBlanks(&fake, 1);
    struct TickCheck check = { vblank, i + 1 };
    CHECK(eventually(ticksReached, &check));
    setNow(now + NS_US(20));
    CHECK(win32VBlank_wait(vblank) == LG_DS_WAIT_FRAME_CADENCE);
    setNow(now + NS_US(300));
    win32VBlank_frameSubmitted(vblank);

    // only the first submit after a wake is the frame
    setNow(now + NS_MS(3));
    win32VBlank_frameSubmitted(vblank);
    now += PERIOD_240;
  }

  // one frame is not ready for the next blank
  setNow(now);
  fakeBlanks(&fake, 1);
  struct TickCheck check = { vblank, 100 };
  CHECK(eventually(ticksReached, &check));
  CHECK(win32VBlank_wait(vblank) == LG_DS_WAIT_FRAME_CADENCE);
  setNow(now + PERIOD_240 + NS_US(500));
  win32VBlank_frameSubmitted(vblank);

  win32VBlank_getStats(vblank, &stats);
  CHECK(stats.frames == 100);
  CHECK(stats.late == 1);
  CHECK(stats.wakeP50 >= NS_US(15) && stats.wakeP50 <= NS_US(25));
  CHECK(stats.wakeP99 <= NS_US(25));
  CHECK(stats.submitP50 >= NS_US(295) && stats.submitP50 <= NS_US(305));
  CHECK(stats.submitP95 <= NS_US(305));
  CHECK(stats.submitMax == PERIOD_240 + NS_US(500));

  fakeRelease(&fake);
  CHECK(win32VBlank_destroy(&vblank));
  fakeFree(&fake);
}

// a display that is off, or an adapter that has no blanks, gives none, and the
// render thread is not left waiting for one
static void testStall(void)
{
  struct Fake fake;
  fakeInit(&fake, "fake");
  atomic_store(&fake.block, true);

  Win32VBlankParams params =
  {
    .nominalPeriod = NS_MS(2),
    .now           = fakeNow,
    .stallMin      = NS_MS(30),
  };
  params.sources[params.sourceCount++] = (Win32VBlankSource)
    { "fake", fakeWait, &fake };
  Win32VBlank * vblank = NULL;
  CHECK(win32VBlank_create(&params, &vblank));

  // the first wait is the longest, until it is a stall, and then the next ones
  // are by the clock, a period each and not a cadence
  for (unsigned i = 0; i < 5; ++i)
    CHECK(win32VBlank_wait(vblank) == LG_DS_WAIT_FRAME_NONE);

  Win32VBlankStats stats;
  win32VBlank_getStats(vblank, &stats);
  CHECK(stats.stalls == 1);
  CHECK(stats.ticks == 0);

  // an interrupt gets through a stall
  win32VBlank_interrupt(vblank);
  CHECK(win32VBlank_wait(vblank) & LG_DS_WAIT_FRAME_INTERRUPTED);

  // the display comes on: the blank is the cadence, and the stall is over
  atomic_store(&fake.block, false);
  setNow(NS_MS(7000));
  fakeBlanks(&fake, 1);
  CHECK(win32VBlank_wait(vblank) == LG_DS_WAIT_FRAME_CADENCE);
  setNow(NS_MS(7004));
  blank(&fake, vblank, NS_MS(7004));
  win32VBlank_getStats(vblank, &stats);
  CHECK(stats.stalls == 1);
  CHECK(stats.ticks == 2);

  fakeRelease(&fake);
  CHECK(win32VBlank_destroy(&vblank));
  fakeFree(&fake);
}

// a display that is off can keep a blank from coming for as long as it is
// off, and destroy does not wait for that
static void testBlocked(void)
{
  struct Fake fake;
  fakeInit(&fake, "fake");
  atomic_store(&fake.block, true);
  struct Fake * fakes[] = { &fake };
  Win32VBlank * vblank = make(fakes, 1, PERIOD_240);
  usleep(50000);

  CHECK(!win32VBlank_destroy(&vblank));
  CHECK(!vblank);

  // the display comes on, and the thread that was left finishes. The fake is
  // not freed until the call that it was in has returned, as it uses it
  atomic_store(&fake.block, false);
  atomic_store(&fake.failAfter, 0);
  lgSignalEvent(fake.event);
  CHECK(eventually(fakeReturned, &fake));
  fakeFree(&fake);
}

static void testDestroy(void)
{
  // a destroy of nothing is nothing
  Win32VBlank * none = NULL;
  CHECK(win32VBlank_destroy(&none));

  // one right after the create
  struct Fake fake;
  fakeInit(&fake, "fake");
  struct Fake * fakes[] = { &fake };
  Win32VBlank * vblank = make(fakes, 1, PERIOD_240);
  fakeRelease(&fake);
  CHECK(win32VBlank_destroy(&vblank));
  fakeFree(&fake);

  // a source without a wait is refused
  Win32VBlankParams params = { .sourceCount = 1 };
  Win32VBlank * refused = NULL;
  CHECK(!win32VBlank_create(&params, &refused));
  CHECK(!refused);
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "cadence"  , testCadence   },
  { "coalesce" , testCoalesce  },
  { "interrupt", testInterrupt },
  { "period"   , testPeriod    },
  { "reset"    , testReset     },
  { "fallback" , testFallback  },
  { "clock"    , testClock     },
  { "submit"   , testSubmit    },
  { "stall"    , testStall     },
  { "blocked"  , testBlocked   },
  { "destroy"  , testDestroy   },
};

int main(int argc, char * argv[])
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return EXIT_FAILURE;
  }

  alarm(20);
  debug_init();
  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    if (strcmp(argv[1], tests[i].name) == 0)
    {
      tests[i].run();
      return EXIT_SUCCESS;
    }

  fprintf(stderr, "unknown test case: %s\n", argv[1]);
  return EXIT_FAILURE;
}
