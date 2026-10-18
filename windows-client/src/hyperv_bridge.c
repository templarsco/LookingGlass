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

/*
 * Looking Glass Hyper-V / TCP user-mode bridge (lg-hyperv-bridge).
 *
 * Relays a Looking Glass LGMP session between a Windows guest's user-mode
 * named section (populated by looking-glass-host.exe with os:shmFile) and a
 * Windows host's user-mode named section (consumed by looking-glass-client.exe
 * with -f Local\looking-glass) over Hyper-V Sockets (AF_HYPERV) or TCP, with
 * zero kernel drivers required in the guest:
 *
 *   - LGMP_Q_FRAME: streams full frames or damage-rectangle deltas from the
 *     guest section into the host section's ring of KVMFRFrameBuffer slots.
 *   - LGMP_Q_POINTER: streams cursor position and shape updates from the
 *     guest section into the host section, and relays KVMFRSetCursorPos back
 *     to the guest.
 *   - LGMP_Q_INPUT: publishes KVMFR_FEATURE_INPUT and the LGMP input stream
 *     endpoints on the host section so looking-glass-client.exe sends native
 *     KVMFRInputMessage packets (relative/absolute mouse + USB HID keyboard),
 *     which the guest bridge injects via Win32 SendInput.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sddl.h>

#include "session.h"

#include "common/debug.h"
#include "common/framebuffer.h"
#include "common/ivshmem.h"
#include "common/option.h"
#include "common/time.h"
#include "common/windebug.h"

#include <LGProtocol/KVMFR.h>
#include <LGProtocol/KVMFRInput.h>
#include <LGProtocol/LGMPConfig.h>
#include <lgmp/client.h>
#include <lgmp/host.h>
#include <lgmp/stream.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AF_HYPERV
#define AF_HYPERV       34
#endif
#ifndef HV_PROTOCOL_RAW
#define HV_PROTOCOL_RAW 1
#endif

typedef struct _SOCKADDR_HV
{
  ADDRESS_FAMILY Family;
  USHORT         Reserved;
  GUID           VmId;
  GUID           ServiceId;
}
SOCKADDR_HV;

/* {00000000-0000-0000-0000-000000000000} - wildcard VM ID on host */
static const GUID HV_GUID_WILDCARD =
  { 0x00000000, 0x0000, 0x0000, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };

/* {a42e7cda-d03f-480c-9cc2-a4de20abb878} - parent partition from guest */
static const GUID HV_GUID_PARENT =
  { 0xa42e7cda, 0xd03f, 0x480c, { 0x9c, 0xc2, 0xa4, 0xde, 0x20, 0xab, 0xb8, 0x78 } };

/* Default Looking Glass Hyper-V Socket Service GUID:
 * {e0b8c9d0-1a2b-4c3d-8e4f-5a6b7c8d9e0f} */
static const GUID LG_DEFAULT_SERVICE_GUID =
  { 0xe0b8c9d0, 0x1a2b, 0x4c3d, { 0x8e, 0x4f, 0x5a, 0x6b, 0x7c, 0x8d, 0x9e, 0x0f } };

#define BRIDGE_MAGIC           0x4247474Cu /* 'LGGB' */
#define BRIDGE_VERSION         1u
#define BRIDGE_DEFAULT_SECTION "Local\\looking-glass"
#define BRIDGE_DEFAULT_SIZE_MB 128u
#define BRIDGE_DEFAULT_PORT    32589u
#define MAX_POINTER_SHAPE_SIZE (512u * 512u * 4u)
#define POINTER_SLOT_SIZE      (sizeof(KVMFRCursor) + (128u * 128u * 4u))

enum BridgePacketType
{
  BRIDGE_PKT_HELLO       = 1,
  BRIDGE_PKT_FRAME_FULL  = 2,
  BRIDGE_PKT_FRAME_RECTS = 3,
  BRIDGE_PKT_POINTER     = 4,
  BRIDGE_PKT_CURSOR_POS  = 5,
  BRIDGE_PKT_INPUT_MSG   = 6,
  BRIDGE_PKT_PING        = 7,
};

#pragma pack(push, 1)
struct BridgePacketHeader
{
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint32_t flags;
  uint32_t payloadSize;
};

struct BridgeFramePacket
{
  KVMFRFrame frame;
  uint32_t   frameDataSize;
};

struct BridgeRectEntry
{
  KVMFRFrameDamageRect rect;
  uint32_t             rowBytes;
};

struct BridgePointerPacket
{
  KVMFRCursor cursor;
  uint32_t    shapeSize;
};
#pragma pack(pop)

static const struct LGMPQueueConfig frameQueueConfig =
{
  .queueID     = LGMP_Q_FRAME,
  .numMessages = LGMP_Q_FRAME_LEN,
  .subTimeout  = 1000
};

static const struct LGMPQueueConfig pointerQueueConfig =
{
  .queueID     = LGMP_Q_POINTER,
  .numMessages = LGMP_Q_POINTER_LEN,
  .subTimeout  = 1000
};

static const struct LGMPQueueConfig inputQueueConfig =
{
  .queueID     = LGMP_Q_INPUT,
  .numMessages = LGMP_Q_INPUT_LEN,
  .subTimeout  = 1000
};

struct BridgeOptions
{
  bool         isHost;
  bool         isGuest;
  bool         tcpListen;
  bool         tcpConnect;
  const char * sectionName;
  unsigned     sectionSizeMB;
  const char * tcpEndpoint;
  GUID         vmId;
  GUID         serviceId;
  bool         useHyperV;
  bool         noInject;
  const char * inputLogPath;
  unsigned     maxFrames;
  unsigned     timeoutSec;
};

static volatile LONG stopRequested = 0;

static BOOL WINAPI ctrlHandler(DWORD type)
{
  (void)type;
  InterlockedExchange(&stopRequested, 1);
  return TRUE;
}

/* USB HID usage (0..255) to PS/2 Set 1 make code (0x00XX or 0xE0XX), derived
 * directly from client/src/kb_hid.c and client/src/kb.c. */
