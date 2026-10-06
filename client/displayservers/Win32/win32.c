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

/* The Windows display server.
 *
 * A dedicated thread owns the window and pumps its messages, like the X11
 * event thread, because the main thread does not pump messages while it
 * waits for the renderer, and the modal move and size loop would stall it.
 * Operations called on other threads post a message to the window thread
 * and never wait for it; the render thread calls some of them while it holds
 * the renderer lock.
 *
 * Sizes and positions reported to the core are in logical units, the
 * physical pixels divided by the monitor scale, as on Wayland. */

#include <windows.h>
#include <windowsx.h>
#include <GL/gl.h>
#include <GL/wglext.h>

#include "interface/displayserver.h"
#include "app.h"
#include "common/debug.h"
#include "common/thread.h"
#include "common/time.h"
#include "common/windebug.h"

#include "input_event.h"
#include "keymap.h"
#include "placement.h"
#include "vblank.h"
#include "vblank_source.h"

#include "resources/no-input-cursor/16.xcur.h"
#include "resources/no-input-cursor/32.xcur.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_CLASS L"LookingGlassClient"

// the scan code a low level keyboard hook reports for the left control key
// that AltGr layouts send ahead of right alt
#define ALTGR_CONTROL_SCANCODE 0x21D

enum
{
  WM_LG_SET_POINTER = WM_APP,
  WM_LG_SET_SIZE,
  WM_LG_SET_FULLSCREEN,
  WM_LG_UPDATE_GRABS,
  WM_LG_IDLE,
  WM_LG_HOOK_KEY,
  WM_LG_SHUTDOWN,
  WM_LG_DESTROY,
};

static struct Win32DS
{
  LG_DSInitParams params;
  HINSTANCE       instance;
  bool            classRegistered;
  HWND            window;
  HDC             dc;
  LGThread      * thread;
  HANDLE          readyEvent;
  bool            initOk;
  HCURSOR         cursors[LG_POINTER_COUNT];
  HCURSOR         squareCursor;

  PFNWGLSWAPINTERVALEXTPROC swapInterval;
  bool                      swapIntervalChecked;

  /* the pacing of waitFrame, made when win:jitRender is on. The window thread
   * makes it and frees it with the display server, and the render thread
   * waits on it */
  Win32VBlank    * vblank;
  Win32KmtSource * kmt;
  uint64_t         nominalPeriod;  // window thread only

  /* shared with other threads */
  _Atomic(bool)      ready;
  _Atomic(bool)      stopping;
  _Atomic(unsigned)  dpi;
  _Atomic(int)       pointer;
  _Atomic(bool)      fullscreen;
  _Atomic(bool)      entered;
  _Atomic(bool)      wantKeyboard;
  _Atomic(bool)      wantCapture;
  _Atomic(bool)      clipped;
  _Atomic(uint64_t)  framePeriod;
  _Atomic(uintptr_t) keyboardLayout;
  _Atomic(uint64_t)  lastGuestWarp;

  /* window thread only */
  Win32Input      input;
  HHOOK           keyboardHook;
  bool            rawMouse;
  bool            absValid;
  double          absX, absY;
  bool            trackingLeave;
  bool            fullscreenApplied;
  LONG            restoreStyle;
  WINDOWPLACEMENT restorePlacement;
  HMONITOR        monitor;
  WCHAR           highSurrogate;
}
win32;

static inline bool stopped(void)
{
  return atomic_load_explicit(&win32.stopping, memory_order_acquire);
}

static inline double currentScale(void)
{
  return atomic_load_explicit(&win32.dpi, memory_order_relaxed) / 96.0;
}

static void postMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (win32.window)
    PostMessageW(win32.window, msg, wParam, lParam);
}

/* input sink, called on the window thread */

static void inputPosition(void * opaque, double x, double y)
{
  if (!stopped())
    app_updateCursorPos(x, y);
}

static void inputRelative(void * opaque, double x, double y,
    double rawX, double rawY)
{
  if (!stopped())
    app_handleMouseRelative(x, y, rawX, rawY);
}

static void inputButton(void * opaque, unsigned int button, bool pressed)
{
  if (stopped())
    return;

  if (pressed)
    app_handleButtonPress(button);
  else
    app_handleButtonRelease(button);
}

static void inputWheel(void * opaque, double motion)
{
  if (!stopped())
    app_handleWheelMotion(motion);
}

static void inputKey(void * opaque, int scancode, bool pressed)
{
  if (stopped())
    return;

  if (pressed)
    app_handleKeyPress(scancode);
  else
    app_handleKeyRelease(scancode);
}

static void inputText(void * opaque, const char * text)
{
  if (!stopped())
    app_handleKeyboardTyped(text);
}

static void inputModifiers(void * opaque, bool ctrl, bool shift,
    bool alt, bool super)
{
  if (!stopped())
    app_handleKeyboardModifiers(ctrl, shift, alt, super);
}

static void inputLEDs(void * opaque, bool numLock, bool capsLock,
    bool scrollLock)
{
  if (!stopped())
    app_handleKeyboardLEDs(numLock, capsLock, scrollLock);
}

static void inputEnter(void * opaque, bool entered)
{
  atomic_store_explicit(&win32.entered, entered, memory_order_release);
  if (!stopped())
    app_handleEnterEvent(entered);
}

static void inputFocus(void * opaque, bool focused)
{
  if (!stopped())
    app_handleFocusEvent(focused);
}

static const LG_DSInputSink inputSink =
{
  .position  = inputPosition,
  .relative  = inputRelative,
  .button    = inputButton,
  .wheel     = inputWheel,
  .key       = inputKey,
  .text      = inputText,
  .modifiers = inputModifiers,
  .leds      = inputLEDs,
  .enter     = inputEnter,
  .focus     = inputFocus,
};

/* cursors */

