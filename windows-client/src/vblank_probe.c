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

// Measures what a display of this PC offers a client that wants to render in
// step with it: the time between the vertical blanks that the graphics kernel
// reports, the time between the compositions that DWM reports, and DWM's own
// account of the refresh rate and of the frames it dropped. It opens no window
// and draws nothing, so it can run while the PC is in use.
//
//   lg-windows-client-vblank-probe [--seconds N] [--monitor N] [--list]

#include <windows.h>
#include <dwmapi.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the few definitions of the graphics kernel's thunks that this needs. They
// are in gdi32.dll, and MinGW has no d3dkmthk.h
typedef UINT D3DKMT_HANDLE;

typedef struct
{
  HDC           hDc;
  D3DKMT_HANDLE hAdapter;
  LUID          AdapterLuid;
  UINT          VidPnSourceId;
}
LgOpenAdapterFromHdc;

typedef struct
{
  D3DKMT_HANDLE hAdapter;
}
LgCloseAdapter;

typedef struct
{
  D3DKMT_HANDLE hAdapter;
  D3DKMT_HANDLE hDevice;
  UINT          VidPnSourceId;
}
LgWaitForVerticalBlank;

typedef struct
{
  D3DKMT_HANDLE hAdapter;
  UINT          VidPnSourceId;
  BOOL          InVerticalBlank;
  UINT          ScanLine;
}
LgGetScanLine;

LONG WINAPI D3DKMTOpenAdapterFromHdc(LgOpenAdapterFromHdc *);
LONG WINAPI D3DKMTCloseAdapter(const LgCloseAdapter *);
LONG WINAPI D3DKMTWaitForVerticalBlankEvent(const LgWaitForVerticalBlank *);
LONG WINAPI D3DKMTGetScanLine(LgGetScanLine *);

#define PROBE_TICKS     (240 * 120)
#define PROBE_MONITORS  16

static double g_ticksPerMs;

static double nowMs(void)
{
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart / g_ticksPerMs;
}

struct Stats
{
  size_t count, missed, early;
  double mean, min, max, p50, p95, p99, p999;
};

static int compareDouble(const void * a, const void * b)
{
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

// the intervals between the times, in milliseconds; a missed tick is an
// interval of more than one and a half periods, an early one of under half
static void intervals(const double * times, size_t count, double period,
    struct Stats * stats, double * scratch)
{
  memset(stats, 0, sizeof(*stats));
  if (count < 2)
    return;

  const size_t n = count - 1;
  double sum = 0;
  for (size_t i = 0; i < n; ++i)
  {
    scratch[i] = times[i + 1] - times[i];
    sum += scratch[i];
    if (period > 0)
    {
      if (scratch[i] > period * 1.5)
        ++stats->missed;
      else if (scratch[i] < period * 0.5)
        ++stats->early;
    }
  }

  qsort(scratch, n, sizeof(*scratch), compareDouble);
  stats->count = n;
  stats->mean  = sum / (double)n;
  stats->min   = scratch[0];
  stats->max   = scratch[n - 1];
  stats->p50   = scratch[n / 2];
  stats->p95   = scratch[(size_t)((double)n * 0.95)];
  stats->p99   = scratch[(size_t)((double)n * 0.99)];
  stats->p999  = scratch[(size_t)((double)n * 0.999)];
}

static void report(const char * name, const struct Stats * s)
{
  if (!s->count)
  {
    printf("  %-22s no ticks\n", name);
    return;
  }

  printf("  %-22s %6lu ticks, mean %.4f ms (%.3f Hz), p50 %.4f, p95 %.4f, "
      "p99 %.4f, p99.9 %.4f, min %.4f, max %.4f ms; missed %lu, early %lu\n",
      name, (unsigned long)s->count, s->mean, 1000.0 / s->mean, s->p50, s->p95, s->p99,
      s->p999, s->min, s->max, (unsigned long)s->missed,
      (unsigned long)s->early);
}

struct Monitor
{
  WCHAR  device[CCHDEVICENAME];
  RECT   rect;
  bool   primary;
  DWORD  hz;
};

struct MonitorList
{
  struct Monitor monitors[PROBE_MONITORS];
  int            count;
};

static BOOL CALLBACK monitorEnum(HMONITOR monitor, HDC dc, LPRECT rect,
    LPARAM param)
{
  struct MonitorList * list = (struct MonitorList *)param;
  if (list->count >= PROBE_MONITORS)
    return FALSE;

  MONITORINFOEXW info = { .cbSize = sizeof(info) };
  if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info))
    return TRUE;

  DEVMODEW mode = { .dmSize = sizeof(mode) };
  struct Monitor * m = &list->monitors[list->count++];
  wcsncpy(m->device, info.szDevice, CCHDEVICENAME - 1);
  m->device[CCHDEVICENAME - 1] = L'\0';
  m->rect    = info.rcMonitor;
  m->primary = info.dwFlags & MONITORINFOF_PRIMARY;
  m->hz      = EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS,
      &mode) ? mode.dmDisplayFrequency : 0;
  return TRUE;
}