static const uint16_t hidToPs2Table[256] =
{
  [  4] = 0x001E, /* KEY_A */
  [  5] = 0x0030, /* KEY_B */
  [  6] = 0x002E, /* KEY_C */
  [  7] = 0x0020, /* KEY_D */
  [  8] = 0x0012, /* KEY_E */
  [  9] = 0x0021, /* KEY_F */
  [ 10] = 0x0022, /* KEY_G */
  [ 11] = 0x0023, /* KEY_H */
  [ 12] = 0x0017, /* KEY_I */
  [ 13] = 0x0024, /* KEY_J */
  [ 14] = 0x0025, /* KEY_K */
  [ 15] = 0x0026, /* KEY_L */
  [ 16] = 0x0032, /* KEY_M */
  [ 17] = 0x0031, /* KEY_N */
  [ 18] = 0x0018, /* KEY_O */
  [ 19] = 0x0019, /* KEY_P */
  [ 20] = 0x0010, /* KEY_Q */
  [ 21] = 0x0013, /* KEY_R */
  [ 22] = 0x001F, /* KEY_S */
  [ 23] = 0x0014, /* KEY_T */
  [ 24] = 0x0016, /* KEY_U */
  [ 25] = 0x002F, /* KEY_V */
  [ 26] = 0x0011, /* KEY_W */
  [ 27] = 0x002D, /* KEY_X */
  [ 28] = 0x0015, /* KEY_Y */
  [ 29] = 0x002C, /* KEY_Z */
  [ 30] = 0x0002, /* KEY_1 */
  [ 31] = 0x0003, /* KEY_2 */
  [ 32] = 0x0004, /* KEY_3 */
  [ 33] = 0x0005, /* KEY_4 */
  [ 34] = 0x0006, /* KEY_5 */
  [ 35] = 0x0007, /* KEY_6 */
  [ 36] = 0x0008, /* KEY_7 */
  [ 37] = 0x0009, /* KEY_8 */
  [ 38] = 0x000A, /* KEY_9 */
  [ 39] = 0x000B, /* KEY_0 */
  [ 40] = 0x001C, /* KEY_ENTER */
  [ 41] = 0x0001, /* KEY_ESC */
  [ 42] = 0x000E, /* KEY_BACKSPACE */
  [ 43] = 0x000F, /* KEY_TAB */
  [ 44] = 0x0039, /* KEY_SPACE */
  [ 45] = 0x000C, /* KEY_MINUS */
  [ 46] = 0x000D, /* KEY_EQUAL */
  [ 47] = 0x001A, /* KEY_LEFTBRACE */
  [ 48] = 0x001B, /* KEY_RIGHTBRACE */
  [ 49] = 0x002B, /* KEY_BACKSLASH */
  [ 51] = 0x0027, /* KEY_SEMICOLON */
  [ 52] = 0x0028, /* KEY_APOSTROPHE */
  [ 53] = 0x0029, /* KEY_GRAVE */
  [ 54] = 0x0033, /* KEY_COMMA */
  [ 55] = 0x0034, /* KEY_DOT */
  [ 56] = 0x0035, /* KEY_SLASH */
  [ 57] = 0x003A, /* KEY_CAPSLOCK */
  [ 58] = 0x003B, /* KEY_F1 */
  [ 59] = 0x003C, /* KEY_F2 */
  [ 60] = 0x003D, /* KEY_F3 */
  [ 61] = 0x003E, /* KEY_F4 */
  [ 62] = 0x003F, /* KEY_F5 */
  [ 63] = 0x0040, /* KEY_F6 */
  [ 64] = 0x0041, /* KEY_F7 */
  [ 65] = 0x0042, /* KEY_F8 */
  [ 66] = 0x0043, /* KEY_F9 */
  [ 67] = 0x0044, /* KEY_F10 */
  [ 68] = 0x0057, /* KEY_F11 */
  [ 69] = 0x0058, /* KEY_F12 */
  [ 70] = 0xE037, /* KEY_PRINT / KEY_SYSRQ */
  [ 71] = 0x0046, /* KEY_SCROLLLOCK */
  [ 72] = 0xE046, /* KEY_PAUSE */
  [ 73] = 0xE052, /* KEY_INSERT */
  [ 74] = 0xE047, /* KEY_HOME */
  [ 75] = 0xE049, /* KEY_PAGEUP */
  [ 76] = 0xE053, /* KEY_DELETE */
  [ 77] = 0xE04F, /* KEY_END */
  [ 78] = 0xE051, /* KEY_PAGEDOWN */
  [ 79] = 0xE04D, /* KEY_RIGHT */
  [ 80] = 0xE04B, /* KEY_LEFT */
  [ 81] = 0xE050, /* KEY_DOWN */
  [ 82] = 0xE048, /* KEY_UP */
  [ 83] = 0x0045, /* KEY_NUMLOCK */
  [ 84] = 0xE035, /* KEY_KPSLASH */
  [ 85] = 0x0037, /* KEY_KPASTERISK */
  [ 86] = 0x004A, /* KEY_KPMINUS */
  [ 87] = 0x004E, /* KEY_KPPLUS */
  [ 88] = 0xE01C, /* KEY_KPENTER */
  [ 89] = 0x004F, /* KEY_KP1 */
  [ 90] = 0x0050, /* KEY_KP2 */
  [ 91] = 0x0051, /* KEY_KP3 */
  [ 92] = 0x004B, /* KEY_KP4 */
  [ 93] = 0x004C, /* KEY_KP5 */
  [ 94] = 0x004D, /* KEY_KP6 */
  [ 95] = 0x0047, /* KEY_KP7 */
  [ 96] = 0x0048, /* KEY_KP8 */
  [ 97] = 0x0049, /* KEY_KP9 */
  [ 98] = 0x0052, /* KEY_KP0 */
  [ 99] = 0x0053, /* KEY_KPDOT */
  [100] = 0x0056, /* KEY_102ND */
  [101] = 0xE05D, /* KEY_COMPOSE */
  [102] = 0xE05E, /* KEY_POWER */
  [103] = 0x0059, /* KEY_KPEQUAL */
  [104] = 0x005D, /* KEY_F13 */
  [105] = 0x005E, /* KEY_F14 */
  [106] = 0x005F, /* KEY_F15 */
  [107] = 0x0055, /* KEY_F16 */
  [108] = 0xE003, /* KEY_F17 */
  [109] = 0xE077, /* KEY_F18 */
  [110] = 0xE004, /* KEY_F19 */
  [111] = 0x005A, /* KEY_F20 */
  [112] = 0x0074, /* KEY_F21 */
  [113] = 0xE079, /* KEY_F22 */
  [114] = 0x006D, /* KEY_F23 */
  [115] = 0x006F, /* KEY_F24 */
  [127] = 0xE020, /* KEY_MUTE */
  [128] = 0xE030, /* KEY_VOLUMEUP */
  [129] = 0xE02E, /* KEY_VOLUMEDOWN */
  [133] = 0x007E, /* KEY_KPCOMMA */
  [135] = 0x0073, /* KEY_RO */
  [136] = 0x0070, /* KEY_KATAKANAHIRAGANA */
  [137] = 0x007D, /* KEY_YEN */
  [138] = 0x0079, /* KEY_HENKAN */
  [139] = 0x007B, /* KEY_MUHENKAN */
  [224] = 0x001D, /* KEY_LEFTCTRL */
  [225] = 0x002A, /* KEY_LEFTSHIFT */
  [226] = 0x0038, /* KEY_LEFTALT */
  [227] = 0xE05B, /* KEY_LEFTMETA */
  [228] = 0xE01D, /* KEY_RIGHTCTRL */
  [229] = 0x0036, /* KEY_RIGHTSHIFT */
  [230] = 0xE038, /* KEY_RIGHTALT */
  [231] = 0xE05C, /* KEY_RIGHTMETA */
};

static void usage(const char * prog)
{
  fprintf(stderr,
    "Usage: %s (--host | --guest) [options]\n"
    "\n"
    "Relays a Looking Glass LGMP session between a Windows guest's named\n"
    "shared memory section and a Windows host's named shared memory section\n"
    "over Hyper-V Sockets (AF_HYPERV) or TCP.\n"
    "\n"
    "  --host               run on the Hyper-V host, publishing the host\n"
    "                       named section for looking-glass-client.exe\n"
    "  --guest              run inside the Windows guest, reading from\n"
    "                       looking-glass-host.exe's named section\n"
    "  --section=NAME       named section path (default: %s)\n"
    "  --size=MB            host section size in MiB (default: %u)\n"
    "  --tcp=[ADDR:]PORT    use TCP instead of AF_HYPERV (default port: %u)\n"
    "  --vm-id=GUID         restrict AF_HYPERV host listener to VM GUID\n"
    "  --service-id=GUID    override AF_HYPERV service GUID\n"
    "  --no-inject          do not call Win32 SendInput / SetCursorPos\n"
    "  --input-log=PATH     append received input events to PATH\n"
    "  --frames=N           stop after relaying N frames\n"
    "  --timeout=S          stop after S seconds\n",
    prog, BRIDGE_DEFAULT_SECTION, BRIDGE_DEFAULT_SIZE_MB, BRIDGE_DEFAULT_PORT);
}

static bool parseUnsigned(const char * s, unsigned * out)
{
  char * end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (!*s || (end && *end) || v > UINT32_MAX)
    return false;
  *out = (unsigned)v;
  return true;
}

static bool parseGuid(const char * s, GUID * out)
{
  unsigned long  d1;
  unsigned int   d2, d3;
  unsigned int   d4[8];
  char           extra;

  if (sscanf(s,
      "%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x%c",
      &d1, &d2, &d3,
      &d4[0], &d4[1], &d4[2], &d4[3],
      &d4[4], &d4[5], &d4[6], &d4[7], &extra) != 11)
    return false;

  out->Data1 = (uint32_t)d1;
  out->Data2 = (uint16_t)d2;
  out->Data3 = (uint16_t)d3;
  for (int i = 0; i < 8; ++i)
    out->Data4[i] = (uint8_t)d4[i];
  return true;
}

static bool parseArgs(int argc, char * argv[], struct BridgeOptions * opts)
{
  *opts = (struct BridgeOptions)
  {
    .sectionName   = BRIDGE_DEFAULT_SECTION,
    .sectionSizeMB = BRIDGE_DEFAULT_SIZE_MB,
    .vmId          = HV_GUID_WILDCARD,
    .serviceId     = LG_DEFAULT_SERVICE_GUID,
    .useHyperV     = true,
  };

  for (int i = 1; i < argc; ++i)
  {
    const char * arg = argv[i];
    if (strcmp(arg, "--host") == 0)
      opts->isHost = true;
    else if (strcmp(arg, "--guest") == 0)
      opts->isGuest = true;
    else if (strcmp(arg, "--listen") == 0)
      opts->tcpListen = true;
    else if (strcmp(arg, "--connect") == 0)
      opts->tcpConnect = true;
    else if (strncmp(arg, "--section=", 10) == 0 && arg[10])
      opts->sectionName = arg + 10;
    else if (strncmp(arg, "--size=", 7) == 0)
    {
      if (!parseUnsigned(arg + 7, &opts->sectionSizeMB) ||
          opts->sectionSizeMB < 4 || opts->sectionSizeMB > 2048)
        return false;
    }
    else if (strncmp(arg, "--tcp=", 6) == 0 && arg[6])
    {
      opts->tcpEndpoint = arg + 6;
      opts->useHyperV   = false;
    }
    else if (strcmp(arg, "--tcp") == 0)
    {
      opts->tcpEndpoint = "127.0.0.1:32589";
      opts->useHyperV   = false;
    }
    else if (strncmp(arg, "--vm-id=", 8) == 0)
    {
      if (!parseGuid(arg + 8, &opts->vmId))
        return false;
    }
    else if (strncmp(arg, "--service-id=", 13) == 0)
    {
      if (!parseGuid(arg + 13, &opts->serviceId))
        return false;
    }
    else if (strcmp(arg, "--no-inject") == 0)
      opts->noInject = true;
    else if (strncmp(arg, "--input-log=", 12) == 0 && arg[12])
      opts->inputLogPath = arg + 12;
    else if (strncmp(arg, "--frames=", 9) == 0)
    {
      if (!parseUnsigned(arg + 9, &opts->maxFrames))
        return false;
    }
    else if (strncmp(arg, "--timeout=", 10) == 0)
    {
      if (!parseUnsigned(arg + 10, &opts->timeoutSec))
        return false;
    }
    else
      return false;
  }

  return opts->isHost != opts->isGuest && !(opts->tcpListen && opts->tcpConnect);
}

static bool sendAll(SOCKET s, const void * buf, size_t len)
{
  const char * p = (const char *)buf;
  while (len > 0)
  {
    int chunk = len > 0x40000000u ? 0x40000000 : (int)len;
    int n = send(s, p, chunk, 0);
    if (n <= 0)
      return false;
    p   += n;
    len -= (size_t)n;
  }
  return true;
}