static uint32_t readU32(const unsigned char * data)
{
  return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
    (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

// creates a cursor from the first image of an Xcursor file
static HCURSOR loadXcursor(const char * file, size_t size)
{
  const unsigned char * data = (const unsigned char *)file;
  if (size < 16 || memcmp(data, "Xcur", 4) != 0)
    return NULL;

  const uint32_t headerSize = readU32(data + 4);
  const uint32_t tocCount   = readU32(data + 12);
  for (uint32_t i = 0; i < tocCount; ++i)
  {
    const size_t toc = (size_t)headerSize + (size_t)i * 12;
    if (toc + 12 > size || readU32(data + toc) != 0xfffd0002)
      continue;

    const size_t image = readU32(data + toc + 8);
    if (image + 36 > size)
      return NULL;

    const uint32_t width  = readU32(data + image + 16);
    const uint32_t height = readU32(data + image + 20);
    const uint32_t xhot   = readU32(data + image + 24);
    const uint32_t yhot   = readU32(data + image + 28);
    if (!width || !height || width > 256 || height > 256 ||
        image + 36 + (size_t)width * height * 4 > size)
      return NULL;

    BITMAPV5HEADER info =
    {
      .bV5Size        = sizeof(info),
      .bV5Width       = width,
      .bV5Height      = -(LONG)height,
      .bV5Planes      = 1,
      .bV5BitCount    = 32,
      .bV5Compression = BI_BITFIELDS,
      .bV5RedMask     = 0x00FF0000,
      .bV5GreenMask   = 0x0000FF00,
      .bV5BlueMask    = 0x000000FF,
      .bV5AlphaMask   = 0xFF000000,
    };

    void * bits;
    HDC screen = GetDC(NULL);
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO *)&info,
        DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!color)
      return NULL;

    // Xcursor pixels are premultiplied, Windows cursors are not
    uint32_t * out = bits;
    for (size_t p = 0; p < (size_t)width * height; ++p)
    {
      const uint32_t argb = readU32(data + image + 36 + p * 4);
      const uint32_t a = argb >> 24;
      if (a == 0 || a == 255)
      {
        out[p] = a ? argb : 0;
        continue;
      }

      const uint32_t r = min(((argb >> 16) & 0xFF) * 255 / a, 255U);
      const uint32_t g = min(((argb >>  8) & 0xFF) * 255 / a, 255U);
      const uint32_t b = min(( argb        & 0xFF) * 255 / a, 255U);
      out[p] = a << 24 | r << 16 | g << 8 | b;
    }

    HBITMAP mask = CreateBitmap(width, height, 1, 1, NULL);
    ICONINFO icon =
    {
      .fIcon    = FALSE,
      .xHotspot = xhot,
      .yHotspot = yhot,
      .hbmMask  = mask,
      .hbmColor = color,
    };
    HCURSOR cursor = mask ? CreateIconIndirect(&icon) : NULL;

    DeleteObject(color);
    if (mask)
      DeleteObject(mask);
    return cursor;
  }

  return NULL;
}

static void loadCursors(bool largeDot)
{
  static const LPCWSTR system[LG_POINTER_COUNT] =
  {
    [LG_POINTER_ARROW      ] = (LPCWSTR)IDC_ARROW,
    [LG_POINTER_INPUT      ] = (LPCWSTR)IDC_IBEAM,
    [LG_POINTER_MOVE       ] = (LPCWSTR)IDC_SIZEALL,
    [LG_POINTER_RESIZE_NS  ] = (LPCWSTR)IDC_SIZENS,
    [LG_POINTER_RESIZE_EW  ] = (LPCWSTR)IDC_SIZEWE,
    [LG_POINTER_RESIZE_NESW] = (LPCWSTR)IDC_SIZENESW,
    [LG_POINTER_RESIZE_NWSE] = (LPCWSTR)IDC_SIZENWSE,
    [LG_POINTER_HAND       ] = (LPCWSTR)IDC_HAND,
    [LG_POINTER_NOT_ALLOWED] = (LPCWSTR)IDC_NO,
  };

  for (int i = 0; i < LG_POINTER_COUNT; ++i)
    if (system[i])
      win32.cursors[i] = LoadCursorW(NULL, system[i]);

  if (largeDot)
    win32.squareCursor = loadXcursor(b_no_input_cursor_32_xcur,
        b_no_input_cursor_32_xcur_size);
  else
    win32.squareCursor = loadXcursor(b_no_input_cursor_16_xcur,
        b_no_input_cursor_16_xcur_size);

  if (!win32.squareCursor)
    DEBUG_WARN("Failed to load the no input cursor");

  // LG_POINTER_NONE stays NULL, which hides the cursor
  win32.cursors[LG_POINTER_SQUARE] = win32.squareCursor ?
    win32.squareCursor : win32.cursors[LG_POINTER_ARROW];
}

static bool cursorInClient(POINT * client)
{
  POINT pt;
  if (!GetCursorPos(&pt) || WindowFromPoint(pt) != win32.window)
    return false;

  RECT rect;
  ScreenToClient(win32.window, &pt);
  GetClientRect(win32.window, &rect);
  if (!PtInRect(&rect, pt))
    return false;

  if (client)
    *client = pt;
  return true;
}

static void applyCursor(void)
{
  if (GetCapture() == win32.window || cursorInClient(NULL))
    SetCursor(win32.cursors[atomic_load(&win32.pointer)]);
}

/* pointer tracking, window thread */

static void trackLeave(void)
{
  if (win32.trackingLeave)
    return;

  TRACKMOUSEEVENT track =
  {
    .cbSize    = sizeof(track),
    .dwFlags   = TME_LEAVE,
    .hwndTrack = win32.window,
  };
  win32.trackingLeave = TrackMouseEvent(&track);
}

static void checkHover(void)
{
  POINT pt;
  if (cursorInClient(&pt))
  {
    const double scale = currentScale();
    trackLeave();
    win32InputPointerEnter(&win32.input, pt.x / scale, pt.y / scale);
  }
  else
    win32InputPointerLeave(&win32.input);
}

static void clipToClient(void)
{
  RECT rect;
  GetClientRect(win32.window, &rect);
  MapWindowPoints(win32.window, NULL, (POINT *)&rect, 2);
  if (ClipCursor(&rect))
    atomic_store(&win32.clipped, true);
}

static void unclip(void)
{
  if (atomic_exchange(&win32.clipped, false))
    ClipCursor(NULL);
}

static bool registerRawMouse(bool enable)
{
  RAWINPUTDEVICE device =
  {
    .usUsagePage = 0x01, // generic desktop
    .usUsage     = 0x02, // mouse
    .dwFlags     = enable ? 0 : RIDEV_REMOVE,
    .hwndTarget  = enable ? win32.window : NULL,
  };

  if (!RegisterRawInputDevices(&device, 1, sizeof(device)))
  {
    DEBUG_WINERROR("RegisterRawInputDevices failed", GetLastError());
    return false;
  }
  return true;
}

