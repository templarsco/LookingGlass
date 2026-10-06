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
 *
 * --then makes it change after the frames of a step. The size can change
 * with the session going on, as when the guest sets another resolution, and
 * a step can start a new session on the same memory, as when the capture host
 * restarts, which LGMP gives a new session ID so that its clients see it.
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

#define MAX_STEPS        8
#define DEFAULT_DWELL_MS 1000u
#define MAX_PAUSE_MS     60000u

/* what the producer serves until its frames, counted from the start of the
 * session, have all been posted */
struct Step
{
  unsigned width;
  unsigned height;
  unsigned frames;   /* the serial that ends it, or zero to go on */
  bool     restart;  /* a new LGMP session starts with it */
};

struct Options
{
  const char * name;
  struct Step  steps[MAX_STEPS];
  unsigned     stepCount;
  unsigned     fps;
  unsigned     timeout;
  unsigned     dwell;    /* milliseconds before the next step */
  unsigned     gap;      /* milliseconds without a host before a new session */
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
    "  --then=WxH:N[:restart]\n"
    "                 after the frames so far, post frames up to N at this size,\n"
    "                 or from 1 to N in a new session on the same memory with\n"
    "                 'restart'; up to %u times, each after a --frames or --then\n"
    "                 that ends\n"
    "  --dwell=MS     wait this long after a step's last frame, %u by default\n"
    "  --gap=MS       stop serving this long before a new session, 0 by default;\n"
    "                 a client sees the host gone after a second\n"
    "  --fps=N        frames per second, 60 by default\n"
    "  --timeout=S    stop after S seconds\n"
    "  --world        also let every account open the section, which the\n"
    "                 client must refuse\n",
    program, MAX_STEPS - 1, DEFAULT_DWELL_MS);
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

/* a number that ends where the end points, which is not at its start */
static bool parseNumber(const char * value, const char ** end,
    unsigned * result)
{
  if (*value < '0' || *value > '9')
    return false;

  char * stop;
  const unsigned long parsed = strtoul(value, &stop, 10);
  if (parsed > UINT32_MAX)
    return false;

  *result = (unsigned)parsed;
  *end    = stop;
  return true;
}

/* WxH:N or WxH:N:restart */
static bool parseStep(const char * value, struct Step * step)
{
  *step = (struct Step) { 0 };
  if (!parseNumber(value, &value, &step->width) || *value++ != 'x' ||
      !parseNumber(value, &value, &step->height) || *value++ != ':' ||
      !parseNumber(value, &value, &step->frames))
    return false;

  if (!*value)
    return true;

  step->restart = strcmp(value, ":restart") == 0;
  return step->restart;
}

static bool checkSteps(const struct Options * opts)
{
  for (unsigned i = 0; i < opts->stepCount; ++i)
  {
    const struct Step * step = &opts->steps[i];
    if (!step->width || !step->height ||
        step->width > MAX_DIMENSION || step->height > MAX_DIMENSION)
    {
      fprintf(stderr, "The frame size must be between 1x1 and %ux%u\n",
          MAX_DIMENSION, MAX_DIMENSION);
      return false;
    }

    if (i + 1 < opts->stepCount && !step->frames)
    {
      fprintf(stderr, "--frames is needed for a step that --then follows\n");
      return false;
    }

    if (!i)
      continue;

    if (!step->frames)
    {
      fprintf(stderr, "A --then step needs at least one frame\n");
      return false;
    }

    // the serial goes on within a session
    if (!step->restart && step->frames <= opts->steps[i - 1].frames)
    {
      fprintf(stderr, "A --then step that does not restart must end after "
          "frame %u, where the step before it ends\n",
          opts->steps[i - 1].frames);
      return false;
    }
  }
  return true;
}