static bool recvAll(SOCKET s, void * buf, size_t len)
{
  char * p = (char *)buf;
  while (len > 0)
  {
    int chunk = len > 0x40000000u ? 0x40000000 : (int)len;
    int n = recv(s, p, chunk, 0);
    if (n <= 0)
      return false;
    p   += n;
    len -= (size_t)n;
  }
  return true;
}

static bool sendPacket(SOCKET s, uint16_t type, uint32_t flags,
    const void * payload, uint32_t payloadSize)
{
  struct BridgePacketHeader hdr =
  {
    .magic       = BRIDGE_MAGIC,
    .version     = BRIDGE_VERSION,
    .type        = type,
    .flags       = flags,
    .payloadSize = payloadSize
  };
  if (!sendAll(s, &hdr, sizeof(hdr)))
    return false;
  if (payloadSize > 0 && !sendAll(s, payload, payloadSize))
    return false;
  return true;
}

static bool socketHasData(SOCKET s, unsigned waitMs)
{
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(s, &rfds);
  struct timeval tv =
  {
    .tv_sec  = (long)(waitMs / 1000u),
    .tv_usec = (long)((waitMs % 1000u) * 1000u)
  };
  int rc = select(0, &rfds, NULL, NULL, &tv);
  return rc > 0;
}

static bool parseTcpEndpoint(const char * spec, char * hostBuf,
    size_t hostBufSize, uint16_t * portOut)
{
  const char * colon = strrchr(spec, ':');
  unsigned port = BRIDGE_DEFAULT_PORT;
  if (colon)
  {
    size_t hostLen = (size_t)(colon - spec);
    if (hostLen >= hostBufSize)
      return false;
    if (hostLen == 0)
      snprintf(hostBuf, hostBufSize, "0.0.0.0");
    else
    {
      memcpy(hostBuf, spec, hostLen);
      hostBuf[hostLen] = '\0';
    }
    if (!parseUnsigned(colon + 1, &port) || port == 0 || port > 65535)
      return false;
  }
  else
  {
    if (parseUnsigned(spec, &port) && port > 0 && port <= 65535)
      snprintf(hostBuf, hostBufSize, "127.0.0.1");
    else
    {
      snprintf(hostBuf, hostBufSize, "%s", spec);
      port = BRIDGE_DEFAULT_PORT;
    }
  }
  *portOut = (uint16_t)port;
  return true;
}

static SOCKET createHostListener(const struct BridgeOptions * opts)
{
  if (opts->useHyperV)
  {
    SOCKET s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET)
    {
      DEBUG_ERROR("socket(AF_HYPERV) failed: %d", WSAGetLastError());
      return INVALID_SOCKET;
    }

    SOCKADDR_HV addr = { 0 };
    addr.Family    = AF_HYPERV;
    addr.VmId      = opts->vmId;
    addr.ServiceId = opts->serviceId;

    if (bind(s, (const struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(s, 1) == SOCKET_ERROR)
    {
      DEBUG_ERROR("bind/listen(AF_HYPERV) failed: %d", WSAGetLastError());
      closesocket(s);
      return INVALID_SOCKET;
    }

    DEBUG_INFO("Listening on AF_HYPERV service {%08lx-%04x-%04x-...}",
        (unsigned long)opts->serviceId.Data1,
        (unsigned)opts->serviceId.Data2,
        (unsigned)opts->serviceId.Data3);
    return s;
  }

  char host[256];
  uint16_t port = BRIDGE_DEFAULT_PORT;
  if (!parseTcpEndpoint(opts->tcpEndpoint, host, sizeof(host), &port))
  {
    DEBUG_ERROR("Invalid --tcp endpoint: %s", opts->tcpEndpoint);
    return INVALID_SOCKET;
  }

  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET)
    return INVALID_SOCKET;

  BOOL reuse = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

  struct sockaddr_in sin = { 0 };
  sin.sin_family = AF_INET;
  sin.sin_port   = htons(port);
  sin.sin_addr.s_addr = inet_addr(host);
  if (sin.sin_addr.s_addr == INADDR_NONE)
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(s, (const struct sockaddr *)&sin, sizeof(sin)) == SOCKET_ERROR ||
      listen(s, 1) == SOCKET_ERROR)
  {
    DEBUG_ERROR("bind/listen(%s:%u) failed: %d", host, port, WSAGetLastError());
    closesocket(s);
    return INVALID_SOCKET;
  }

  DEBUG_INFO("Listening on TCP %s:%u", host, port);
  return s;
}

static SOCKET acceptClient(SOCKET listener, uint64_t deadlineNs)
{
  while (!InterlockedCompareExchange(&stopRequested, 0, 0))
  {
    if (deadlineNs && nanotime() >= deadlineNs)
      return INVALID_SOCKET;

    if (!socketHasData(listener, 50))
      continue;

    SOCKET client = accept(listener, NULL, NULL);
    if (client != INVALID_SOCKET)
    {
      BOOL nodelay = TRUE;
      setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
          (const char *)&nodelay, sizeof(nodelay));
      return client;
    }
  }
  return INVALID_SOCKET;
}

static SOCKET connectToHost(const struct BridgeOptions * opts, uint64_t deadlineNs)
{
  while (!InterlockedCompareExchange(&stopRequested, 0, 0))
  {
    if (deadlineNs && nanotime() >= deadlineNs)
      return INVALID_SOCKET;

    if (opts->useHyperV)
    {
      SOCKET s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
      if (s != INVALID_SOCKET)
      {
        SOCKADDR_HV addr = { 0 };
        addr.Family    = AF_HYPERV;
        addr.VmId      = HV_GUID_PARENT;
        addr.ServiceId = opts->serviceId;

        if (connect(s, (const struct sockaddr *)&addr, sizeof(addr)) == 0)
        {
          DEBUG_INFO("Connected to host over AF_HYPERV");
          return s;
        }
        closesocket(s);
      }
    }
    else
    {
      char host[256];
      uint16_t port = BRIDGE_DEFAULT_PORT;
      if (!parseTcpEndpoint(opts->tcpEndpoint, host, sizeof(host), &port))
        return INVALID_SOCKET;

      SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (s != INVALID_SOCKET)
      {
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
            (const char *)&nodelay, sizeof(nodelay));

        u_long nonBlock = 1;
        ioctlsocket(s, FIONBIO, &nonBlock);

        struct sockaddr_in sin = { 0 };
        sin.sin_family      = AF_INET;
        sin.sin_port        = htons(port);
        sin.sin_addr.s_addr = inet_addr(host);

        int rc = connect(s, (const struct sockaddr *)&sin, sizeof(sin));
        bool connected = (rc == 0);
        if (!connected && WSAGetLastError() == WSAEWOULDBLOCK)
        {
          fd_set wfds, efds;
          FD_ZERO(&wfds);
          FD_ZERO(&efds);
          FD_SET(s, &wfds);
          FD_SET(s, &efds);
          struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
          if (select(0, NULL, &wfds, &efds, &tv) > 0 &&
              FD_ISSET(s, &wfds) && !FD_ISSET(s, &efds))
          {
            int soErr = 0;
            int soLen = sizeof(soErr);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soErr, &soLen) == 0 &&
                soErr == 0)
              connected = true;
          }
        }

        if (connected)
        {
          nonBlock = 0;
          ioctlsocket(s, FIONBIO, &nonBlock);
          DEBUG_INFO("Connected to host over TCP %s:%u", host, port);
          return s;
        }
        closesocket(s);
      }
    }

    Sleep(50);
  }
  return INVALID_SOCKET;
}

struct InputInjector
{
  uint8_t  prevButtons;
  bool     prevHid[256];
  uint32_t screenWidth;
  uint32_t screenHeight;
  bool     noInject;
  FILE   * logFile;
  HDESK    desktop;
  uint64_t lastDeskSyncNs;
};

static void injectorSyncDesktop(struct InputInjector * inj)
{
  if (inj->noInject)
    return;

  uint64_t now = nanotime();
  if (inj->desktop && now - inj->lastDeskSyncNs < 250000000ULL)
    return;
  inj->lastDeskSyncNs = now;

  HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
  if (!desk)
    return;

  if (!SetThreadDesktop(desk))
  {
    CloseDesktop(desk);
    return;
  }

  if (inj->desktop)
    CloseDesktop(inj->desktop);
  inj->desktop = desk;
}

static void injectKey(uint8_t hidUsage, bool pressed)
{
  uint16_t ps2 = hidToPs2Table[hidUsage];
  if (!ps2)
    return;

  INPUT in = { 0 };
  in.type           = INPUT_KEYBOARD;
  in.ki.wScan       = (WORD)(ps2 & 0xFF);
  in.ki.dwFlags     = KEYEVENTF_SCANCODE |
                      ((ps2 & 0xFF00) == 0xE000 ? KEYEVENTF_EXTENDEDKEY : 0) |
                      (pressed ? 0 : KEYEVENTF_KEYUP);
  SendInput(1, &in, sizeof(in));
}