static void handleRawMouse(const RAWMOUSE * mouse)
{
  if (mouse->usFlags & MOUSE_MOVE_ABSOLUTE)
  {
    // tablets and remote sessions report positions, turn them into motion
    const bool virtualDesktop = mouse->usFlags & MOUSE_VIRTUAL_DESKTOP;
    const double left   = virtualDesktop ?
      GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
    const double top    = virtualDesktop ?
      GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
    const double width  = GetSystemMetrics(virtualDesktop ?
        SM_CXVIRTUALSCREEN : SM_CXSCREEN);
    const double height = GetSystemMetrics(virtualDesktop ?
        SM_CYVIRTUALSCREEN : SM_CYSCREEN);

    const double x = left + mouse->lLastX / 65535.0 * width;
    const double y = top  + mouse->lLastY / 65535.0 * height;
    if (win32.absValid)
      win32InputRelativeMotion(&win32.input, x - win32.absX, y - win32.absY);

    win32.absX     = x;
    win32.absY     = y;
    win32.absValid = true;
    return;
  }

  win32.absValid = false;
  if (mouse->lLastX || mouse->lLastY)
    win32InputRelativeMotion(&win32.input, mouse->lLastX, mouse->lLastY);
}

/* keyboard, window thread */

static void reportKeyboardState(void)
{
  win32InputKeyboardState(&win32.input,
      GetKeyState(VK_NUMLOCK) & 1,
      GetKeyState(VK_CAPITAL) & 1,
      GetKeyState(VK_SCROLL ) & 1);
}

static bool isStateKey(int key)
{
  switch (key)
  {
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
    case KEY_CAPSLOCK:
    case KEY_NUMLOCK:
    case KEY_SCROLLLOCK:
      return true;

    default:
      return false;
  }
}

static int keyFromEvent(unsigned int vk, unsigned int scanCode, bool extended)
{
  // some injected input only carries a virtual key
  if (scanCode == 0)
  {
    const UINT mapped = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    scanCode = mapped & 0xFF;
    extended = (mapped & 0xFF00) == 0xE000;
  }

  return win32KeymapToLinux(vk, scanCode, extended);
}

static void handleKey(unsigned int vk, unsigned int scanCode, bool extended,
    bool pressed)
{
  const int key = keyFromEvent(vk, scanCode, extended);
  if (!key)
    return;

  // Windows takes the Print Screen press for itself and only reports the
  // release, so report both
  if (vk == VK_SNAPSHOT && !pressed &&
      !win32InputKeyHeld(&win32.input, key))
    win32InputKey(&win32.input, key, true);

  if (win32InputKey(&win32.input, key, pressed) && isStateKey(key))
    reportKeyboardState();
}

/* AltGr layouts send a left control press just ahead of right alt, with the
 * same time stamp; report only the right alt, as other platforms do. */
static bool isAltGrControl(unsigned int vk, bool extended)
{
  if (vk != VK_CONTROL || extended)
    return false;

  MSG next;
  if (!PeekMessageW(&next, NULL, 0, 0, PM_NOREMOVE))
    return false;

  return
    (next.message == WM_KEYDOWN || next.message == WM_SYSKEYDOWN ||
     next.message == WM_KEYUP   || next.message == WM_SYSKEYUP) &&
    next.wParam == VK_MENU && (HIWORD(next.lParam) & KF_EXTENDED) &&
    next.time == (DWORD)GetMessageTime();
}

static size_t heldKeys(int * keys, size_t capacity)
{
  size_t count = 0;
  for (unsigned int index = 1; index < 0x200 && count < capacity; ++index)
  {
    const unsigned int scanCode = index & 0xFF;
    const bool extended = index & 0x100;
    if (!scanCode)
      continue;

    const int key = win32KeymapToLinux(0, scanCode, extended);
    if (!key)
      continue;

    const UINT vk = MapVirtualKeyW(scanCode | (extended ? 0xE000 : 0),
        MAPVK_VSC_TO_VK_EX);
    if (vk && (GetKeyState(vk) & 0x8000))
      keys[count++] = key;
  }
  return count;
}

static LRESULT CALLBACK keyboardHook(int code, WPARAM wParam, LPARAM lParam)
{
  const KBDLLHOOKSTRUCT * info = (const KBDLLHOOKSTRUCT *)lParam;
  if (code != HC_ACTION || stopped() ||
      !atomic_load(&win32.wantKeyboard) ||
      GetForegroundWindow() != win32.window ||
      (info->flags & LLKHF_INJECTED))
    return CallNextHookEx(NULL, code, wParam, lParam);

  /* the lock keys go through so that the host keeps their state, which the
   * core mirrors into the guest; they arrive as window messages instead */
  switch (info->vkCode)
  {
    case VK_CAPITAL:
    case VK_NUMLOCK:
    case VK_SCROLL:
      return CallNextHookEx(NULL, code, wParam, lParam);
  }

  // swallow everything else, including the system shortcuts, for the guest
  if (info->scanCode != ALTGR_CONTROL_SCANCODE)
    postMessage(WM_LG_HOOK_KEY, info->vkCode,
        (info->scanCode & 0xFF) |
        ((info->flags & LLKHF_EXTENDED) ? 0x100 : 0) |
        ((info->flags & LLKHF_UP      ) ? 0     : 0x200));
  return 1;
}

/* window state, window thread */

static void updateFramePeriod(void)
{
  MONITORINFOEXW info = { .cbSize = sizeof(info) };
  DEVMODEW mode = { .dmSize = sizeof(mode) };
  uint64_t period = 0;

  const bool haveMonitor = GetMonitorInfoW(win32.monitor,
      (MONITORINFO *)&info);
  if (haveMonitor &&
      EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency > 1)
    period = UINT64_C(1000000000) / mode.dmDisplayFrequency;

  atomic_store(&win32.framePeriod, period);

  // the blanks that waitFrame waits for are the display's that the window is
  // on, which may not be the one that it was on or that was set up, and its
  // mode may have changed. The mode's rate is a whole number of hertz, so
  // what is measured takes over once there is enough of it
  if (haveMonitor && win32.kmt)
    win32KmtSource_retarget(win32.kmt, info.szDevice);
  if (win32.vblank && period != win32.nominalPeriod)
    win32VBlank_setNominalPeriod(win32.vblank, period);
  win32.nominalPeriod = period;
}

/* the thread that waits for the blanks has to wake as soon as one comes */
static void vblankThreadStart(void)
{
  if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST))
    DEBUG_WINERROR("Failed to raise the priority of the vertical blank thread",
        GetLastError());
}

