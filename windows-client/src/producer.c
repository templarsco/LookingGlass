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
 * Windows client test producer: a stand-in for the guest's capture host on
 * the PC's side of the shared memory.
 *
 * It creates a named section that only this user and the system can open,
 * publishes a KVMFR session on it and posts BGRA frames with a padded
 * stride, in the frame layout and post-then-write order of host/src/app.c.
 * The pixels are the pattern of the client's test transport, so a capture
 * of the client's window is checked the same way for both transports.
 *
 * With --world the section also lets every account open it, which the client
 * must refuse.
 */

#include "session.h"

#include "common/debug.h"
#include "common/framebuffer.h"
#include "common/sysinfo.h"
#include "common/time.h"
#include "common/windebug.h"

#include <LGProtocol/KVMFR.h>
#include <LGProtocol/LGMPConfig.h>
#include <lgmp/host.h>

#include <windows.h>
#include <sddl.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRODUCER_HOSTVER "limiar-windows-client-producer"
/* pixels of padding per row, so a pitch/width mix-up shows */
#define STRIDE_PADDING   16u
#define MAX_DIMENSION    8192u
/* room for the LGMP header and queues next to the frame buffers */
#define LGMP_OVERHEAD    (1024u * 1024u)
#define POLL_NS          1000000ULL

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

struct Options
{
  const char * name;
  unsigned     width;
  unsigned     height;
  unsigned     frames;
  unsigned     fps;
  unsigned     timeout;
  bool         world;
};

static volatile LONG stopRequested = 0;

static BOOL WINAPI ctrlHandler(DWORD type)
{
  InterlockedExchange(&stopRequested, 1);
  return TRUE;
}

static void usage(const char * program)
{
  fprintf(stderr,
    "Usage: %s [options] NAME\n"
    "\n"
    "Creates the shared memory section NAME, such as Local\\looking-glass, and\n"
    "serves Looking Glass frames on it until stopped.\n"
    "\n"
    "  --size=WxH     frame size, 1280x720 by default\n"
    "  --frames=N     post frames 1 to N and then keep the last one; by default\n"
    "                 the frames keep changing\n"
    "  --fps=N        frames per second, 60 by default\n"
    "  --timeout=S    stop after S seconds\n"
    "  --world        also let every account open the section, which the\n"
    "                 client must refuse\n",
    program);
}

static bool parseUnsigned(const char * value, unsigned * result)
{
  char * end;
  const unsigned long parsed = strtoul(value, &end, 10);
  if (!*value || *end || parsed > UINT32_MAX)
    return false;

  *result = (unsigned)parsed;
  return true;
}

static bool parseArgs(int argc, char * argv[], struct Options * opts)
{
  *opts = (struct Options)
  {
    .width  = 1280,
    .height = 720,
    .fps    = 60
  };

  for (int i = 1; i < argc; ++i)
  {
    const char * arg = argv[i];
    bool valid = true;

    if (strncmp(arg, "--size=", 7) == 0)
    {
      char extra;
      valid = sscanf(arg + 7, "%ux%u%c", &opts->width, &opts->height,
          &extra) == 2;
    }
    else if (strncmp(arg, "--frames=", 9) == 0)
      valid = parseUnsigned(arg + 9, &opts->frames);
    else if (strncmp(arg, "--fps=", 6) == 0)
      valid = parseUnsigned(arg + 6, &opts->fps);
    else if (strncmp(arg, "--timeout=", 10) == 0)
      valid = parseUnsigned(arg + 10, &opts->timeout);
    else if (strcmp(arg, "--world") == 0)
      opts->world = true;
    else if (arg[0] != '-' && !opts->name)
      opts->name = arg;
    else
      valid = false;

    if (!valid)
    {
      fprintf(stderr, "Invalid argument: %s\n", arg);
      return false;
    }
  }

  if (!opts->name)
  {
    fprintf(stderr, "The section name is missing\n");
    return false;
  }

  if (!opts->width || !opts->height ||
      opts->width > MAX_DIMENSION || opts->height > MAX_DIMENSION)
  {
    fprintf(stderr, "The frame size must be between 1x1 and %ux%u\n",
        MAX_DIMENSION, MAX_DIMENSION);
    return false;
  }

  if (!opts->fps || opts->fps > 1000)
  {
    fprintf(stderr, "The frame rate must be between 1 and 1000\n");
    return false;
  }

  return true;
}