static void injectMouseButtons(uint8_t prevButtons, uint8_t newButtons)
{
  uint8_t diff = prevButtons ^ newButtons;
  if (!diff)
    return;

  INPUT inputs[5];
  UINT  count = 0;
  memset(inputs, 0, sizeof(inputs));

  if (diff & (1u << 0))
  {
    inputs[count].type       = INPUT_MOUSE;
    inputs[count].mi.dwFlags = (newButtons & (1u << 0)) ?
      MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    ++count;
  }
  if (diff & (1u << 1))
  {
    inputs[count].type       = INPUT_MOUSE;
    inputs[count].mi.dwFlags = (newButtons & (1u << 1)) ?
      MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    ++count;
  }
  if (diff & (1u << 2))
  {
    inputs[count].type       = INPUT_MOUSE;
    inputs[count].mi.dwFlags = (newButtons & (1u << 2)) ?
      MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    ++count;
  }
  if (diff & (1u << 3))
  {
    inputs[count].type         = INPUT_MOUSE;
    inputs[count].mi.dwFlags   = (newButtons & (1u << 3)) ?
      MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    inputs[count].mi.mouseData = XBUTTON1;
    ++count;
  }
  if (diff & (1u << 4))
  {
    inputs[count].type         = INPUT_MOUSE;
    inputs[count].mi.dwFlags   = (newButtons & (1u << 4)) ?
      MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    inputs[count].mi.mouseData = XBUTTON2;
    ++count;
  }

  if (count > 0)
    SendInput(count, inputs, sizeof(INPUT));
}

static void injectorReleaseAll(struct InputInjector * inj)
{
  if (!inj->noInject)
  {
    if (inj->prevButtons)
      injectMouseButtons(inj->prevButtons, 0);

    for (unsigned u = 0; u < 256; ++u)
      if (inj->prevHid[u])
        injectKey((uint8_t)u, false);
  }

  inj->prevButtons = 0;
  memset(inj->prevHid, 0, sizeof(inj->prevHid));
}

static void injectorHandleMessage(struct InputInjector * inj,
    const KVMFRInputMessage * msg)
{
  injectorSyncDesktop(inj);
  switch (msg->type)
  {
    case KVMFR_INPUT_MESSAGE_CLAIM:
      if (inj->logFile)
      {
        fprintf(inj->logFile, "CLAIM gen=%u seq=%u\n",
            msg->generation, msg->sequence);
        fflush(inj->logFile);
      }
      injectorReleaseAll(inj);
      break;

    case KVMFR_INPUT_MESSAGE_RELEASE:
    case KVMFR_INPUT_MESSAGE_RESET:
      if (inj->logFile)
      {
        fprintf(inj->logFile, "RELEASE gen=%u seq=%u\n",
            msg->generation, msg->sequence);
        fflush(inj->logFile);
      }
      injectorReleaseAll(inj);
      break;

    case KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE:
    {
      const KVMFRInputMouseRelative * mr = &msg->payload.mouseRelative;
      if (inj->logFile)
      {
        fprintf(inj->logFile, "MOUSE_REL dx=%d dy=%d wheel=%d buttons=0x%02x\n",
            (int)mr->deltaX, (int)mr->deltaY, (int)mr->wheel,
            (unsigned)mr->buttons);
        fflush(inj->logFile);
      }
      if (!inj->noInject)
      {
        if (mr->deltaX != 0 || mr->deltaY != 0)
        {
          INPUT in = { 0 };
          in.type       = INPUT_MOUSE;
          in.mi.dx      = mr->deltaX;
          in.mi.dy      = mr->deltaY;
          in.mi.dwFlags = MOUSEEVENTF_MOVE;
          SendInput(1, &in, sizeof(in));
        }
        injectMouseButtons(inj->prevButtons, (uint8_t)mr->buttons);
        if (mr->wheel != 0)
        {
          INPUT in = { 0 };
          in.type         = INPUT_MOUSE;
          in.mi.dwFlags   = MOUSEEVENTF_WHEEL;
          in.mi.mouseData = (DWORD)((int)mr->wheel * WHEEL_DELTA);
          SendInput(1, &in, sizeof(in));
        }
      }
      inj->prevButtons = (uint8_t)mr->buttons;
      break;
    }

    case KVMFR_INPUT_MESSAGE_MOUSE_ABSOLUTE:
    {
      const KVMFRInputMouseAbsolute * ma = &msg->payload.mouseAbsolute;
      if (inj->logFile)
      {
        fprintf(inj->logFile, "MOUSE_ABS x=%u y=%u wheel=%d buttons=0x%02x\n",
            (unsigned)ma->x, (unsigned)ma->y, (int)ma->wheel,
            (unsigned)ma->buttons);
        fflush(inj->logFile);
      }
      if (!inj->noInject)
      {
        uint32_t clampedX = ma->x <= KVMFR_INPUT_MOUSE_ABSOLUTE_MAX ?
          ma->x : KVMFR_INPUT_MOUSE_ABSOLUTE_MAX;
        uint32_t clampedY = ma->y <= KVMFR_INPUT_MOUSE_ABSOLUTE_MAX ?
          ma->y : KVMFR_INPUT_MOUSE_ABSOLUTE_MAX;

        INPUT in = { 0 };
        in.type       = INPUT_MOUSE;
        in.mi.dx      = (LONG)((clampedX * 65535ULL) /
                               KVMFR_INPUT_MOUSE_ABSOLUTE_MAX);
        in.mi.dy      = (LONG)((clampedY * 65535ULL) /
                               KVMFR_INPUT_MOUSE_ABSOLUTE_MAX);
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &in, sizeof(in));

        injectMouseButtons(inj->prevButtons, (uint8_t)ma->buttons);
        if (ma->wheel != 0)
        {
          INPUT win = { 0 };
          win.type         = INPUT_MOUSE;
          win.mi.dwFlags   = MOUSEEVENTF_WHEEL;
          win.mi.mouseData = (DWORD)((int)ma->wheel * WHEEL_DELTA);
          SendInput(1, &win, sizeof(win));
        }
      }
      inj->prevButtons = (uint8_t)ma->buttons;
      break;
    }

    case KVMFR_INPUT_MESSAGE_KEYBOARD:
    {
      const KVMFRInputKeyboard * kb = &msg->payload.keyboard;
      if (inj->logFile)
      {
        fprintf(inj->logFile,
            "KEYBOARD mod=0x%02x keys=%u,%u,%u,%u,%u,%u\n",
            (unsigned)kb->modifiers,
            (unsigned)kb->keys[0], (unsigned)kb->keys[1],
            (unsigned)kb->keys[2], (unsigned)kb->keys[3],
            (unsigned)kb->keys[4], (unsigned)kb->keys[5]);
        fflush(inj->logFile);
      }

      bool nextHid[256];
      memset(nextHid, 0, sizeof(nextHid));
      for (unsigned b = 0; b < 8; ++b)
        if (kb->modifiers & (1u << b))
          nextHid[224u + b] = true;
      for (unsigned k = 0; k < KVMFR_INPUT_KEYBOARD_KEY_COUNT; ++k)
        if (kb->keys[k] != 0)
          nextHid[kb->keys[k]] = true;

      if (!inj->noInject)
      {
        for (unsigned u = 0; u < 256; ++u)
          if (inj->prevHid[u] != nextHid[u])
            injectKey((uint8_t)u, nextHid[u]);
      }
      memcpy(inj->prevHid, nextHid, sizeof(inj->prevHid));
      break;
    }

    default:
      break;
  }
}

static uint32_t computeCursorShapeSize(const KVMFRCursor * cursor)
{
  switch (cursor->type)
  {
    case CURSOR_TYPE_COLOR:
    case CURSOR_TYPE_MASKED_COLOR:
    {
      uint32_t pitch = cursor->pitch ? cursor->pitch : cursor->width * 4u;
      uint64_t total = (uint64_t)pitch * cursor->height;
      return total <= MAX_POINTER_SHAPE_SIZE ? (uint32_t)total : 0;
    }
    case CURSOR_TYPE_MONOCHROME:
    {
      uint32_t pitch = cursor->pitch ? cursor->pitch : (cursor->width + 7u) / 8u;
      uint64_t total = (uint64_t)pitch * cursor->height;
      return total <= MAX_POINTER_SHAPE_SIZE ? (uint32_t)total : 0;
    }
    default:
      return 0;
  }
}

static bool drainHostToGuestSocket(SOCKET sock, PLGMPClientQueue pointerQueue,
    struct InputInjector * inj)
{
  while (socketHasData(sock, 0))
  {
    struct BridgePacketHeader hdr;
    if (!recvAll(sock, &hdr, sizeof(hdr)))
      return false;

    if (hdr.magic != BRIDGE_MAGIC || hdr.version != BRIDGE_VERSION)
    {
      DEBUG_ERROR("Invalid packet header from host");
      return false;
    }

    if (hdr.type == BRIDGE_PKT_CURSOR_POS &&
        hdr.payloadSize == sizeof(KVMFRSetCursorPos))
    {
      KVMFRSetCursorPos pos;
      if (!recvAll(sock, &pos, sizeof(pos)))
        return false;

      if (inj->logFile)
      {
        fprintf(inj->logFile, "CURSOR_POS x=%d y=%d\n",
            (int)pos.x, (int)pos.y);
        fflush(inj->logFile);
      }
      if (!inj->noInject)
      {
        injectorSyncDesktop(inj);
        SetCursorPos(pos.x, pos.y);
      }
      if (pointerQueue)
        lgmpClientSendData(pointerQueue, &pos, sizeof(pos), NULL);
    }
    else if (hdr.type == BRIDGE_PKT_INPUT_MSG &&
             hdr.payloadSize == sizeof(KVMFRInputMessage))
    {
      KVMFRInputMessage msg;
      if (!recvAll(sock, &msg, sizeof(msg)))
        return false;
      injectorHandleMessage(inj, &msg);
    }
    else if (hdr.payloadSize > 0)
    {
      uint8_t discard[256];
      uint32_t rem = hdr.payloadSize;
      while (rem > 0)
      {
        uint32_t chunk = rem > sizeof(discard) ? (uint32_t)sizeof(discard) : rem;
        if (!recvAll(sock, discard, chunk))
          return false;
        rem -= chunk;
      }
    }
  }
  return true;
}