/* window thread */
static void startVBlank(void)
{
  MONITORINFOEXW info = { .cbSize = sizeof(info) };
  Win32VBlankParams params =
  {
    .nominalPeriod = atomic_load(&win32.framePeriod),
    .threadStart   = vblankThreadStart,
  };

  if (GetMonitorInfoW(win32.monitor, (MONITORINFO *)&info))
  {
    win32.kmt = win32KmtSource_create(info.szDevice);
    if (win32.kmt)
      params.sources[params.sourceCount++] = (Win32VBlankSource)
        { "graphics kernel", win32KmtSource_wait, win32.kmt };
  }

  // for a session that cannot ask the graphics kernel, such as a remote one
  params.sources[params.sourceCount++] = (Win32VBlankSource)
    { "DWM", win32DwmSource_wait, NULL };

  win32.nominalPeriod = params.nominalPeriod;
  if (!win32VBlank_create(&params, &win32.vblank))
  {
    DEBUG_WARN("Failed to start the vertical blank pacing, waitFrame wakes by "
        "the clock");
    win32KmtSource_destroy(win32.kmt);
    win32.kmt = NULL;
  }
}

static void stopVBlank(void)
{
  if (!win32.vblank)
    return;

  Win32VBlankStats stats;
  win32VBlank_getStats(win32.vblank, &stats);
  if (stats.ticks)
    DEBUG_INFO("Vertical blank by %s: %lu blanks, %.4f ms (%.3f Hz)%s, "
        "p50 %.4f p95 %.4f p99 %.4f max %.4f ms, %lu missed, %lu early",
        stats.source ? stats.source : "the clock",
        stats.ticks, stats.period / 1e6, 1e9 / (double)stats.period,
        stats.measured ? " measured" : " of the display mode",
        stats.p50 / 1e6, stats.p95 / 1e6, stats.p99 / 1e6, stats.max / 1e6,
        stats.missed, stats.early);

  if (stats.stalls)
    DEBUG_WARN("The vertical blank stalled %lu times: no blank came for a long while",
        stats.stalls);

  if (stats.frames)
    DEBUG_INFO("Render after the blank: let go at p50 %.4f p95 %.4f p99 "
        "%.4f max %.4f ms, frame submitted at p50 %.4f p95 %.4f p99 %.4f "
        "max %.4f ms; %lu of %lu frames took over a period",
        stats.wakeP50 / 1e6, stats.wakeP95 / 1e6, stats.wakeP99 / 1e6,
        stats.wakeMax / 1e6, stats.submitP50 / 1e6, stats.submitP95 / 1e6,
        stats.submitP99 / 1e6, stats.submitMax / 1e6, stats.late,
        stats.frames);

  // a source that does not return keeps its thread, and what it uses
  if (win32VBlank_destroy(&win32.vblank))
    win32KmtSource_destroy(win32.kmt);
  win32.kmt = NULL;
}

static void updateMonitor(void)
{
  HMONITOR monitor = MonitorFromWindow(win32.window,
      MONITOR_DEFAULTTONEAREST);
  if (monitor == win32.monitor)
    return;

  win32.monitor = monitor;
  updateFramePeriod();
}

static void reportPosition(void)
{
  if (stopped())
    return;

  const double scale = currentScale();
  POINT origin = { 0, 0 };
  ClientToScreen(win32.window, &origin);
  app_updateWindowPos(lround(origin.x / scale), lround(origin.y / scale));
}

static void reportSize(void)
{
  if (stopped() || IsIconic(win32.window))
    return;

  RECT rect;
  GetClientRect(win32.window, &rect);
  if (rect.right <= 0 || rect.bottom <= 0)
    return;

  const double scale = currentScale();
  app_handleResizeEvent(
      max(1L, lround(rect.right  / scale)),
      max(1L, lround(rect.bottom / scale)),
      scale, (struct Border) { 0 });
  app_invalidateWindow(true);
  reportPosition();
}