static void * tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS type)
{
  DWORD size = 0;
  GetTokenInformation(token, type, NULL, 0, &size);

  void * info = size ? malloc(size) : NULL;
  if (!info || !GetTokenInformation(token, type, info, size, &size))
  {
    DEBUG_WINERROR("GetTokenInformation failed", GetLastError());
    free(info);
    return NULL;
  }
  return info;
}

/* owned by this user, with a protected DACL for this user and the system;
 * the VM's account would be added here when a VM maps the section */
static HANDLE sectionCreate(const struct Options * opts, uint32_t size)
{
  HANDLE               section   = NULL;
  HANDLE               token     = NULL;
  TOKEN_USER         * user      = NULL;
  char               * userSid   = NULL;
  PSECURITY_DESCRIPTOR sd        = NULL;

  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
  {
    DEBUG_WINERROR("OpenProcessToken failed", GetLastError());
    goto out;
  }

  if (!(user = tokenInfo(token, TokenUser)))
    goto out;

  if (!ConvertSidToStringSidA(user->User.Sid, &userSid))
  {
    DEBUG_WINERROR("ConvertSidToStringSidA failed", GetLastError());
    goto out;
  }

  // an explicit owner, as the default owner can be a group
  char sddl[512];
  snprintf(sddl, sizeof(sddl), "O:%sD:P(A;;GA;;;SY)(A;;GA;;;%s)%s", userSid,
      userSid, opts->world ? "(A;;GRGW;;;WD)" : "");

  if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl,
        SDDL_REVISION_1, &sd, NULL))
  {
    DEBUG_WINERROR("ConvertStringSecurityDescriptorToSecurityDescriptorA "
        "failed", GetLastError());
    goto out;
  }

  SECURITY_ATTRIBUTES sa =
  {
    .nLength              = sizeof(sa),
    .lpSecurityDescriptor = sd,
    .bInheritHandle       = FALSE
  };

  section = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa,
      PAGE_READWRITE | SEC_COMMIT, 0, size, opts->name);
  if (!section)
  {
    DEBUG_WINERROR("CreateFileMappingA failed", GetLastError());
    goto out;
  }

  // someone else's section keeps their DACL
  if (GetLastError() == ERROR_ALREADY_EXISTS)
  {
    DEBUG_ERROR("The section %s already exists", opts->name);
    CloseHandle(section);
    section = NULL;
  }

out:
  LocalFree(sd);
  LocalFree(userSid);
  free(user);
  if (token)
    CloseHandle(token);
  return section;
}

/* the pattern of test_getColor() in client/transports/Test/test.c, with
 * padding bytes that change every frame */
static void generateFrame(uint8_t * data, const struct Options * opts,
    uint32_t pitch, uint32_t serial)
{
  memset(data, 0xa5 ^ (uint8_t)serial, (size_t)pitch * opts->height);

  const unsigned widthRange  = opts->width  > 1 ? opts->width  - 1 : 1;
  const unsigned heightRange = opts->height > 1 ? opts->height - 1 : 1;

  unsigned boxSize = opts->width < opts->height ? opts->width : opts->height;
  if (boxSize > 64)
    boxSize = 64;
  const unsigned rangeX = opts->width  > boxSize ? opts->width  - boxSize : 1;
  const unsigned rangeY = opts->height > boxSize ? opts->height - boxSize : 1;
  const unsigned boxX   = (serial * 7) % rangeX;
  const unsigned boxY   = (serial * 5) % rangeY;
  const uint32_t color  = serial * 2654435761U;

  for (unsigned y = 0; y < opts->height; ++y)
  {
    uint8_t * row = data + (size_t)y * pitch;
    for (unsigned x = 0; x < opts->width; ++x)
    {
      uint8_t r = (uint64_t)x * 255 / widthRange;
      uint8_t g = (uint64_t)y * 255 / heightRange;
      uint8_t b = ((x / 32) ^ (y / 32)) & 1 ? 0x30 : 0x90;
      if (x >= boxX && y >= boxY && x < boxX + boxSize && y < boxY + boxSize)
      {
        r = color >> 16;
        g = color >> 8;
        b = color;
      }

      uint8_t * pixel = row + (size_t)x * 4;
      pixel[0] = b;
      pixel[1] = g;
      pixel[2] = r;
      pixel[3] = 0xff;
    }
  }
}