static int runGuestBridge(const struct BridgeOptions * opts)
{
  const uint64_t deadlineNs = opts->timeoutSec ?
    nanotime() + (uint64_t)opts->timeoutSec * 1000000000ULL : 0;

  option_register((struct Option[])
  {
    {
      .module         = "os",
      .name           = "shmFile",
      .description    = "Shared memory section path",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = (char *)opts->sectionName
    },
    {
      .module      = "os",
      .name        = "shmSize",
      .description = "Shared memory size MiB",
      .type        = OPTION_TYPE_INT,
      .value.x_int = 0
    },
    { 0 }
  });

  SOCKET listener = opts->tcpListen ? createHostListener(opts) : INVALID_SOCKET;
  if (opts->tcpListen && listener == INVALID_SOCKET)
    return EXIT_FAILURE;

  struct IVSHMEM shm = { 0 };
  while (!InterlockedCompareExchange(&stopRequested, 0, 0))
  {
    if (deadlineNs && nanotime() >= deadlineNs)
    {
      if (listener != INVALID_SOCKET)
        closesocket(listener);
      return EXIT_FAILURE;
    }
    if (ivshmemOpenDev(&shm, opts->sectionName))
      break;
    Sleep(20);
  }
  if (!shm.mem)
  {
    if (listener != INVALID_SOCKET)
      closesocket(listener);
    return EXIT_FAILURE;
  }

  int      ret          = EXIT_FAILURE;
  SOCKET   sock         = INVALID_SOCKET;
  PLGMPClient client    = NULL;
  uint8_t * deltaBuf    = NULL;
  size_t    deltaCap    = 0;
  unsigned  relayedFrames = 0;
  struct InputInjector inj =
  {
    .noInject = opts->noInject,
    .logFile  = opts->inputLogPath ? fopen(opts->inputLogPath, "w") : NULL
  };

next_peer:
  if (sock == INVALID_SOCKET)
  {
    sock = opts->tcpListen ?
      acceptClient(listener, deadlineNs) :
      connectToHost(opts, deadlineNs);
    if (sock == INVALID_SOCKET)
      goto out;
  }

  if (client)
    lgmpClientFree(&client);

  if (lgmpClientInit(shm.mem, shm.size, &client) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpClientInit failed");
    goto out;
  }

  uint32_t  udataSize     = 0;
  uint32_t  clientID      = 0;
  uint32_t  remoteVersion = 0;
  uint8_t * udata         = NULL;
  while (!InterlockedCompareExchange(&stopRequested, 0, 0))
  {
    if (deadlineNs && nanotime() >= deadlineNs)
      goto out;

    LGMP_STATUS st = lgmpClientSessionInit(client, &udataSize, &udata,
        &clientID, &remoteVersion);
    if (st == LGMP_OK)
      break;
    Sleep(10);
  }
  if (!udata)
    goto out;

  if (!sendPacket(sock, BRIDGE_PKT_HELLO, 0, udata, udataSize))
    goto peer_disconnected;

  PLGMPClientQueue frameQueue   = NULL;
  PLGMPClientQueue pointerQueue = NULL;
  while (!InterlockedCompareExchange(&stopRequested, 0, 0))
  {
    if (deadlineNs && nanotime() >= deadlineNs)
      goto out;
    if (!frameQueue)
      lgmpClientSubscribe(client, LGMP_Q_FRAME, &frameQueue);
    if (!pointerQueue)
      lgmpClientSubscribe(client, LGMP_Q_POINTER, &pointerQueue);
    if (frameQueue && pointerQueue)
      break;
    Sleep(10);
  }
  if (!frameQueue || !pointerQueue)
    goto out;

  uint32_t prevWidth  = 0;
  uint32_t prevHeight = 0;
  uint32_t prevPitch  = 0;
  uint32_t prevType   = 0;

  while (!InterlockedCompareExchange(&stopRequested, 0, 0))
  {
    if (deadlineNs && nanotime() >= deadlineNs)
      break;

    if (!drainHostToGuestSocket(sock, pointerQueue, &inj))
      goto peer_disconnected;

    bool didWork = false;
    LGMPMessage msg;

    LGMP_STATUS pst = lgmpClientProcess(pointerQueue, &msg);
    if (pst == LGMP_ERR_INVALID_SESSION)
      goto next_peer;
    if (pst == LGMP_OK)
    {
      didWork = true;
      const KVMFRCursor * cursor = (const KVMFRCursor *)msg.mem;
      uint32_t shapeSize = (msg.udata & CURSOR_FLAG_SHAPE) ?
        computeCursorShapeSize(cursor) : 0;

      struct BridgePointerPacket pkt =
      {
        .cursor    = *cursor,
        .shapeSize = shapeSize
      };
      struct BridgePacketHeader hdr =
      {
        .magic       = BRIDGE_MAGIC,
        .version     = BRIDGE_VERSION,
        .type        = BRIDGE_PKT_POINTER,
        .flags       = msg.udata,
        .payloadSize = (uint32_t)(sizeof(pkt) + shapeSize)
      };

      const uint8_t * shapePtr = (const uint8_t *)(cursor + 1);
      bool ok = sendAll(sock, &hdr, sizeof(hdr)) &&
                sendAll(sock, &pkt, sizeof(pkt)) &&
                (shapeSize == 0 || sendAll(sock, shapePtr, shapeSize));
      lgmpClientMessageDone(pointerQueue);
      if (!ok)
        goto peer_disconnected;
    }

    LGMP_STATUS fst = lgmpClientProcess(frameQueue, &msg);
    if (fst == LGMP_ERR_INVALID_SESSION)
      goto next_peer;
    if (fst == LGMP_OK)
    {
      didWork = true;
      const KVMFRFrame * frame = (const KVMFRFrame *)msg.mem;
      const KVMFRFrameBuffer * fb =
        (const KVMFRFrameBuffer *)((const uint8_t *)frame + frame->offset);
      const uint32_t dataWidth  = frame->dataWidth  ? frame->dataWidth  : frame->frameWidth;
      const uint32_t dataHeight = frame->dataHeight ? frame->dataHeight : frame->frameHeight;
      const uint32_t fullSize   = dataHeight * frame->pitch;

      inj.screenWidth  = frame->screenWidth  ? frame->screenWidth  : frame->frameWidth;
      inj.screenHeight = frame->screenHeight ? frame->screenHeight : frame->frameHeight;

      if (!framebuffer_wait(fb, fullSize))
      {
        lgmpClientMessageDone(frameQueue);
        continue;
      }
      const uint8_t * pixels = framebuffer_get_buffer(fb);

      uint32_t bpp = frame->stride ? (frame->pitch / frame->stride) : 4u;
      if (bpp == 0 || bpp > 16)
        bpp = 4u;

      bool sameGeometry =
        frame->frameWidth  == prevWidth  &&
        frame->frameHeight == prevHeight &&
        frame->pitch       == prevPitch  &&
        frame->type        == prevType;

      bool sendRects = false;
      size_t rectsPayloadSize = sizeof(struct BridgeFramePacket);
      if (sameGeometry &&
          frame->damageRectsCount > 0 &&
          frame->damageRectsCount <= KVMFR_MAX_DAMAGE_RECTS)
      {
        sendRects = true;
        for (uint32_t r = 0; r < frame->damageRectsCount; ++r)
        {
          KVMFRFrameDamageRect rc = frame->damageRects[r];
          if (frame->type == FRAME_TYPE_BGR_32)
          {
            uint32_t origX   = rc.x;
            uint32_t scaledX = (origX * 3u) / 4u;
            rc.x     = scaledX;
            rc.width = (((origX + rc.width) * 3u + 3u) / 4u) - scaledX;
          }
          if (rc.x + rc.width > dataWidth ||
              rc.y + rc.height > dataHeight ||
              rc.width == 0 || rc.height == 0)
          {
            sendRects = false;
            break;
          }
          uint32_t rowBytes = rc.width * bpp;
          rectsPayloadSize += sizeof(struct BridgeRectEntry) +
                              (size_t)rowBytes * rc.height;
        }
        if (rectsPayloadSize >= sizeof(struct BridgeFramePacket) + (fullSize * 3u) / 4u)
          sendRects = false;
      }

      bool ok = false;
      if (sendRects)
      {
        if (rectsPayloadSize > deltaCap)
        {
          uint8_t * nb = (uint8_t *)realloc(deltaBuf, rectsPayloadSize);
          if (!nb)
          {
            lgmpClientMessageDone(frameQueue);
            break;
          }
          deltaBuf = nb;
          deltaCap = rectsPayloadSize;
        }

        struct BridgeFramePacket * fp = (struct BridgeFramePacket *)deltaBuf;
        fp->frame         = *frame;
        fp->frameDataSize = fullSize;

        uint8_t * dst = deltaBuf + sizeof(*fp);
        for (uint32_t r = 0; r < frame->damageRectsCount; ++r)
        {
          KVMFRFrameDamageRect rc = frame->damageRects[r];
          if (frame->type == FRAME_TYPE_BGR_32)
          {
            uint32_t origX   = rc.x;
            uint32_t scaledX = (origX * 3u) / 4u;
            rc.x     = scaledX;
            rc.width = (((origX + rc.width) * 3u + 3u) / 4u) - scaledX;
          }
          uint32_t rowBytes = rc.width * bpp;
          struct BridgeRectEntry entry =
          {
            .rect     = rc,
            .rowBytes = rowBytes
          };
          memcpy(dst, &entry, sizeof(entry));
          dst += sizeof(entry);

          for (uint32_t y = 0; y < rc.height; ++y)
          {
            const uint8_t * srcRow =
              pixels + (size_t)(rc.y + y) * frame->pitch + (size_t)rc.x * bpp;
            memcpy(dst, srcRow, rowBytes);
            dst += rowBytes;
          }
        }

        ok = sendPacket(sock, BRIDGE_PKT_FRAME_RECTS, msg.udata,
            deltaBuf, (uint32_t)rectsPayloadSize);
      }
      else
      {
        struct BridgeFramePacket fp =
        {
          .frame         = *frame,
          .frameDataSize = fullSize
        };
        struct BridgePacketHeader hdr =
        {
          .magic       = BRIDGE_MAGIC,
          .version     = BRIDGE_VERSION,
          .type        = BRIDGE_PKT_FRAME_FULL,
          .flags       = msg.udata,
          .payloadSize = (uint32_t)(sizeof(fp) + fullSize)
        };
        ok = sendAll(sock, &hdr, sizeof(hdr)) &&
             sendAll(sock, &fp, sizeof(fp)) &&
             sendAll(sock, pixels, fullSize);
      }

      prevWidth  = frame->frameWidth;
      prevHeight = frame->frameHeight;
      prevPitch  = frame->pitch;
      prevType   = frame->type;

      lgmpClientMessageDone(frameQueue);
      if (!ok)
        goto peer_disconnected;

      ++relayedFrames;
      if (opts->maxFrames && relayedFrames >= opts->maxFrames)
      {
        /* Give any pending input / cursor returns a brief window to arrive */
        for (unsigned i = 0; i < 20; ++i)
        {
          if (!drainHostToGuestSocket(sock, pointerQueue, &inj))
            break;
          Sleep(5);
        }
        ret = EXIT_SUCCESS;
        goto out;
      }
    }

    if (!didWork)
      Sleep(1);
  }

peer_disconnected:
  injectorReleaseAll(&inj);
  if (sock != INVALID_SOCKET)
  {
    closesocket(sock);
    sock = INVALID_SOCKET;
  }
  if (!opts->maxFrames &&
      !InterlockedCompareExchange(&stopRequested, 0, 0) &&
      (!deadlineNs || nanotime() < deadlineNs))
    goto next_peer;

  if (!opts->maxFrames && relayedFrames > 0)
    ret = EXIT_SUCCESS;

out:
  injectorReleaseAll(&inj);
  if (inj.logFile)
    fclose(inj.logFile);
  free(deltaBuf);
  if (client)
    lgmpClientFree(&client);
  if (sock != INVALID_SOCKET)
    closesocket(sock);
  if (listener != INVALID_SOCKET)
    closesocket(listener);
  ivshmemClose(&shm);
  return ret;
}