static void applyFullscreen(bool fullscreen)
{
  if (fullscreen == win32.fullscreenApplied)
    return;

  if (fullscreen)
  {
    win32.restoreStyle = GetWindowLongW(win32.window, GWL_STYLE);
    win32.restorePlacement.length = sizeof(win32.restorePlacement);
    GetWindowPlacement(win32.window, &win32.restorePlacement);

    MONITORINFO info = { .cbSize = sizeof(info) };
    GetMonitorInfoW(MonitorFromWindow(win32.window,
          MONITOR_DEFAULTTONEAREST), &info);

    SetWindowLongW(win32.window, GWL_STYLE,
        win32.restoreStyle & ~(WS_OVERLAPPEDWINDOW | WS_POPUP));
    SetWindowPos(win32.window, HWND_TOP,
        info.rcMonitor.left, info.rcMonitor.top,
        info.rcMonitor.right  - info.rcMonitor.left,
        info.rcMonitor.bottom - info.rcMonitor.top,
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  else
  {
    SetWindowLongW(win32.window, GWL_STYLE, win32.restoreStyle);
    SetWindowPlacement(win32.window, &win32.restorePlacement);
    SetWindowPos(win32.window, NULL, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
        SWP_FRAMECHANGED);
  }

  win32.fullscreenApplied = fullscreen;
  atomic_store(&win32.fullscreen, fullscreen);
}

static void resizeClient(int width, int height)
{
  if (win32.fullscreenApplied || IsZoomed(win32.window) ||
      IsIconic(win32.window) || width <= 0 || height <= 0)
    return;

  const UINT dpi = atomic_load(&win32.dpi);
  RECT rect =
  {
    .right  = lround(width  * (dpi / 96.0)),
    .bottom = lround(height * (dpi / 96.0)),
  };
  AdjustWindowRectExForDpi(&rect, GetWindowLongW(win32.window, GWL_STYLE),
      FALSE, GetWindowLongW(win32.window, GWL_EXSTYLE), dpi);
  SetWindowPos(win32.window, NULL, 0, 0,
      rect.right - rect.left, rect.bottom - rect.top,
      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void updateGrabs(void)
{
  const bool active = !stopped() && win32.input.focused;

  const bool keyboard = active && atomic_load(&win32.wantKeyboard);
  if (keyboard && !win32.keyboardHook)
  {
    win32.keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook,
        win32.instance, 0);
    if (!win32.keyboardHook)
      DEBUG_WINERROR("Failed to grab the keyboard", GetLastError());
  }
  else if (!keyboard && win32.keyboardHook)
  {
    UnhookWindowsHookEx(win32.keyboardHook);
    win32.keyboardHook = NULL;
  }

  const bool capture = active && atomic_load(&win32.wantCapture);
  if (capture && !win32.rawMouse)
  {
    win32.rawMouse = registerRawMouse(true);
    win32.absValid = false;
  }
  else if (!capture && win32.rawMouse)
  {
    registerRawMouse(false);
    win32.rawMouse = false;
  }

  if (capture && win32.rawMouse)
  {
    // keep the hidden cursor over the window so clicks still land here
    clipToClient();
    win32InputSetCaptured(&win32.input, true);
  }
  else if (win32InputIsCaptured(&win32.input) || atomic_load(&win32.clipped))
  {
    win32InputSetCaptured(&win32.input, false);
    unclip();
    checkHover();
  }
}

static void setFocus(bool focused)
{
  int keys[128] = { 0 };
  const size_t count = focused ? heldKeys(keys, ARRAYSIZE(keys)) : 0;

  if (win32InputFocus(&win32.input, focused, keys, count) && focused)
    reportKeyboardState();

  if (!focused)
    win32.highSurrogate = 0;

  updateGrabs();
}

static void handleChar(WCHAR c)
{
  if (IS_HIGH_SURROGATE(c))
  {
    win32.highSurrogate = c;
    return;
  }

  WCHAR text[2] = { c };
  int length = 1;
  if (IS_LOW_SURROGATE(c))
  {
    if (!win32.highSurrogate)
      return;
    text[0] = win32.highSurrogate;
    text[1] = c;
    length  = 2;
  }
  win32.highSurrogate = 0;

  // editing keys reach the overlay as key events
  if (text[0] < 0x20 || text[0] == 0x7F || !app_isOverlayMode())
    return;

  char utf8[8];
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, length,
      utf8, sizeof(utf8) - 1, NULL, NULL);
  if (size <= 0)
    return;

  utf8[size] = '\0';
  win32InputText(&win32.input, utf8);
}

static void mouseButton(enum Win32InputButton button, bool pressed,
    LPARAM lParam)
{
  if (pressed)
  {
    // a click can arrive before any motion
    const double scale = currentScale();
    trackLeave();
    win32InputPointerEnter(&win32.input,
        GET_X_LPARAM(lParam) / scale, GET_Y_LPARAM(lParam) / scale);
    win32InputPointerButton(&win32.input, button, true);

    // keep receiving the mouse while a button is held outside the window
    if (win32.input.buttons && GetCapture() != win32.window)
      SetCapture(win32.window);
    return;
  }

  win32InputPointerButton(&win32.input, button, false);
  if (!win32.input.buttons && GetCapture() == win32.window)
  {
    ReleaseCapture();
    checkHover();
  }
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam,
    LPARAM lParam)
{
  if (!atomic_load(&win32.ready))
    return DefWindowProcW(hwnd, msg, wParam, lParam);

  switch (msg)
  {
    case WM_CLOSE:
      if (!stopped())
        app_handleCloseEvent();
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
      ValidateRect(hwnd, NULL);
      if (!stopped())
        app_invalidateWindow(false);
      return 0;

    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED)
      {
        reportSize();
        if (win32InputIsCaptured(&win32.input))
          clipToClient();
      }
      return 0;

    case WM_MOVE:
      updateMonitor();
      reportPosition();
      if (win32InputIsCaptured(&win32.input))
        clipToClient();
      return 0;

    case WM_DPICHANGED:
    {
      atomic_store(&win32.dpi, HIWORD(wParam));
      if (win32.fullscreenApplied)
      {
        applyFullscreen(false);
        applyFullscreen(true);
      }
      else
      {
        const RECT * rect = (const RECT *)lParam;
        SetWindowPos(hwnd, NULL, rect->left, rect->top,
            rect->right - rect->left, rect->bottom - rect->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
      }
      reportSize();
      return 0;
    }

    case WM_DISPLAYCHANGE:
      updateFramePeriod();
      if (win32.fullscreenApplied)
      {
        applyFullscreen(false);
        applyFullscreen(true);
      }
      return 0;

    case WM_ACTIVATE:
      if (LOWORD(wParam) != WA_INACTIVE &&
          win32InputIsCaptured(&win32.input))
        clipToClient();
      break;

    case WM_SETFOCUS:
      setFocus(true);
      return 0;

    case WM_KILLFOCUS:
      setFocus(false);
      return 0;

    case WM_SETCURSOR:
      if (LOWORD(lParam) == HTCLIENT)
      {
        SetCursor(win32.cursors[atomic_load(&win32.pointer)]);
        return TRUE;
      }
      break;

    case WM_MOUSEMOVE:
    {
      const POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      const double scale = currentScale();
      RECT rect;
      GetClientRect(hwnd, &rect);

      trackLeave();
      if (!PtInRect(&rect, pt) ||
          !win32InputPointerEnter(&win32.input, pt.x / scale, pt.y / scale))
        win32InputPointerMotion(&win32.input, pt.x / scale, pt.y / scale);
      return 0;
    }

    case WM_MOUSELEAVE:
      win32.trackingLeave = false;
      win32InputPointerLeave(&win32.input);
      return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
      mouseButton(WIN32_BUTTON_LEFT, msg == WM_LBUTTONDOWN, lParam);
      return 0;

    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
      mouseButton(WIN32_BUTTON_MIDDLE, msg == WM_MBUTTONDOWN, lParam);
      return 0;

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      mouseButton(WIN32_BUTTON_RIGHT, msg == WM_RBUTTONDOWN, lParam);
      return 0;

    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
      mouseButton(
          GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ?
            WIN32_BUTTON_X1 : WIN32_BUTTON_X2,
          msg == WM_XBUTTONDOWN, lParam);
      return TRUE;

    case WM_CAPTURECHANGED:
      // another window took the mouse while buttons were held
      if ((HWND)lParam != hwnd && win32.input.buttons)
      {
        win32InputReleaseButtons(&win32.input);
        checkHover();
      }
      return 0;

    case WM_MOUSEWHEEL:
      win32InputPointerWheel(&win32.input, GET_WHEEL_DELTA_WPARAM(wParam));
      return 0;

    case WM_INPUT:
    {
      RAWINPUT raw;
      UINT size = sizeof(raw);
      if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &size,
            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
          raw.header.dwType == RIM_TYPEMOUSE)
        handleRawMouse(&raw.data.mouse);
      break;
    }

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    {
      const WORD flags = HIWORD(lParam);
      const bool extended = flags & KF_EXTENDED;
      if (!isAltGrControl(wParam, extended))
        handleKey(wParam, LOBYTE(flags), extended, !(flags & KF_UP));

      // keep Alt+F4 and the system menu working when not grabbed
      if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
        break;
      return 0;
    }

    case WM_CHAR:
      handleChar((WCHAR)wParam);
      return 0;

    case WM_SYSCOMMAND:
      // a lone Alt or F10 would enter menu mode and swallow the next keys
      if ((wParam & 0xFFF0) == SC_KEYMENU && lParam != ' ')
        return 0;
      break;

    case WM_INPUTLANGCHANGE:
      atomic_store(&win32.keyboardLayout, (uintptr_t)lParam);
      break;

    case WM_LG_HOOK_KEY:
      handleKey(wParam, lParam & 0xFF, lParam & 0x100, lParam & 0x200);
      return 0;

    case WM_LG_SET_POINTER:
      applyCursor();
      return 0;

    case WM_LG_SET_SIZE:
      resizeClient((int)wParam, (int)lParam);
      return 0;

    case WM_LG_SET_FULLSCREEN:
      applyFullscreen(wParam);
      return 0;

    case WM_LG_UPDATE_GRABS:
      updateGrabs();
      return 0;

    case WM_LG_IDLE:
      SetThreadExecutionState(wParam ?
          ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED :
          ES_CONTINUOUS);
      return 0;

    case WM_LG_SHUTDOWN:
      // the core is going away; stop calling it and let the input go
      atomic_store(&win32.stopping, true);
      updateGrabs();
      unclip();
      if (GetCapture() == hwnd)
        ReleaseCapture();
      SetThreadExecutionState(ES_CONTINUOUS);
      return 0;

    case WM_LG_DESTROY:
      DestroyWindow(hwnd);
      return 0;
  }

  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static wchar_t * utf8ToWide(const char * str)
{
  const int length = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
  if (length <= 0)
    return NULL;

  wchar_t * out = malloc(length * sizeof(*out));
  if (out && !MultiByteToWideChar(CP_UTF8, 0, str, -1, out, length))
  {
    free(out);
    return NULL;
  }
  return out;
}

static bool setPixelFormat(void)
{
  PIXELFORMATDESCRIPTOR pfd =
  {
    .nSize      = sizeof(pfd),
    .nVersion   = 1,
    .dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
    .iPixelType = PFD_TYPE_RGBA,
    .cColorBits = 24,
    .cAlphaBits = 8,
    .iLayerType = PFD_MAIN_PLANE,
  };

  const int format = ChoosePixelFormat(win32.dc, &pfd);
  if (!format || !SetPixelFormat(win32.dc, format, &pfd))
  {
    DEBUG_WINERROR("Failed to set the OpenGL pixel format", GetLastError());
    return false;
  }

  if (DescribePixelFormat(win32.dc, format, sizeof(pfd), &pfd) &&
      (pfd.dwFlags & PFD_GENERIC_FORMAT) &&
      !(pfd.dwFlags & PFD_GENERIC_ACCELERATED))
    DEBUG_WARN("No OpenGL driver found, only the software renderer is "
        "available");

  return true;
}

#define MAX_WORK_AREAS 16

struct WorkAreas
{
  Win32Rect area[MAX_WORK_AREAS];
  size_t    count;
};

static BOOL CALLBACK collectWorkArea(HMONITOR monitor, HDC dc, LPRECT rect,
    LPARAM param)
{
  struct WorkAreas * list = (struct WorkAreas *)param;
  MONITORINFO info = { .cbSize = sizeof(info) };
  if (list->count < MAX_WORK_AREAS && GetMonitorInfoW(monitor, &info))
    list->area[list->count++] = (Win32Rect)
    {
      info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom
    };
  return TRUE;
}

static bool createWindow(void)
{
  const LG_DSInitParams * params = &win32.params;

  WNDCLASSEXW windowClass =
  {
    .cbSize        = sizeof(windowClass),
    .style         = CS_OWNDC,
    .lpfnWndProc   = windowProc,
    .hInstance     = win32.instance,
    .hIcon         = LoadIconW(win32.instance, MAKEINTRESOURCEW(32512)),
    .lpszClassName = WINDOW_CLASS,
  };

  if (!RegisterClassExW(&windowClass))
  {
    DEBUG_WINERROR("RegisterClassExW failed", GetLastError());
    return false;
  }
  win32.classRegistered = true;

  DWORD style = params->borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
  if (!params->resizable)
    style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
  style |= WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
  const DWORD exStyle = WS_EX_APPWINDOW;

  wchar_t * title = utf8ToWide(params->title ? params->title : "");
  win32.window = CreateWindowExW(exStyle, WINDOW_CLASS,
      title ? title : L"", style,
      params->center ? CW_USEDEFAULT : params->x,
      params->center ? CW_USEDEFAULT : params->y,
      params->w, params->h, NULL, NULL, win32.instance, NULL);
  free(title);

  if (!win32.window)
  {
    DEBUG_WINERROR("CreateWindowExW failed", GetLastError());
    return false;
  }

  // size the client area for the scale of the monitor the window is on
  const UINT dpi = GetDpiForWindow(win32.window);
  atomic_store(&win32.dpi, dpi ? dpi : 96);
  win32.monitor = MonitorFromWindow(win32.window, MONITOR_DEFAULTTOPRIMARY);

  RECT rect =
  {
    .right  = lround(params->w * currentScale()),
    .bottom = lround(params->h * currentScale()),
  };
  AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle,
      atomic_load(&win32.dpi));
  const int width  = rect.right  - rect.left;
  const int height = rect.bottom - rect.top;

  int x, y;
  if (params->center)
  {
    MONITORINFO info = { .cbSize = sizeof(info) };
    GetMonitorInfoW(win32.monitor, &info);
    x = info.rcWork.left + (info.rcWork.right  - info.rcWork.left - width ) / 2;
    y = info.rcWork.top  + (info.rcWork.bottom - info.rcWork.top  - height) / 2;
  }
  else
  {
    // the configured position is the client area origin
    x = params->x + rect.left;
    y = params->y + rect.top;

    // which is on no monitor if it was saved for one that is not there now,
    // and a window that is on no screen cannot be taken back by anyone
    struct WorkAreas monitors = { .count = 0 };
    EnumDisplayMonitors(NULL, NULL, collectWorkArea, (LPARAM)&monitors);

    Win32Rect placed = { x, y, x + width, y + height };
    if (win32Placement_fit(&placed, monitors.area, monitors.count))
    {
      DEBUG_WARN("The configured window position %d,%d is not on a monitor, "
          "using %d,%d", params->x, params->y,
          (int)(placed.left - rect.left), (int)(placed.top - rect.top));
      x = placed.left;
      y = placed.top;
    }
  }

  SetWindowPos(win32.window, NULL, x, y, width, height,
      SWP_NOZORDER | SWP_NOACTIVATE);

  win32.dc = GetDC(win32.window);
  if (!win32.dc || !setPixelFormat())
  {
    DestroyWindow(win32.window);
    win32.window = NULL;
    return false;
  }

  atomic_store(&win32.keyboardLayout,
      (uintptr_t)GetKeyboardLayout(GetCurrentThreadId()));
  updateFramePeriod();
  if (params->jitRender)
    startVBlank();

  atomic_store(&win32.ready, true);
  if (params->showInactive)
    ShowWindow(win32.window, SW_SHOWNOACTIVATE);
  else
  {
    ShowWindow(win32.window, params->maximize ? SW_SHOWMAXIMIZED : SW_SHOW);
    SetForegroundWindow(win32.window);
  }

  if (params->fullscreen)
    applyFullscreen(true);

  reportSize();
  checkHover();
  return true;
}

static int windowThread(void * opaque)
{
  win32.initOk = createWindow();
  SetEvent(win32.readyEvent);
  if (!win32.initOk)
    return 1;

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  // hooks and clipping belong to this thread, release them with it
  if (win32.keyboardHook)
  {
    UnhookWindowsHookEx(win32.keyboardHook);
    win32.keyboardHook = NULL;
  }
  unclip();
  return 0;
}

/* display server ops */

static void win32Setup(void)
{
}

static bool win32Probe(void)
{
  return true;
}

static bool win32EarlyInit(void)
{
  // the manifest asks for this too; setting it again only fails
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  win32KeymapInit();
  return true;
}

static void freeResources(void)
{
  if (win32.classRegistered)
  {
    UnregisterClassW(WINDOW_CLASS, win32.instance);
    win32.classRegistered = false;
  }

  if (win32.squareCursor)
  {
    DestroyCursor(win32.squareCursor);
    win32.squareCursor = NULL;
  }

  if (win32.readyEvent)
  {
    CloseHandle(win32.readyEvent);
    win32.readyEvent = NULL;
  }
}

static bool win32Init(const LG_DSInitParams params)
{
  win32.params   = params;
  win32.instance = GetModuleHandleW(NULL);
  win32InputInit(&win32.input, &inputSink, NULL);
  atomic_store(&win32.pointer, LG_POINTER_SQUARE);
  atomic_store(&win32.dpi, 96);
  loadCursors(params.largeCursorDot);

  win32.readyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!win32.readyEvent)
  {
    DEBUG_WINERROR("CreateEventW failed", GetLastError());
    freeResources();
    return false;
  }

  if (!lgCreateThread("win32Window", windowThread, NULL, &win32.thread))
  {
    DEBUG_ERROR("Failed to create the window thread");
    freeResources();
    return false;
  }

  WaitForSingleObject(win32.readyEvent, INFINITE);
  if (!win32.initOk)
  {
    lgJoinThread(win32.thread, NULL);
    win32.thread = NULL;
    freeResources();
    return false;
  }

  return true;
}