int main(int argc, char * argv[])
{
  debug_init();

  struct Options opts;
  if (!parseArgs(argc, argv, &opts))
  {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  int                ret          = EXIT_FAILURE;
  HANDLE             section      = NULL;
  uint8_t          * mem          = NULL;
  PLGMPHost          host         = NULL;
  PLGMPHostQueue     frameQueue   = NULL;
  PLGMPHostQueue     pointerQueue = NULL;
  PLGMPMemory        frameMemory[LGMP_Q_FRAME_LEN] = { 0 };
  KVMFRFrame       * frames     [LGMP_Q_FRAME_LEN] = { 0 };
  uint8_t          * source       = NULL;
  struct SessionData session;
  LGMP_STATUS        status;

  const long pageSize = sysinfo_getPageSize();
  if (pageSize <= 0 || (pageSize & (pageSize - 1)) ||
      (size_t)pageSize < sizeof(KVMFRFrame) + sizeof(KVMFRFrameBuffer))
  {
    DEBUG_ERROR("Unusable page size: %ld", pageSize);
    goto out;
  }

  const uint32_t alignSize       = (uint32_t)pageSize;
  const uint32_t stride          = opts.width + STRIDE_PADDING;
  const uint32_t pitch           = stride * 4;
  const uint32_t frameSize       = pitch * opts.height;
  const uint32_t frameMemorySize = alignSize +
    ((frameSize + alignSize - 1) & ~(alignSize - 1));
  const uint32_t sectionSize     =
    LGMP_Q_FRAME_LEN * frameMemorySize + LGMP_OVERHEAD;

  if (!(section = sectionCreate(&opts, sectionSize)))
    goto out;

  if (!(mem = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0,
      sectionSize)))
  {
    DEBUG_WINERROR("MapViewOfFile failed", GetLastError());
    goto out;
  }

  if (!sessionBuild(&session, PRODUCER_HOSTVER, "producer",
        "Limiar test producer"))
    goto out;

  if ((status = lgmpHostInit(mem, sectionSize, &host, session.size,
      session.data)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostInit failed: %s", lgmpStatusString(status));
    goto out;
  }

  if ((status = lgmpHostQueueNew(host, frameQueueConfig, &frameQueue))
        != LGMP_OK ||
      (status = lgmpHostQueueNew(host, pointerQueueConfig, &pointerQueue))
        != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueNew failed: %s", lgmpStatusString(status));
    goto out;
  }

  /* the pixel data starts on the next alignment boundary, as in the host */
  const uint32_t alignOffset = alignSize - sizeof(KVMFRFrameBuffer);
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if ((status = lgmpHostMemAllocAligned(host, frameMemorySize, alignSize,
        &frameMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAllocAligned failed: %s",
          lgmpStatusString(status));
      goto out;
    }

    frames[i] = lgmpHostMemPtr(frameMemory[i]);
    memset(frames[i], 0, sizeof(*frames[i]));
    frames[i]->offset = alignOffset;
  }

  /* the SIMD framebuffer writers need an aligned source */
  if (!(source = _aligned_malloc(frameSize, 64)))
  {
    DEBUG_ERROR("Out of memory");
    goto out;
  }

  SetConsoleCtrlHandler(ctrlHandler, TRUE);

  printf("Serving %ux%u BGRA frames with a stride of %u on %s\n",
      opts.width, opts.height, stride, opts.name);
  fflush(stdout);

  const uint64_t start    = nanotime();
  const uint64_t interval = 1000000000ULL / opts.fps;
  uint64_t       next     = start;
  uint32_t       serial   = 0;
  unsigned       index    = 0;
  unsigned       clients  = 0;

  while (!InterlockedCompareExchange(&stopRequested, 0, 0))
  {
    const uint64_t now = nanotime();
    if (opts.timeout && now - start >= opts.timeout * 1000000000ULL)
    {
      DEBUG_INFO("Stopping after %u seconds", opts.timeout);
      break;
    }

    if ((status = lgmpHostProcess(host)) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostProcess failed: %s", lgmpStatusString(status));
      goto out;
    }

    // the client can send cursor positions, which this producer ignores
    uint8_t data[LGMP_MSGS_SIZE];
    size_t  dataSize;
    while (lgmpHostReadData(pointerQueue, data, &dataSize) == LGMP_OK)
      lgmpHostAckData(pointerQueue);

    if (!lgmpHostQueueHasSubs(frameQueue))
    {
      nsleep(POLL_NS);
      continue;
    }

    // reading the count of new subscribers resets it
    const uint32_t newSubs = lgmpHostQueueNewSubs(frameQueue);
    clients += newSubs;

    // a client that joins after the last frame gets it again, as in the host
    if (opts.frames && serial >= opts.frames)
    {
      if (newSubs &&
          (status = lgmpHostQueuePost(frameQueue, 0,
            frameMemory[(index + LGMP_Q_FRAME_LEN - 1) % LGMP_Q_FRAME_LEN]))
            != LGMP_OK)
        DEBUG_ERROR("lgmpHostQueuePost failed: %s", lgmpStatusString(status));

      nsleep(POLL_NS);
      continue;
    }

    // the oldest buffer is free once fewer frames than buffers are pending
    if (now < next || lgmpHostQueuePending(frameQueue) == LGMP_Q_FRAME_LEN)
    {
      nsleep(POLL_NS);
      continue;
    }

    KVMFRFrame * fi = frames[index];
    KVMFRFrameBuffer * fb =
      (KVMFRFrameBuffer *)((uint8_t *)fi + fi->offset);

    fi->formatVer        = 1;
    fi->frameSerial      = ++serial;
    fi->type             = FRAME_TYPE_BGRA;
    fi->screenWidth      = opts.width;
    fi->screenHeight     = opts.height;
    fi->dataWidth        = opts.width;
    fi->dataHeight       = opts.height;
    fi->frameWidth       = opts.width;
    fi->frameHeight      = opts.height;
    fi->rotation         = FRAME_ROT_0;
    fi->stride           = stride;
    fi->pitch            = pitch;
    fi->flags            = 0;
    fi->damageRectsCount = 0;
    fi->sdrWhiteLevel    = KVMFR_SDR_WHITE_LEVEL_DEFAULT;
    __atomic_store_n(&fi->timingValid, 0, __ATOMIC_RELAXED);

    generateFrame(source, &opts, pitch, serial);
    framebuffer_prepare(fb);

    /* post and then write, as the capture host does */
    if ((status = lgmpHostQueuePost(frameQueue, 0, frameMemory[index]))
        != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostQueuePost failed: %s", lgmpStatusString(status));
      goto out;
    }

    if (!framebuffer_write(fb, source, frameSize))
    {
      DEBUG_ERROR("framebuffer_write failed");
      goto out;
    }

    index = (index + 1) % LGMP_Q_FRAME_LEN;
    next  = now + interval;
  }

  printf("Posted %u frames, %u clients subscribed\n", serial, clients);
  ret = EXIT_SUCCESS;

out:
  if (source)
    _aligned_free(source);
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (frameMemory[i])
      lgmpHostMemFree(&frameMemory[i]);
  if (host)
    lgmpHostFree(&host);
  if (mem)
    UnmapViewOfFile(mem);
  if (section)
    CloseHandle(section);
  return ret;
}