struct HostSession
{
  PLGMPHost             host;
  PLGMPHostQueue        frameQueue;
  PLGMPHostQueue        pointerQueue;
  PLGMPHostQueue        inputQueue;
  PLGMPHostStream       inputStreams[KVMFR_INPUT_STREAM_ENDPOINT_COUNT];
  KVMFRStreamDescriptor streamDescriptors[KVMFR_INPUT_STREAM_ENDPOINT_COUNT];
  PLGMPMemory           frameMem[LGMP_Q_FRAME_LEN];
  uint32_t              frameMemSize;
  unsigned              frameIndex;
  PLGMPMemory           pointerMem[LGMP_Q_POINTER_LEN];
  unsigned              pointerIndex;
  PLGMPMemory           inputStatusMem[LGMP_Q_INPUT_LEN];
  unsigned              inputStatusIndex;
  uint32_t              statusSerial;
  uint32_t              endpointGeneration;
  uint32_t              streamGeneration;
  uint32_t              streamEpoch;
  uint32_t              boundClientID;
  bool                  hasBoundClient;
  bool                  hasOwner;
  uint32_t              ownerClientID;
  uint32_t              ownerGeneration;
  uint8_t             * shadowFrame;
  size_t                shadowCap;
  KVMFRFrame            lastFrame;
  uint32_t              lastFrameFlags;
  uint32_t              lastFrameDataSize;
  bool                  lastFrameValid;
  PLGMPMemory           lastPointerSlot;
  uint32_t              lastPointerFlags;
  bool                  lastPointerValid;
};

static KVMFRStreamDescriptor exportStreamDescriptor(
    const struct LGMPStreamDescriptor * descriptor)
{
  return (KVMFRStreamDescriptor)
  {
    .magic      = descriptor->magic,
    .version    = descriptor->version,
    .size       = descriptor->size,
    .offset     = descriptor->offset,
    .regionSize = descriptor->regionSize,
    .direction  = descriptor->direction,
    .policy     = descriptor->policy,
    .slotCount  = descriptor->slotCount,
    .slotSize   = descriptor->slotSize,
  };
}

static void hostSessionPublishInputStatus(struct HostSession * hs)
{
  if (!hs->inputQueue || !hs->inputStatusMem[0])
    return;

  PLGMPMemory slot = hs->inputStatusMem[hs->inputStatusIndex];
  hs->inputStatusIndex = (hs->inputStatusIndex + 1u) % LGMP_Q_INPUT_LEN;

  KVMFRInputStatus * st = (KVMFRInputStatus *)lgmpHostMemPtr(slot);
  *st = (KVMFRInputStatus)
  {
    .version             = KVMFR_INPUT_VERSION,
    .capabilities        = KVMFR_INPUT_CAP_MOUSE_RELATIVE |
                           KVMFR_INPUT_CAP_MOUSE_ABSOLUTE |
                           KVMFR_INPUT_CAP_KEYBOARD,
    .flags               = KVMFR_INPUT_STATUS_AVAILABLE |
                           (hs->hasOwner ? KVMFR_INPUT_STATUS_HAS_OWNER : 0),
    .generation          = hs->endpointGeneration,
    .ownerClientID       = hs->hasOwner ? hs->ownerClientID   : 0,
    .ownerGeneration     = hs->hasOwner ? hs->ownerGeneration : 0,
    .lease               = 1000,
    .maxButtons          = KVMFR_INPUT_MOUSE_BUTTON_COUNT,
    .streamVersion       = KVMFR_INPUT_STREAM_VERSION,
    .streamEndpointCount = KVMFR_INPUT_STREAM_ENDPOINT_COUNT,
    .streamGeneration    = hs->streamGeneration,
  };

  for (unsigned i = 0; i < KVMFR_INPUT_STREAM_ENDPOINT_COUNT; ++i)
  {
    st->streamEndpoint[i].stream = hs->streamDescriptors[i];
    st->streamEndpoint[i].flags  = KVMFR_INPUT_STREAM_ENDPOINT_AVAILABLE;
  }

  if (hs->hasBoundClient)
  {
    st->streamEndpoint[0].boundClientID     = hs->boundClientID;
    st->streamEndpoint[0].bindingGeneration = hs->streamEpoch;
    st->streamEndpoint[0].flags            |= KVMFR_INPUT_STREAM_ENDPOINT_BOUND;
  }

  if (++hs->statusSerial == 0)
    ++hs->statusSerial;
  lgmpHostQueuePost(hs->inputQueue, hs->statusSerial, slot);
}

static void hostSessionFree(struct HostSession * hs)
{
  if (hs->shadowFrame)
    _aligned_free(hs->shadowFrame);
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (hs->frameMem[i])
      lgmpHostMemFree(&hs->frameMem[i]);
  for (unsigned i = 0; i < LGMP_Q_POINTER_LEN; ++i)
    if (hs->pointerMem[i])
      lgmpHostMemFree(&hs->pointerMem[i]);
  for (unsigned i = 0; i < LGMP_Q_INPUT_LEN; ++i)
    if (hs->inputStatusMem[i])
      lgmpHostMemFree(&hs->inputStatusMem[i]);
  for (unsigned i = 0; i < KVMFR_INPUT_STREAM_ENDPOINT_COUNT; ++i)
    if (hs->inputStreams[i])
      lgmpHostStreamFree(&hs->inputStreams[i]);
  if (hs->host)
    lgmpHostFree(&hs->host);
  *hs = (struct HostSession) { 0 };
}