static void win32Startup(void)
{
}

static void win32Shutdown(void)
{
  if (!win32.window || stopped())
    return;

  // the window thread owns the hook and the pointer confinement
  SendMessageTimeoutW(win32.window, WM_LG_SHUTDOWN, 0, 0, SMTO_NORMAL, 5000,
      NULL);
  atomic_store(&win32.stopping, true);
}

static void win32Free(void)
{
  win32Shutdown();
  stopVBlank();

  if (win32.thread)
  {
    PostMessageW(win32.window, WM_LG_DESTROY, 0, 0);
    lgJoinThread(win32.thread, NULL);
    win32.thread = NULL;
  }

  win32.window = NULL;
  win32.dc     = NULL;
  freeResources();
}

static bool win32GetProp(LG_DSProperty prop, void * ret)
{
  switch (prop)
  {
    case LG_DS_WARP_SUPPORT:
      *(enum LG_DSWarpSupport *)ret = LG_DS_WARP_SCREEN;
      return true;

    case LG_DS_WINDOW_HIDDEN:
      // a minimized window is shown by nothing, however fast it is drawn
      *(bool *)ret = IsIconic(win32.window);
      return true;

    default:
      return false;
  }
}

#ifdef ENABLE_OPENGL
static LG_DSGLContext win32GLCreateContext(void)
{
  HGLRC context = wglCreateContext(win32.dc);
  if (!context)
    DEBUG_WINERROR("wglCreateContext failed", GetLastError());
  return (LG_DSGLContext)context;
}