static DWORD WINAPI watchdog(LPVOID param)
{
  Sleep((DWORD)(uintptr_t)param);
  fprintf(stderr, "the probe did not finish in time, so it stops\n");
  fflush(stderr);
  ExitProcess(3);
  return 0;
}

static void dwmInfo(DWM_TIMING_INFO * out)
{
  DWM_TIMING_INFO info = { .cbSize = sizeof(info) };
  const HRESULT hr = DwmGetCompositionTimingInfo(NULL, &info);
  if (FAILED(hr))
  {
    printf("  DwmGetCompositionTimingInfo failed: 0x%08lx\n",
        (unsigned long)hr);
    memset(out, 0, sizeof(*out));
    return;
  }

  *out = info;
  printf("  DWM composition: %" PRIu32 "/%" PRIu32 " = %.4f Hz, period %.4f ms\n",
      info.rateRefresh.uiNumerator, info.rateRefresh.uiDenominator,
      info.rateRefresh.uiDenominator ? (double)info.rateRefresh.uiNumerator /
        (double)info.rateRefresh.uiDenominator : 0.0,
      (double)info.qpcRefreshPeriod / g_ticksPerMs);
}

int main(int argc, char * argv[])
{
  double seconds = 3;
  int    monitorIndex = 0;
  bool   list = false;

  for (int i = 1; i < argc; ++i)
  {
    if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
      seconds = atof(argv[++i]);
    else if (strcmp(argv[i], "--monitor") == 0 && i + 1 < argc)
      monitorIndex = atoi(argv[++i]);
    else if (strcmp(argv[i], "--list") == 0)
      list = true;
    else
    {
      fprintf(stderr, "usage: %s [--seconds N] [--monitor N] [--list]\n",
          argv[0]);
      return 2;
    }
  }
  if (seconds < 0.5 || seconds > 60)
  {
    fprintf(stderr, "--seconds is from 0.5 to 60\n");
    return 2;
  }

  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  g_ticksPerMs = (double)frequency.QuadPart / 1000.0;

  struct MonitorList monitors = { 0 };
  EnumDisplayMonitors(NULL, NULL, monitorEnum, (LPARAM)&monitors);
  if (!monitors.count)
  {
    fprintf(stderr, "no display monitor\n");
    return 1;
  }

  printf("Monitors\n");
  for (int i = 0; i < monitors.count; ++i)
    printf("  %d: %ls %ldx%ld at %ld,%ld, %lu Hz by the display mode%s\n", i,
        monitors.monitors[i].device,
        monitors.monitors[i].rect.right - monitors.monitors[i].rect.left,
        monitors.monitors[i].rect.bottom - monitors.monitors[i].rect.top,
        monitors.monitors[i].rect.left, monitors.monitors[i].rect.top,
        (unsigned long)monitors.monitors[i].hz,
        monitors.monitors[i].primary ? ", primary" : "");
  if (list)
    return 0;

  if (monitorIndex < 0 || monitorIndex >= monitors.count)
  {
    fprintf(stderr, "there is no monitor %d\n", monitorIndex);
    return 2;
  }
  const struct Monitor * monitor = &monitors.monitors[monitorIndex];
  printf("\nMeasuring monitor %d, %ls, for %.1f s each way\n", monitorIndex,
      monitor->device, seconds);

  CreateThread(NULL, 0, watchdog,
      (LPVOID)(uintptr_t)((seconds * 3 + 15) * 1000), 0, NULL);

  double * kmt    = malloc(sizeof(double) * PROBE_TICKS);
  double * flush  = malloc(sizeof(double) * PROBE_TICKS);
  double * scratch = malloc(sizeof(double) * PROBE_TICKS);
  if (!kmt || !flush || !scratch)
  {
    fprintf(stderr, "out of memory\n");
    return 1;
  }

  DWM_TIMING_INFO dwm;
  printf("\n");
  dwmInfo(&dwm);
  const double dwmPeriod = dwm.qpcRefreshPeriod ?
    (double)dwm.qpcRefreshPeriod / g_ticksPerMs :
    (monitor->hz ? 1000.0 / (double)monitor->hz : 0.0);
  const double expected = monitor->hz ? 1000.0 / (double)monitor->hz : dwmPeriod;

  // the graphics kernel's vertical blank of this monitor
  size_t kmtCount = 0;
  LONG   kmtStatus = 0;
  LgOpenAdapterFromHdc adapter = { 0 };
  adapter.hDc = CreateDCW(monitor->device, monitor->device, NULL, NULL);
  if (!adapter.hDc)
  {
    printf("\nD3DKMT: CreateDC failed for %ls: %lu\n", monitor->device,
        GetLastError());
    kmtStatus = -1;
  }
  else
  {
    kmtStatus = D3DKMTOpenAdapterFromHdc(&adapter);
    if (kmtStatus)
      printf("\nD3DKMT: D3DKMTOpenAdapterFromHdc failed: 0x%08lx\n",
          (unsigned long)kmtStatus);
  }

  if (!kmtStatus)
  {
    printf("\nD3DKMT: adapter LUID %08lx-%08lx, source %u\n",
        (unsigned long)adapter.AdapterLuid.HighPart,
        (unsigned long)adapter.AdapterLuid.LowPart, adapter.VidPnSourceId);

    LgGetScanLine scan = { adapter.hAdapter, adapter.VidPnSourceId };
    const LONG scanStatus = D3DKMTGetScanLine(&scan);
    printf("  D3DKMTGetScanLine: 0x%08lx, scan line %u, in vertical blank %d\n",
        (unsigned long)scanStatus, scan.ScanLine, (int)scan.InVerticalBlank);

    const LgWaitForVerticalBlank wait =
      { adapter.hAdapter, 0, adapter.VidPnSourceId };
    const double end = nowMs() + seconds * 1000.0;
    while (nowMs() < end && kmtCount < PROBE_TICKS)
    {
      const LONG status = D3DKMTWaitForVerticalBlankEvent(&wait);
      if (status)
      {
        printf("  D3DKMTWaitForVerticalBlankEvent failed: 0x%08lx after %lu "
            "ticks\n", (unsigned long)status, (unsigned long)kmtCount);
        kmtStatus = status;
        break;
      }
      kmt[kmtCount++] = nowMs();
    }

    const LgCloseAdapter close = { adapter.hAdapter };
    D3DKMTCloseAdapter(&close);
  }
  if (adapter.hDc)
    DeleteDC(adapter.hDc);

  // DWM's composition
  size_t flushCount = 0;
  {
    const double end = nowMs() + seconds * 1000.0;
    while (nowMs() < end && flushCount < PROBE_TICKS)
    {
      if (FAILED(DwmFlush()))
      {
        printf("  DwmFlush failed after %lu ticks\n", (unsigned long)flushCount);
        break;
      }
      flush[flushCount++] = nowMs();
    }
  }

  struct Stats kmtStats, flushStats;
  intervals(kmt, kmtCount, expected, &kmtStats, scratch);
  intervals(flush, flushCount, expected, &flushStats, scratch);

  printf("\nIntervals, against %.4f ms from the display mode\n", expected);
  report("D3DKMT vertical blank", &kmtStats);
  report("DwmFlush", &flushStats);

  free(kmt);
  free(flush);
  free(scratch);
  return kmtStatus && !flushCount ? 1 : 0;
}