static bool hostSessionInit(struct HostSession * hs, struct IVSHMEM * shm,
    const uint8_t * guestUdata, uint32_t guestUdataSize)
{
  struct SessionData sessionInfo = { 0 };
  if (guestUdata && guestUdataSize >= sizeof(KVMFR) &&
      guestUdataSize <= sizeof(sessionInfo.data))
  {
    memcpy(sessionInfo.data, guestUdata, guestUdataSize);
    sessionInfo.size = guestUdataSize;
  }
  else if (!sessionBuild(&sessionInfo, "lg-hyperv-bridge", "D12",
        "Limiar-OpenHCL-ASUS-01"))
    return false;

  KVMFR * hdr = (KVMFR *)sessionInfo.data;
  hdr->features |= KVMFR_FEATURE_SETCURSORPOS | KVMFR_FEATURE_INPUT;

  if (lgmpHostInit(shm->mem, shm->size, &hs->host,
        sessionInfo.size, sessionInfo.data) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostInit failed");
    return false;
  }

  if (lgmpHostQueueNew(hs->host, frameQueueConfig, &hs->frameQueue) != LGMP_OK ||
      lgmpHostQueueNew(hs->host, pointerQueueConfig, &hs->pointerQueue) != LGMP_OK ||
      lgmpHostQueueNew(hs->host, inputQueueConfig, &hs->inputQueue) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueNew failed");
    return false;
  }

  const struct LGMPStreamConfig streamConfig =
  {
    .direction = LGMP_STREAM_CLIENT_TO_HOST,
    .policy    = LGMP_STREAM_RELIABLE_FIFO,
    .slotCount = KVMFR_INPUT_STREAM_SLOT_COUNT,
    .slotSize  = KVMFR_INPUT_STREAM_SLOT_SIZE,
  };

  for (unsigned i = 0; i < KVMFR_INPUT_STREAM_ENDPOINT_COUNT; ++i)
  {
    if (lgmpHostStreamNew(hs->host, streamConfig, &hs->inputStreams[i]) !=
        LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostStreamNew(%u) failed", i);
      return false;
    }
    struct LGMPStreamDescriptor desc;
    lgmpHostStreamGetDescriptor(hs->inputStreams[i], &desc);
    hs->streamDescriptors[i] = exportStreamDescriptor(&desc);
  }

  for (unsigned i = 0; i < LGMP_Q_INPUT_LEN; ++i)
  {
    if (lgmpHostMemAlloc(hs->host, sizeof(KVMFRInputStatus),
          &hs->inputStatusMem[i]) != LGMP_OK)
      return false;
  }

  for (unsigned i = 0; i < LGMP_Q_POINTER_LEN; ++i)
  {
    if (lgmpHostMemAlloc(hs->host, POINTER_SLOT_SIZE,
          &hs->pointerMem[i]) != LGMP_OK)
      return false;
  }

  hs->endpointGeneration = 1;
  hs->streamGeneration   = 1;
  hostSessionPublishInputStatus(hs);
  return true;
}

static bool hostSessionEnsureFrameMem(struct HostSession * hs,
    uint32_t frameDataSize)
{
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const uint32_t pageSize = si.dwPageSize ? si.dwPageSize : 4096u;
  const uint32_t offset   = pageSize - (uint32_t)sizeof(KVMFRFrameBuffer);
  const uint32_t needed   = (offset + sizeof(KVMFRFrameBuffer) + frameDataSize +
                             pageSize - 1u) & ~(pageSize - 1u);

  if (hs->frameMem[0] && hs->frameMemSize >= needed && hs->shadowCap >= frameDataSize)
    return true;

  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (hs->frameMem[i])
      lgmpHostMemFree(&hs->frameMem[i]);

  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if (lgmpHostMemAllocAligned(hs->host, needed, pageSize,
          &hs->frameMem[i]) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAllocAligned(%u) failed", needed);
      return false;
    }
  }
  hs->frameMemSize = needed;

  if (hs->shadowFrame)
    _aligned_free(hs->shadowFrame);
  uint8_t * sb = (uint8_t *)_aligned_malloc(frameDataSize, 64);
  if (!sb)
  {
    hs->shadowFrame = NULL;
    hs->shadowCap   = 0;
    return false;
  }
  memset(sb, 0, frameDataSize);
  hs->shadowFrame = sb;
  hs->shadowCap   = frameDataSize;
  return true;
}

static bool hostSessionPostShadowFrame(struct HostSession * hs,
    const KVMFRFrame * srcFrame, uint32_t flags, uint32_t frameDataSize)
{
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const uint32_t pageSize = si.dwPageSize ? si.dwPageSize : 4096u;
  const uint32_t offset   = pageSize - (uint32_t)sizeof(KVMFRFrameBuffer);

  PLGMPMemory slot = hs->frameMem[hs->frameIndex];
  hs->frameIndex = (hs->frameIndex + 1u) % LGMP_Q_FRAME_LEN;

  KVMFRFrame * dstFrame = (KVMFRFrame *)lgmpHostMemPtr(slot);
  *dstFrame        = *srcFrame;
  dstFrame->offset = offset;

  KVMFRFrameBuffer * fb = (KVMFRFrameBuffer *)((uint8_t *)dstFrame + offset);
  framebuffer_prepare(fb);

  lgmpHostQueueNewSubs(hs->frameQueue);
  if (lgmpHostQueuePost(hs->frameQueue, flags, slot) != LGMP_OK)
    return false;

  framebuffer_write(fb, hs->shadowFrame, frameDataSize);
  hs->lastFrame         = *srcFrame;
  hs->lastFrameFlags    = flags;
  hs->lastFrameDataSize = frameDataSize;
  hs->lastFrameValid    = true;
  return true;
}

static bool hostPumpReverseChannels(struct HostSession * hs, SOCKET clientSock)
{
  if (!hs->host)
    return true;

  if (lgmpHostProcess(hs->host) != LGMP_OK)
    return false;

  if (hs->frameQueue && lgmpHostQueueNewSubs(hs->frameQueue) > 0 &&
      hs->lastFrameValid && hs->shadowFrame &&
      lgmpHostQueuePending(hs->frameQueue) < LGMP_Q_FRAME_LEN)
  {
    KVMFRFrame fullFrame = hs->lastFrame;
    fullFrame.damageRectsCount = 0;
    hostSessionPostShadowFrame(hs, &fullFrame, hs->lastFrameFlags,
        hs->lastFrameDataSize);
  }

  if (hs->pointerQueue && lgmpHostQueueNewSubs(hs->pointerQueue) > 0 &&
      hs->lastPointerValid && hs->lastPointerSlot &&
      lgmpHostQueuePending(hs->pointerQueue) < LGMP_Q_POINTER_LEN)
  {
    lgmpHostQueuePost(hs->pointerQueue, hs->lastPointerFlags,
        hs->lastPointerSlot);
  }

  for (;;)
  {
    KVMFRSetCursorPos pos = { 0 };
    size_t size = sizeof(pos);
    if (lgmpHostReadData(hs->pointerQueue, &pos, &size) != LGMP_OK)
      break;

    lgmpHostAckData(hs->pointerQueue);
    if (size == sizeof(pos) &&
        pos.msg.type == KVMFR_MESSAGE_SETCURSORPOS)
    {
      if (!sendPacket(clientSock, BRIDGE_PKT_CURSOR_POS, 0, &pos, sizeof(pos)))
        return false;
    }
  }

  uint32_t clientIDs[32];
  unsigned int count = 32;
  if (lgmpHostGetClientIDs(hs->inputQueue, clientIDs, &count) == LGMP_OK)
  {
    bool boundStillSubscribed = false;
    for (unsigned int i = 0; i < count; ++i)
      if (hs->hasBoundClient && clientIDs[i] == hs->boundClientID)
        boundStillSubscribed = true;

    if (!boundStillSubscribed)
    {
      if (hs->hasOwner)
      {
        KVMFRInputMessage rel = { 0 };
        rel.type       = KVMFR_INPUT_MESSAGE_RELEASE;
        rel.generation = hs->ownerGeneration;
        sendPacket(clientSock, BRIDGE_PKT_INPUT_MSG, 0, &rel, sizeof(rel));
      }
      hs->hasBoundClient  = false;
      hs->boundClientID   = 0;
      hs->hasOwner        = false;
      hs->ownerClientID   = 0;
      hs->ownerGeneration = 0;

      if (count > 0)
      {
        hs->boundClientID  = clientIDs[0];
        hs->hasBoundClient = true;
        lgmpHostStreamBind(hs->inputStreams[0], hs->boundClientID,
            &hs->streamEpoch);
        hostSessionPublishInputStatus(hs);
      }
    }
  }

  if (hs->hasBoundClient)
  {
    for (;;)
    {
      LGMPStreamBuffer buffer = { 0 };
      const LGMP_STATUS st = lgmpHostStreamReadPeek(hs->inputStreams[0], &buffer);
      if (st != LGMP_OK)
        break;

      KVMFRInputMessage msg = { 0 };
      if (buffer.size >= sizeof(msg))
        memcpy(&msg, buffer.data, sizeof(msg));
      lgmpHostStreamReadRelease(hs->inputStreams[0], &buffer);

      if (msg.type == KVMFR_INPUT_MESSAGE_CLAIM && msg.sequence == 1)
      {
        hs->hasOwner        = true;
        hs->ownerClientID   = hs->boundClientID;
        hs->ownerGeneration = msg.generation;
        hostSessionPublishInputStatus(hs);
      }
      else if (msg.type == KVMFR_INPUT_MESSAGE_RELEASE &&
               hs->hasOwner &&
               hs->ownerClientID == hs->boundClientID &&
               hs->ownerGeneration == msg.generation)
      {
        hs->hasOwner        = false;
        hs->ownerClientID   = 0;
        hs->ownerGeneration = 0;
        hostSessionPublishInputStatus(hs);
      }

      if (!sendPacket(clientSock, BRIDGE_PKT_INPUT_MSG, 0, &msg, sizeof(msg)))
        return false;
    }
  }

  return true;
}