static void win32GLDeleteContext(LG_DSGLContext context)
{
  wglMakeCurrent(NULL, NULL);
  wglDeleteContext((HGLRC)context);
}

static void win32GLMakeCurrent(LG_DSGLContext context)
{
  if (!wglMakeCurrent(context ? win32.dc : NULL, (HGLRC)context))
    DEBUG_WINERROR("wglMakeCurrent failed", GetLastError());
}

static void win32GLSetSwapInterval(int interval)
{
  if (!win32.swapIntervalChecked)
  {
    win32.swapInterval = (PFNWGLSWAPINTERVALEXTPROC)(void *)
      wglGetProcAddress("wglSwapIntervalEXT");
    win32.swapIntervalChecked = true;
  }

  if (!win32.swapInterval)
  {
    DEBUG_WARN("wglSwapIntervalEXT is not available, vsync is unchanged");
    return;
  }

  win32.swapInterval(interval);
}

static void win32GLSwapBuffers(void)
{
  SwapBuffers(win32.dc);

  // the frame that waitFrame let the render thread go for is submitted
  if (win32.vblank)
    win32VBlank_frameSubmitted(win32.vblank);
}
#endif

static bool win32GetFramePeriod(uint64_t * period)
{
  // measured from the blanks when there are enough, which the display mode's
  // whole hertz is not, and which the frame scheduler sums over time
  if (win32.vblank && win32VBlank_getPeriod(win32.vblank, period))
    return true;

  const uint64_t value = atomic_load(&win32.framePeriod);
  if (!value)
    return false;

  *period = value;
  return true;
}

static LG_DSWaitFrameResult win32WaitFrame(void)
{
  if (win32.vblank)
    return win32VBlank_wait(win32.vblank);

  // no pacing could be made, and this is called only with win:jitRender: wake
  // at the display's rate, and without claiming that it is the display's
  // cadence
  uint64_t period = 0;
  if (!win32GetFramePeriod(&period))
    period = UINT64_C(16666667);
  nsleep(period);
  return LG_DS_WAIT_FRAME_NONE;
}

static void win32SkipFrame(void)
{
  // there is nothing to arm again: the blanks come either way
}

static void win32StopWaitFrame(void)
{
  if (win32.vblank)
    win32VBlank_interrupt(win32.vblank);
}

static void win32SetCursorPos(double x, double y)
{
  const double scale = currentScale();
  POINT pt = { lround(x * scale), lround(y * scale) };
  ClientToScreen(win32.window, &pt);
  SetCursorPos(pt.x, pt.y);
}

