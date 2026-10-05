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

/* The tests that run on a clock they set define clock_gettime(), which the
 * inline microtime() and nanotime() of common/time.h call on Linux. Windows
 * reads the performance counter there, unless LG_TEST_MOCK_CLOCK is defined,
 * and winpthreads' time.h defines clock_gettime() itself, inline. The build
 * includes this header first, with -include, in the tests that set the clock.
 * It lets winpthreads have its clock_gettime(), and renames every one that
 * follows, the test's and the one that common/time.h calls. */

#ifndef LG_CLIENT_TESTS_WINDOWS_MOCK_CLOCK_H
#define LG_CLIENT_TESTS_WINDOWS_MOCK_CLOCK_H

#include <time.h>

#define LG_TEST_MOCK_CLOCK
#define clock_gettime lgTestClockGettime

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

int lgTestClockGettime(clockid_t id, struct timespec * ts);

#endif