static int runHostBridge(const struct BridgeOptions * opts)
{
  const uint64_t deadlineNs = opts->timeoutSec ?
    nanotime() + (uint64_t)opts->timeoutSec * 1000000000ULL : 0;

  option_register((struct Option[])
  {
    {
      .module         = "os",
      .name           = "shmFile",
      .description    = "Shared memory section path",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = (char *)opts->sectionName
    },
    {
      .module      = "os",
      .name        = "shmSize",
      .description = "Shared memory size MiB",
      .type        = OPTION_TYPE_INT,
      .value.x_int = (int)opts->sectionSizeMB
    },
    { 0 }
  });

  struct IVSHMEM shm = { 0 };
  if (!ivshmemInit(&shm) || !ivshmemOpen(&shm))
  {
    DEBUG_ERROR("Failed to create or open host section %s", opts->sectionName);
    return EXIT_FAILURE;
  }

  int      ret      = EXIT_FAILURE;
  SOCKET   listener = opts->tcpConnect ? INVALID_SOCKET : createHostListener(opts);
  SOCKET   client   = INVALID_SOCKET;
  struct HostSession hs = { 0 };

  if (!opts->tcpConnect && listener == INVALID_SOCKET)
    goto out;

  unsigned relayedFrames = 0;

accept_next:
  client = opts->tcpConnect ?
    connectToHost(opts, deadlineNs) :
    acceptClient(listener, deadlineNs);
  if (client == INVALID_SOCKET)
    goto out;

  while (!InterlockedCompareExchange(&stopRequested, 0, 0))
  {
    if (deadlineNs && nanotime() >= deadlineNs)
      break;

    if (hs.host && !hostPumpReverseChannels(&hs, client))
      break;

    if (!socketHasData(client, 1))
      continue;

    struct BridgePacketHeader hdr;
    if (!recvAll(client, &hdr, sizeof(hdr)))
      break;

    if (hdr.magic != BRIDGE_MAGIC || hdr.version != BRIDGE_VERSION)
    {
      DEBUG_ERROR("Invalid packet magic/version from guest");
      break;
    }

    if (hdr.type == BRIDGE_PKT_HELLO)
    {
      uint8_t udata[512];
      if (hdr.payloadSize > sizeof(udata))
        break;
      if (hdr.payloadSize > 0 && !recvAll(client, udata, hdr.payloadSize))
        break;
      hostSessionFree(&hs);
      if (!hostSessionInit(&hs, &shm, udata, hdr.payloadSize))
        break;
    }
    else if (hdr.type == BRIDGE_PKT_POINTER)
    {
      if (hdr.payloadSize < sizeof(struct BridgePointerPacket))
        break;
      struct BridgePointerPacket pkt;
      if (!recvAll(client, &pkt, sizeof(pkt)))
        break;
      uint32_t shapeSize = hdr.payloadSize - (uint32_t)sizeof(pkt);
      if (shapeSize != pkt.shapeSize ||
          sizeof(KVMFRCursor) + shapeSize > POINTER_SLOT_SIZE)
        break;

      if (!hs.host && !hostSessionInit(&hs, &shm, NULL, 0))
        break;

      PLGMPMemory slot = hs.pointerMem[hs.pointerIndex];
      hs.pointerIndex = (hs.pointerIndex + 1u) % LGMP_Q_POINTER_LEN;
      uint8_t * dst = (uint8_t *)lgmpHostMemPtr(slot);
      memcpy(dst, &pkt.cursor, sizeof(pkt.cursor));
      if (shapeSize > 0 && !recvAll(client, dst + sizeof(pkt.cursor), shapeSize))
        break;

      lgmpHostQueueNewSubs(hs.pointerQueue);
      lgmpHostQueuePost(hs.pointerQueue, hdr.flags, slot);
      hs.lastPointerSlot  = slot;
      hs.lastPointerFlags = hdr.flags;
      hs.lastPointerValid = true;
    }
    else if (hdr.type == BRIDGE_PKT_FRAME_FULL)
    {
      if (hdr.payloadSize < sizeof(struct BridgeFramePacket))
        break;
      struct BridgeFramePacket fp;
      if (!recvAll(client, &fp, sizeof(fp)))
        break;
      uint32_t dataSize = hdr.payloadSize - (uint32_t)sizeof(fp);
      if (dataSize != fp.frameDataSize || dataSize == 0)
        break;

      if (!hs.host && !hostSessionInit(&hs, &shm, NULL, 0))
        break;
      if (!hostSessionEnsureFrameMem(&hs, dataSize))
        break;
      if (!recvAll(client, hs.shadowFrame, dataSize))
        break;
      if (!hostSessionPostShadowFrame(&hs, &fp.frame, hdr.flags, dataSize))
        break;

      ++relayedFrames;
      if (relayedFrames == 1)
        DEBUG_INFO("Relayed first frame: %ux%u (data=%ux%u, pitch=%u, type=%u)",
            fp.frame.frameWidth, fp.frame.frameHeight,
            fp.frame.dataWidth, fp.frame.dataHeight,
            fp.frame.pitch, fp.frame.type);
      if (opts->maxFrames && relayedFrames >= opts->maxFrames)
      {
        for (unsigned i = 0; i < 20; ++i)
        {
          if (!hostPumpReverseChannels(&hs, client))
            break;
          Sleep(5);
        }
        ret = EXIT_SUCCESS;
        break;
      }
    }
    else if (hdr.type == BRIDGE_PKT_FRAME_RECTS)
    {
      if (hdr.payloadSize < sizeof(struct BridgeFramePacket))
        break;
      struct BridgeFramePacket fp;
      if (!recvAll(client, &fp, sizeof(fp)))
        break;
      uint32_t rem = hdr.payloadSize - (uint32_t)sizeof(fp);
      if (fp.frameDataSize == 0)
        break;

      if (!hs.host && !hostSessionInit(&hs, &shm, NULL, 0))
        break;
      if (!hostSessionEnsureFrameMem(&hs, fp.frameDataSize))
        break;

      uint32_t bpp = fp.frame.stride ?
        (fp.frame.pitch / fp.frame.stride) : 4u;
      if (bpp == 0 || bpp > 16)
        bpp = 4u;

      const uint32_t dataWidth  = fp.frame.dataWidth  ? fp.frame.dataWidth  : fp.frame.frameWidth;
      const uint32_t dataHeight = fp.frame.dataHeight ? fp.frame.dataHeight : fp.frame.frameHeight;

      bool ok = true;
      for (uint32_t r = 0; r < fp.frame.damageRectsCount; ++r)
      {
        if (rem < sizeof(struct BridgeRectEntry))
        {
          ok = false;
          break;
        }
        struct BridgeRectEntry entry;
        if (!recvAll(client, &entry, sizeof(entry)))
        {
          ok = false;
          break;
        }
        rem -= (uint32_t)sizeof(entry);

        uint64_t rectBytes = (uint64_t)entry.rowBytes * entry.rect.height;
        if (rem < rectBytes ||
            entry.rect.x + entry.rect.width > dataWidth ||
            entry.rect.y + entry.rect.height > dataHeight)
        {
          ok = false;
          break;
        }

        for (uint32_t y = 0; y < entry.rect.height; ++y)
        {
          uint8_t * dstRow = hs.shadowFrame +
            (size_t)(entry.rect.y + y) * fp.frame.pitch +
            (size_t)entry.rect.x * bpp;
          if (!recvAll(client, dstRow, entry.rowBytes))
          {
            ok = false;
            break;
          }
        }
        rem -= (uint32_t)rectBytes;
      }
      if (!ok || rem != 0)
        break;

      if (!hostSessionPostShadowFrame(&hs, &fp.frame, hdr.flags, fp.frameDataSize))
        break;

      ++relayedFrames;
      if (opts->maxFrames && relayedFrames >= opts->maxFrames)
      {
        for (unsigned i = 0; i < 20; ++i)
        {
          if (!hostPumpReverseChannels(&hs, client))
            break;
          Sleep(5);
        }
        ret = EXIT_SUCCESS;
        break;
      }
    }
    else if (hdr.payloadSize > 0)
    {
      uint8_t discard[256];
      uint32_t rem = hdr.payloadSize;
      while (rem > 0)
      {
        uint32_t chunk = rem > sizeof(discard) ? (uint32_t)sizeof(discard) : rem;
        if (!recvAll(client, discard, chunk))
          goto client_done;
        rem -= chunk;
      }
    }
  }

client_done:
  if (!opts->maxFrames &&
      !InterlockedCompareExchange(&stopRequested, 0, 0) &&
      (!deadlineNs || nanotime() < deadlineNs))
  {
    closesocket(client);
    client = INVALID_SOCKET;
    goto accept_next;
  }

  if (!opts->maxFrames && relayedFrames > 0)
    ret = EXIT_SUCCESS;

out:
  hostSessionFree(&hs);
  if (client != INVALID_SOCKET)
    closesocket(client);
  if (listener != INVALID_SOCKET)
    closesocket(listener);
  ivshmemClose(&shm);
  ivshmemFree(&shm);
  return ret;
}

int main(int argc, char * argv[])
{
  debug_init();

  struct BridgeOptions opts;
  if (!parseArgs(argc, argv, &opts))
  {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    return EXIT_FAILURE;

  SetConsoleCtrlHandler(ctrlHandler, TRUE);

  int rc = EXIT_FAILURE;
  do
  {
    rc = opts.isHost ? runHostBridge(&opts) : runGuestBridge(&opts);
    if (opts.maxFrames || opts.timeoutSec)
      break;
    if (!InterlockedCompareExchange(&stopRequested, 0, 0))
      Sleep(200);
  }
  while (!InterlockedCompareExchange(&stopRequested, 0, 0));

  WSACleanup();
  return rc;
}