static void win32GuestPointerUpdated(double x, double y, double localX,
    double localY)
{
  if (app_isCaptureMode() || !atomic_load(&win32.entered))
    return;

  // avoid running too often
  const uint64_t now  = microtime();
  const uint64_t last = atomic_load(&win32.lastGuestWarp);
  if (now - last < 10000)
    return;
  atomic_store(&win32.lastGuestWarp, now);

  win32SetCursorPos(localX, localY);
}

static void win32SetPointer(LG_DSPointer pointer)
{
  if (pointer < 0 || pointer >= LG_POINTER_COUNT)
    return;

  if (atomic_exchange(&win32.pointer, pointer) != (int)pointer)
    postMessage(WM_LG_SET_POINTER, 0, 0);
}

static void win32GrabKeyboard(void)
{
  if (!atomic_exchange(&win32.wantKeyboard, true))
    postMessage(WM_LG_UPDATE_GRABS, 0, 0);
}

static void win32UngrabKeyboard(void)
{
  if (atomic_exchange(&win32.wantKeyboard, false))
    postMessage(WM_LG_UPDATE_GRABS, 0, 0);
}

/* The core only confines the pointer in normal mode with LG_DS_WARP_SURFACE,
 * and this backend reports LG_DS_WARP_SCREEN. */
static void win32GrabPointer(void)
{
}

static void win32UngrabPointer(void)
{
}

static bool win32IsPointerGrabbed(void)
{
  return false;
}

static void win32CapturePointer(void)
{
  atomic_store(&win32.wantCapture, true);
  postMessage(WM_LG_UPDATE_GRABS, 0, 0);
}

static void win32UncapturePointer(void)
{
  /* release at once: the core warps the pointer right after this returns
   * and the confinement would hold it in place */
  atomic_store(&win32.wantCapture, false);
  win32InputSetCaptured(&win32.input, false);
  unclip();
  postMessage(WM_LG_UPDATE_GRABS, 0, 0);
}

static bool win32IsPointerCaptured(void)
{
  return win32InputIsCaptured(&win32.input);
}

static bool win32GetKeyLabel(int sc, char * label, size_t size)
{
  unsigned int scanCode;
  bool extended;
  if (!size || !win32KeymapFromLinux(sc, &scanCode, &extended))
    return false;

  const HKL layout = (HKL)atomic_load(&win32.keyboardLayout);
  const UINT vk = MapVirtualKeyExW(scanCode | (extended ? 0xE000 : 0),
      MAPVK_VSC_TO_VK_EX, layout);
  if (!vk)
    return false;

  // the high bit marks dead keys
  WCHAR c = (WCHAR)(MapVirtualKeyExW(vk, MAPVK_VK_TO_CHAR, layout) &
      0x7FFFFFFF);
  if (c <= 0x20 || c == 0x7F)
    return false;

  CharUpperBuffW(&c, 1);
  const int length = WideCharToMultiByte(CP_UTF8, 0, &c, 1, label,
      (int)size - 1, NULL, NULL);
  if (length <= 0)
    return false;

  label[length] = '\0';
  return true;
}

static void win32WarpPointer(int x, int y, bool exiting)
{
  win32SetCursorPos(x, y);
}

static void win32RealignPointer(void)
{
  app_handleMouseRelative(0.0, 0.0, 0.0, 0.0);
}

static bool win32IsValidPointerPos(int x, int y)
{
  const double scale = currentScale();
  const POINT pt = { lround(x * scale), lround(y * scale) };
  return MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) != NULL;
}

static void win32RequestActivation(void)
{
  FLASHWINFO info =
  {
    .cbSize  = sizeof(info),
    .hwnd    = win32.window,
    .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG,
  };
  FlashWindowEx(&info);
}

// the execution state belongs to a thread, so the window thread keeps it
static void win32InhibitIdle(void)
{
  postMessage(WM_LG_IDLE, 1, 0);
}

static void win32UninhibitIdle(void)
{
  postMessage(WM_LG_IDLE, 0, 0);
}

static void win32Wait(unsigned int time)
{
  Sleep(time);
}

static void win32SetWindowSize(int x, int y)
{
  postMessage(WM_LG_SET_SIZE, x, y);
}

static void win32SetFullscreen(bool fs)
{
  postMessage(WM_LG_SET_FULLSCREEN, fs, 0);
}

static bool win32GetFullscreen(void)
{
  return atomic_load(&win32.fullscreen);
}

static void win32Minimize(void)
{
  ShowWindowAsync(win32.window, SW_MINIMIZE);
}

struct LG_DisplayServerOps LGDS_Win32 =
{
  .name                = "Win32",
  .setup               = win32Setup,
  .probe               = win32Probe,
  .earlyInit           = win32EarlyInit,
  .init                = win32Init,
  .startup             = win32Startup,
  .shutdown            = win32Shutdown,
  .free                = win32Free,
  .getProp             = win32GetProp,
#ifdef ENABLE_OPENGL
  .glCreateContext     = win32GLCreateContext,
  .glDeleteContext     = win32GLDeleteContext,
  .glMakeCurrent       = win32GLMakeCurrent,
  .glSetSwapInterval   = win32GLSetSwapInterval,
  .glSwapBuffers       = win32GLSwapBuffers,
#endif
  .waitFrame           = win32WaitFrame,
  .skipFrame           = win32SkipFrame,
  .stopWaitFrame       = win32StopWaitFrame,
  .getFramePeriod      = win32GetFramePeriod,
  .guestPointerUpdated = win32GuestPointerUpdated,
  .setPointer          = win32SetPointer,
  .grabPointer         = win32GrabPointer,
  .ungrabPointer       = win32UngrabPointer,
  .isPointerGrabbed    = win32IsPointerGrabbed,
  .capturePointer      = win32CapturePointer,
  .uncapturePointer    = win32UncapturePointer,
  .isPointerCaptured   = win32IsPointerCaptured,
  .getKeyLabel         = win32GetKeyLabel,
  .grabKeyboard        = win32GrabKeyboard,
  .ungrabKeyboard      = win32UngrabKeyboard,
  .warpPointer         = win32WarpPointer,
  .realignPointer      = win32RealignPointer,
  .isValidPointerPos   = win32IsValidPointerPos,
  .requestActivation   = win32RequestActivation,
  .inhibitIdle         = win32InhibitIdle,
  .uninhibitIdle       = win32UninhibitIdle,
  .wait                = win32Wait,
  .setWindowSize       = win32SetWindowSize,
  .setFullscreen       = win32SetFullscreen,
  .getFullscreen       = win32GetFullscreen,
  .minimize            = win32Minimize,
};
