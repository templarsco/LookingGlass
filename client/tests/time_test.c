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

/* What microtime() makes of the performance counter on Windows: the counter's
 * frequency is not a whole number of MHz on every PC, so the conversion is
 * checked against exact arithmetic for the frequencies that PCs have. */

#include "test.h"

#include "common/time.h"

#include <stdint.h>

// the exact result, which the 64 bit arithmetic of the conversion must equal
static uint64_t exact(uint64_t ticks, uint64_t freq)
{
  return (uint64_t)((unsigned __int128)ticks * 1000000U / freq);
}

static uint64_t rng = UINT64_C(0x9e3779b97f4a7c15);

static uint64_t next(void)
{
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

static const uint64_t frequencies[] =
{
  1000000,     // the least that Windows documents
  3579545,     // the ACPI timer, which the performance counter used to read
  10000000,    // Windows 10 and later on most PCs, and Hyper-V guests
  19200000,    // Arm
  24000000,
  25000000,
  3000000000,  // the TSC of a PC where Windows reads it as such
  3993600000,
  999999937,   // a prime, so that nothing divides evenly
};

static void testWholeSeconds(void)
{
  // a second of ticks is a million microseconds, at any frequency. The
  // microtime() that divided by the whole MHz made it 1.19 s at 3.579545 MHz
  // and 1.01 s at 19.2 MHz
  for (size_t i = 0; i < sizeof(frequencies) / sizeof(*frequencies); ++i)
  {
    const uint64_t freq = frequencies[i];
    CHECK(ticksToMicroseconds(0, freq)        == 0);
    CHECK(ticksToMicroseconds(freq, freq)     == 1000000);
    CHECK(ticksToMicroseconds(freq * 7, freq) == 7000000);
    CHECK(ticksToMicroseconds(freq * 86400 * 365, freq) ==
        UINT64_C(31536000) * 1000000);
  }
}

static void testPartsOfASecond(void)
{
  CHECK(ticksToMicroseconds(1, 10000000) == 0);            // 0.1 us
  CHECK(ticksToMicroseconds(9, 10000000) == 0);
  CHECK(ticksToMicroseconds(10, 10000000) == 1);
  CHECK(ticksToMicroseconds(3579545 / 2, 3579545) == 499999);  // rounds down
  CHECK(ticksToMicroseconds(3579545 / 2 + 1, 3579545) == 500000);
  CHECK(ticksToMicroseconds(19200, 19200000) == 1000);     // 1 ms
}

static void testExact(void)
{
  // ticks that are random, from a few to as many as 64 bits hold. The result
  // is never more than the ticks at 1 MHz or more, so it cannot wrap
  for (size_t i = 0; i < sizeof(frequencies) / sizeof(*frequencies); ++i)
  {
    const uint64_t freq = frequencies[i];
    for (unsigned n = 0; n < 200000; ++n)
    {
      const unsigned bits  = next() % 64 + 1;
      const uint64_t ticks = next() >> (64 - bits);
      CHECK(ticksToMicroseconds(ticks, freq) == exact(ticks, freq));
    }
  }

  // and frequencies that are random
  for (unsigned n = 0; n < 200000; ++n)
  {
    const uint64_t freq  = 1000000 + next() % 4000000000U;
    const uint64_t ticks = next() % (freq * UINT64_C(1000000000));
    CHECK(ticksToMicroseconds(ticks, freq) == exact(ticks, freq));
  }
}

static void testLongUptime(void)
{
  // 400 days of a counter at 10 MHz is 3.5e14 ticks, whose product with a
  // million wraps 64 bits: the conversion must not
  const uint64_t freq  = 10000000;
  const uint64_t ticks = freq * 86400 * 400 + 1234567;
  CHECK(ticks * 1000000U / 1000000U != ticks);   // the product does wrap
  CHECK(ticksToMicroseconds(ticks, freq) ==
      UINT64_C(86400) * 400 * 1000000 + 123456);
}

static void testNeverBackwards(void)
{
  // the result never goes back as the ticks go up, which a clock must not do
  for (size_t i = 0; i < sizeof(frequencies) / sizeof(*frequencies); ++i)
  {
    const uint64_t freq = frequencies[i];
    uint64_t       last = 0;
    for (uint64_t ticks = freq - 5; ticks < freq * 3; ticks += freq / 997 + 1)
    {
      const uint64_t us = ticksToMicroseconds(ticks, freq);
      CHECK(us >= last);
      last = us;
    }
  }
}

int main(void)
{
  testWholeSeconds();
  testPartsOfASecond();
  testExact();
  testLongUptime();
  testNeverBackwards();
  puts("time tests passed");
  return EXIT_SUCCESS;
}