static bool parseArgs(int argc, char * argv[], struct Options * opts)
{
  *opts = (struct Options)
  {
    .steps     = { { .width = 1280, .height = 720 } },
    .stepCount = 1,
    .fps       = 60,
    .dwell     = DEFAULT_DWELL_MS
  };

  for (int i = 1; i < argc; ++i)
  {
    const char * arg = argv[i];
    bool valid = true;

    if (strncmp(arg, "--size=", 7) == 0)
    {
      char extra;
      valid = sscanf(arg + 7, "%ux%u%c", &opts->steps[0].width,
          &opts->steps[0].height, &extra) == 2;
    }
    else if (strncmp(arg, "--frames=", 9) == 0)
      valid = parseUnsigned(arg + 9, &opts->steps[0].frames);
    else if (strncmp(arg, "--then=", 7) == 0)
      valid = opts->stepCount < MAX_STEPS &&
        parseStep(arg + 7, &opts->steps[opts->stepCount]) &&
        ++opts->stepCount;
    else if (strncmp(arg, "--dwell=", 8) == 0)
      valid = parseUnsigned(arg + 8, &opts->dwell) &&
        opts->dwell <= MAX_PAUSE_MS;
    else if (strncmp(arg, "--gap=", 6) == 0)
      valid = parseUnsigned(arg + 6, &opts->gap) && opts->gap <= MAX_PAUSE_MS;
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

  if (!checkSteps(opts))
    return false;

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
static void generateFrame(uint8_t * data, unsigned width, unsigned height,
    uint32_t pitch, uint32_t serial)
{
  memset(data, 0xa5 ^ (uint8_t)serial, (size_t)pitch * height);

  const unsigned widthRange  = width  > 1 ? width  - 1 : 1;
  const unsigned heightRange = height > 1 ? height - 1 : 1;

  unsigned boxSize = width < height ? width : height;
  if (boxSize > 64)
    boxSize = 64;
  const unsigned rangeX = width  > boxSize ? width  - boxSize : 1;
  const unsigned rangeY = height > boxSize ? height - boxSize : 1;
  const unsigned boxX   = (serial * 7) % rangeX;
  const unsigned boxY   = (serial * 5) % rangeY;
  const uint32_t color  = serial * 2654435761U;

  for (unsigned y = 0; y < height; ++y)
  {
    uint8_t * row = data + (size_t)y * pitch;
    for (unsigned x = 0; x < width; ++x)
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

/* what a session of the host has: the host, its queues and the memory of its
 * frames, which a restart gives up and makes again */
struct Session
{
  PLGMPHost      host;
  PLGMPHostQueue frameQueue;
  PLGMPHostQueue pointerQueue;
  PLGMPMemory    frameMemory[LGMP_Q_FRAME_LEN];
  KVMFRFrame   * frames     [LGMP_Q_FRAME_LEN];
};

static void sessionStop(struct Session * session)
{
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (session->frameMemory[i])
      lgmpHostMemFree(&session->frameMemory[i]);
  if (session->host)
    lgmpHostFree(&session->host);

  *session = (struct Session) { 0 };
}

static bool sessionStart(struct Session * session, uint8_t * mem,
    uint32_t sectionSize, struct SessionData * info, uint32_t frameMemorySize,
    uint32_t alignSize)
{
  LGMP_STATUS status;

  *session = (struct Session) { 0 };
  if ((status = lgmpHostInit(mem, sectionSize, &session->host, info->size,
      info->data)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostInit failed: %s", lgmpStatusString(status));
    goto fail;
  }

  if ((status = lgmpHostQueueNew(session->host, frameQueueConfig,
        &session->frameQueue)) != LGMP_OK ||
      (status = lgmpHostQueueNew(session->host, pointerQueueConfig,
        &session->pointerQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueNew failed: %s", lgmpStatusString(status));
    goto fail;
  }

  /* the pixel data starts on the next alignment boundary, as in the host */
  const uint32_t alignOffset = alignSize - sizeof(KVMFRFrameBuffer);
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if ((status = lgmpHostMemAllocAligned(session->host, frameMemorySize,
        alignSize, &session->frameMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAllocAligned failed: %s",
          lgmpStatusString(status));
      goto fail;
    }

    session->frames[i] = lgmpHostMemPtr(session->frameMemory[i]);
    memset(session->frames[i], 0, sizeof(*session->frames[i]));
    session->frames[i]->offset = alignOffset;
  }
  return true;

fail:
  sessionStop(session);
  return false;
}

/* sleeps, and ends early if the producer is told to stop */
static void pauseFor(unsigned milliseconds)
{
  const uint64_t end = nanotime() + (uint64_t)milliseconds * 1000000ULL;
  while (!InterlockedCompareExchange(&stopRequested, 0, 0) &&
         nanotime() < end)
    nsleep(POLL_NS);
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

  int                ret     = EXIT_FAILURE;
  HANDLE             section = NULL;
  uint8_t          * mem     = NULL;
  uint8_t          * source  = NULL;
  struct Session     session = { 0 };
  struct SessionData info;

  const long pageSize = sysinfo_getPageSize();
  if (pageSize <= 0 || (pageSize & (pageSize - 1)) ||
      (size_t)pageSize < sizeof(KVMFRFrame) + sizeof(KVMFRFrameBuffer))
  {
    DEBUG_ERROR("Unusable page size: %ld", pageSize);
    goto out;
  }

  /* the memory has room for the largest frame of any step */
  const uint32_t alignSize       = (uint32_t)pageSize;
  uint32_t       largestFrame    = 0;
  uint32_t       frameMemorySize = 0;
  for (unsigned i = 0; i < opts.stepCount; ++i)
  {
    const uint32_t size = (opts.steps[i].width + STRIDE_PADDING) * 4 *
      opts.steps[i].height;
    const uint32_t memorySize = alignSize +
      ((size + alignSize - 1) & ~(alignSize - 1));
    if (size > largestFrame)
      largestFrame = size;
    if (memorySize > frameMemorySize)
      frameMemorySize = memorySize;
  }
  const uint32_t sectionSize =
    LGMP_Q_FRAME_LEN * frameMemorySize + LGMP_OVERHEAD;

  if (!(section = sectionCreate(&opts, sectionSize)))
    goto out;

  if (!(mem = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0,
      sectionSize)))
  {
    DEBUG_WINERROR("MapViewOfFile failed", GetLastError());
    goto out;
  }

  if (!sessionBuild(&info, PRODUCER_HOSTVER, "producer",
        "Limiar test producer"))
    goto out;

  if (!sessionStart(&session, mem, sectionSize, &info, frameMemorySize,
        alignSize))
    goto out;

  /* the SIMD framebuffer writers need an aligned source */
  if (!(source = _aligned_malloc(largestFrame, 64)))
  {
    DEBUG_ERROR("Out of memory");
    goto out;
  }

  SetConsoleCtrlHandler(ctrlHandler, TRUE);

  printf("Serving %ux%u BGRA frames with a stride of %u on %s\n",
      opts.steps[0].width, opts.steps[0].height,
      opts.steps[0].width + STRIDE_PADDING, opts.name);
  fflush(stdout);

  const uint64_t start    = nanotime();
  const uint64_t interval = 1000000000ULL / opts.fps;
  uint64_t       next     = start;
  uint64_t       finished = 0;  /* when the step's last frame was posted */
  unsigned       stepNo   = 0;
  unsigned       sessions = 1;
  uint32_t       serial   = 0;
  uint32_t       format   = 0;
  unsigned       index    = 0;
  unsigned       clients  = 0;
  unsigned       lastW    = 0;
  unsigned       lastH    = 0;

  while (!InterlockedCompareExchange(&stopRequested, 0, 0))
  {
    const uint64_t now = nanotime();
    if (opts.timeout && now - start >= opts.timeout * 1000000000ULL)
    {
      DEBUG_INFO("Stopping after %u seconds", opts.timeout);
      break;
    }

    LGMP_STATUS status;
    if ((status = lgmpHostProcess(session.host)) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostProcess failed: %s", lgmpStatusString(status));
      goto out;
    }

    // the client can send cursor positions, which this producer ignores
    uint8_t message[LGMP_MSGS_SIZE];
    size_t  messageSize;
    while (lgmpHostReadData(session.pointerQueue, message, &messageSize) ==
        LGMP_OK)
      lgmpHostAckData(session.pointerQueue);

    if (!lgmpHostQueueHasSubs(session.frameQueue))
    {
      nsleep(POLL_NS);
      continue;
    }

    // reading the count of new subscribers resets it
    const uint32_t newSubs = lgmpHostQueueNewSubs(session.frameQueue);
    clients += newSubs;

    const struct Step * step = &opts.steps[stepNo];
    if (step->frames && serial >= step->frames)
    {
      // a client that joins after the last frame gets it again, as in the host
      if (newSubs &&
          (status = lgmpHostQueuePost(session.frameQueue, 0,
            session.frameMemory[(index + LGMP_Q_FRAME_LEN - 1) %
              LGMP_Q_FRAME_LEN])) != LGMP_OK)
        DEBUG_ERROR("lgmpHostQueuePost failed: %s", lgmpStatusString(status));

      if (stepNo + 1 == opts.stepCount)
      {
        nsleep(POLL_NS);
        continue;
      }

      // the client gets to take this step's frames, and then to show them,
      // before the next step comes
      if (lgmpHostQueuePending(session.frameQueue))
      {
        nsleep(POLL_NS);
        continue;
      }

      if (!finished)
        finished = now;
      if (now - finished < (uint64_t)opts.dwell * 1000000ULL)
      {
        nsleep(POLL_NS);
        continue;
      }

      finished = 0;
      const struct Step * following = &opts.steps[++stepNo];
      if (!following->restart)
      {
        printf("Serving %ux%u frames up to frame %u\n", following->width,
            following->height, following->frames);
        fflush(stdout);
        continue;
      }

      // as if the capture host had stopped and started again: the memory is
      // the same, and the new host gives it a new session ID
      sessionStop(&session);
      printf("The host is gone\n");
      fflush(stdout);
      pauseFor(opts.gap);
      if (InterlockedCompareExchange(&stopRequested, 0, 0))
        break;

      if (!sessionStart(&session, mem, sectionSize, &info, frameMemorySize,
            alignSize))
        goto out;

      serial = 0;
      format = 0;
      index  = 0;
      lastW  = 0;
      lastH  = 0;
      next   = nanotime();
      printf("Serving %ux%u frames up to frame %u in session %u\n",
          following->width, following->height, following->frames,
          ++sessions);
      fflush(stdout);
      continue;
    }

    // the oldest buffer is free once fewer frames than buffers are pending
    if (now < next ||
        lgmpHostQueuePending(session.frameQueue) == LGMP_Q_FRAME_LEN)
    {
      nsleep(POLL_NS);
      continue;
    }

    const unsigned width     = step->width;
    const unsigned height    = step->height;
    const uint32_t stride    = width + STRIDE_PADDING;
    const uint32_t pitch     = stride * 4;
    const uint32_t frameSize = pitch * height;

    KVMFRFrame * fi = session.frames[index];
    KVMFRFrameBuffer * fb =
      (KVMFRFrameBuffer *)((uint8_t *)fi + fi->offset);

    // the host counts a new format when the capture changes size
    if (!serial || width != lastW || height != lastH)
      ++format;
    lastW = width;
    lastH = height;

    fi->formatVer        = format;
    fi->frameSerial      = ++serial;
    fi->type             = FRAME_TYPE_BGRA;
    fi->screenWidth      = width;
    fi->screenHeight     = height;
    fi->dataWidth        = width;
    fi->dataHeight       = height;
    fi->frameWidth       = width;
    fi->frameHeight      = height;
    fi->rotation         = FRAME_ROT_0;
    fi->stride           = stride;
    fi->pitch            = pitch;
    fi->flags            = 0;
    fi->damageRectsCount = 0;
    fi->sdrWhiteLevel    = KVMFR_SDR_WHITE_LEVEL_DEFAULT;
    __atomic_store_n(&fi->timingValid, 0, __ATOMIC_RELAXED);

    generateFrame(source, width, height, pitch, serial);
    framebuffer_prepare(fb);

    /* post and then write, as the capture host does */
    if ((status = lgmpHostQueuePost(session.frameQueue, 0,
          session.frameMemory[index])) != LGMP_OK)
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

  if (sessions > 1)
    printf("Posted %u frames in the last of %u sessions, %u clients "
        "subscribed\n", serial, sessions, clients);
  else
    printf("Posted %u frames, %u clients subscribed\n", serial, clients);
  ret = EXIT_SUCCESS;

out:
  if (source)
    _aligned_free(source);
  sessionStop(&session);
  if (mem)
    UnmapViewOfFile(mem);
  if (section)
    CloseHandle(section);
  return ret;
}
