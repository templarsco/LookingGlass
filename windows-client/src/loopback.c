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
 * Windows client bring-up: LGMP/KVMFR frame loopback.
 *
 * A producer thread plays the guest capture host. It publishes a KVMFR
 * session, posts one frame on the LGMP frame queue and then streams the
 * pixels through the framebuffer write pointer, using the same frame layout
 * and post-then-write order as host/src/app.c.
 *
 * The main thread plays the viewer. It validates the session like the client
 * LGMP transport, subscribes to the frame queue, reads the padded frame
 * through common/framebuffer and checks every pixel.
 *
 * The shared region is an unnamed mapping private to this process, so the
 * test exposes no endpoint to other processes.
 */

#include "common/debug.h"
#include "common/framebuffer.h"
#include "common/sysinfo.h"
#include "common/thread.h"
#include "common/time.h"

#include <LGProtocol/KVMFR.h>
#include <LGProtocol/LGMPConfig.h>
#include <lgmp/client.h>
#include <lgmp/host.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  #include "common/windebug.h"
#else
  #include <sys/mman.h>
#endif

#define TEST_WIDTH      1280u
#define TEST_HEIGHT     720u
/* a padded stride, so a pitch/width mix-up cannot pass */
#define TEST_STRIDE     1296u
#define TEST_BPP        4u
#define TEST_PITCH      (TEST_STRIDE * TEST_BPP)
#define TEST_FRAME_SIZE (TEST_PITCH * TEST_HEIGHT)
#define TEST_SHM_SIZE   (32u * 1024u * 1024u)
#define TEST_TIMEOUT_NS (10ULL * 1000000000ULL)
#define TEST_POLL_NS    1000000ULL
#define TEST_HOSTVER    "limiar-windows-client-loopback"
#define TEST_PAD_BYTE   0xCD

struct SharedRegion
{
  void   * mem;
  size_t   size;
#ifdef _WIN32
  HANDLE   mapping;
#endif
};

struct SessionData
{
  uint8_t  data[512];
  uint32_t size;
};

struct Producer
{
  PLGMPHost          host;
  PLGMPHostQueue     queue;
  PLGMPMemory        frameMemory;
  KVMFRFrame       * frame;
  KVMFRFrameBuffer * frameBuffer;
  const uint8_t    * source;
  uint64_t           deadline;
  atomic_uint        viewerID;
  atomic_bool        stop;
  atomic_bool        cancel;
  atomic_bool        writeDone;
};

static inline uint32_t testPixel(unsigned x, unsigned y)
{
  const uint8_t b = (uint8_t)x;
  const uint8_t g = (uint8_t)y;
  const uint8_t r = (uint8_t)(x * 7u + y * 13u);
  return (uint32_t)b | (uint32_t)g << 8 | (uint32_t)r << 16 | 0xFF000000u;
}

static bool regionCreate(struct SharedRegion * region, size_t size)
{
  memset(region, 0, sizeof(*region));

#ifdef _WIN32
  /* no name: the section cannot be opened by any other process */
  region->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
      PAGE_READWRITE, 0, (DWORD)size, NULL);
  if (!region->mapping)
  {
    DEBUG_WINERROR("CreateFileMappingW failed", GetLastError());
    return false;
  }

  region->mem = MapViewOfFile(region->mapping, FILE_MAP_ALL_ACCESS, 0, 0,
      size);
  if (!region->mem)
  {
    DEBUG_WINERROR("MapViewOfFile failed", GetLastError());
    CloseHandle(region->mapping);
    region->mapping = NULL;
    return false;
  }
#else
  void * mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED)
  {
    DEBUG_ERROR("mmap failed");
    return false;
  }
  region->mem = mem;
#endif

  region->size = size;
  return true;
}

static void regionFree(struct SharedRegion * region)
{
  if (!region->mem)
    return;

#ifdef _WIN32
  UnmapViewOfFile(region->mem);
  CloseHandle(region->mapping);
  region->mapping = NULL;
#else
  munmap(region->mem, region->size);
#endif

  region->mem  = NULL;
  region->size = 0;
}

/* the SIMD framebuffer writers need an aligned source */
static void * alignedAlloc(size_t size, size_t alignment)
{
#ifdef _WIN32
  return _aligned_malloc(size, alignment);
#else
  return aligned_alloc(alignment, size);
#endif
}

static void alignedFree(void * ptr)
{
#ifdef _WIN32
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

static bool sessionAppend(struct SessionData * session, const void * src,
    size_t size)
{
  if (size > sizeof(session->data) - session->size)
  {
    DEBUG_ERROR("Session data overflow");
    return false;
  }

  memcpy(session->data + session->size, src, size);
  session->size += (uint32_t)size;
  return true;
}

/* the same session layout as newKVMFRData() in host/src/app.c */
static bool sessionBuild(struct SessionData * session)
{
  _Static_assert(sizeof(TEST_HOSTVER) <= sizeof(((KVMFR *)0)->hostver),
      "TEST_HOSTVER does not fit in KVMFR.hostver");

  session->size = 0;

  KVMFR kvmfr =
  {
    .version  = KVMFR_VERSION,
    .features = 0
  };
  memcpy(kvmfr.magic, KVMFR_MAGIC, sizeof(kvmfr.magic));
  memcpy(kvmfr.hostver, TEST_HOSTVER, sizeof(TEST_HOSTVER));
  if (!sessionAppend(session, &kvmfr, sizeof(kvmfr)))
    return false;

  {
    static const char model[] = "Limiar loopback";
    KVMFRRecord_VMInfo vmInfo =
    {
      .cpus    = 1,
      .cores   = 1,
      .sockets = 1
    };
    memcpy(vmInfo.capture, "loopback", sizeof("loopback"));

    const KVMFRRecord record =
    {
      .type = KVMFR_RECORD_VMINFO,
      .size = sizeof(vmInfo) + sizeof(model)
    };

    if (!sessionAppend(session, &record, sizeof(record)) ||
        !sessionAppend(session, &vmInfo, sizeof(vmInfo)) ||
        !sessionAppend(session, model  , sizeof(model )))
      return false;
  }

  {
    static const char osName[] = "Limiar loopback";
    KVMFRRecord_OSInfo osInfo =
    {
#ifdef _WIN32
      .os = KVMFR_OS_WINDOWS
#else
      .os = KVMFR_OS_LINUX
#endif
    };

    const KVMFRRecord record =
    {
      .type = KVMFR_RECORD_OSINFO,
      .size = sizeof(osInfo) + sizeof(osName)
    };

    if (!sessionAppend(session, &record, sizeof(record)) ||
        !sessionAppend(session, &osInfo, sizeof(osInfo)) ||
        !sessionAppend(session, osName , sizeof(osName)))
      return false;
  }

  return true;
}

/* the same checks as lgmp_parseSession() in client/transports/LGMP/lgmp.c */
static bool sessionValidate(const uint8_t * data, uint32_t size)
{
  if (!data || size < sizeof(KVMFR))
  {
    DEBUG_ERROR("Session data is too small: %u bytes", size);
    return false;
  }

  KVMFR header;
  memcpy(&header, data, sizeof(header));
  if (memcmp(header.magic, KVMFR_MAGIC, sizeof(header.magic)) != 0)
  {
    DEBUG_ERROR("Invalid KVMFR magic");
    return false;
  }

  if (header.version != KVMFR_VERSION)
  {
    DEBUG_ERROR("KVMFR version mismatch: expected %u, got %u",
        KVMFR_VERSION, header.version);
    return false;
  }

  if (memcmp(header.hostver, TEST_HOSTVER, sizeof(TEST_HOSTVER)) != 0)
  {
    DEBUG_ERROR("Unexpected host version string");
    return false;
  }

  data += sizeof(header);
  size -= sizeof(header);

  bool vmInfo = false;
  bool osInfo = false;
  while (size >= sizeof(KVMFRRecord))
  {
    KVMFRRecord record;
    memcpy(&record, data, sizeof(record));
    data += sizeof(record);
    size -= sizeof(record);
    if (record.size > size)
    {
      DEBUG_ERROR("Truncated session record %u", record.type);
      return false;
    }

    switch (record.type)
    {
      case KVMFR_RECORD_VMINFO:
        vmInfo = record.size >= sizeof(KVMFRRecord_VMInfo);
        break;

      case KVMFR_RECORD_OSINFO:
        osInfo = record.size >= sizeof(KVMFRRecord_OSInfo);
        break;
    }

    data += record.size;
    size -= record.size;
  }

  if (!vmInfo || !osInfo)
  {
    DEBUG_ERROR("Session is missing the VM or OS information record");
    return false;
  }

  return true;
}

static bool hostProcess(struct Producer * p)
{
  const LGMP_STATUS status = lgmpHostProcess(p->host);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostProcess failed: %s", lgmpStatusString(status));
    return false;
  }
  return true;
}

/* LGMP marks a stalled subscriber bad and stops waiting for it, which also
 * clears its pending messages; only subscribers still in good standing are
 * reported here */
static bool viewerActive(struct Producer * p)
{
  uint32_t     clientIDs[32];
  unsigned int count = 0;

  const LGMP_STATUS status = lgmpHostGetClientIDs(p->queue, clientIDs,
      &count);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostGetClientIDs failed: %s", lgmpStatusString(status));
    return false;
  }

  const uint32_t viewerID = atomic_load(&p->viewerID);
  for (unsigned int i = 0; i < count; ++i)
    if (clientIDs[i] == viewerID)
      return true;

  return false;
}

static int producerThread(void * opaque)
{
  struct Producer * p = (struct Producer *)opaque;

  /* keep the session alive until the viewer subscribes */
  while (!lgmpHostQueueHasSubs(p->queue))
  {
    if (atomic_load(&p->cancel) || !hostProcess(p))
      return 1;

    if (nanotime() > p->deadline)
    {
      DEBUG_ERROR("Timed out waiting for the viewer to subscribe");
      return 1;
    }
    nsleep(TEST_POLL_NS);
  }

  KVMFRFrame * fi = p->frame;
  fi->formatVer        = 1;
  fi->frameSerial      = 1;
  fi->type             = FRAME_TYPE_BGRA;
  fi->screenWidth      = TEST_WIDTH;
  fi->screenHeight     = TEST_HEIGHT;
  fi->dataWidth        = TEST_WIDTH;
  fi->dataHeight       = TEST_HEIGHT;
  fi->frameWidth       = TEST_WIDTH;
  fi->frameHeight      = TEST_HEIGHT;
  fi->rotation         = FRAME_ROT_0;
  fi->stride           = TEST_STRIDE;
  fi->pitch            = TEST_PITCH;
  fi->flags            = 0;
  fi->damageRectsCount = 0;
  fi->sdrWhiteLevel    = KVMFR_SDR_WHITE_LEVEL_DEFAULT;

  framebuffer_prepare(p->frameBuffer);

  /* post and then write, as the capture host does */
  const LGMP_STATUS status = lgmpHostQueuePost(p->queue, 0, p->frameMemory);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueuePost failed: %s", lgmpStatusString(status));
    return 1;
  }

  if (!framebuffer_write(p->frameBuffer, p->source, TEST_FRAME_SIZE))
  {
    DEBUG_ERROR("framebuffer_write failed");
    return 1;
  }
  atomic_store(&p->writeDone, true);

  /* the viewer is done only when the host has seen it release the frame;
   * dropping a stalled viewer also empties the queue, so check which */
  for (;;)
  {
    if (atomic_load(&p->cancel) || !hostProcess(p))
      return 1;

    if (atomic_load(&p->stop) && lgmpHostQueuePending(p->queue) == 0)
    {
      if (viewerActive(p))
        return 0;

      DEBUG_ERROR("The viewer was dropped before it released the frame");
      return 1;
    }

    if (nanotime() > p->deadline)
    {
      DEBUG_ERROR("Timed out waiting for the viewer to release the frame");
      return 1;
    }
    nsleep(TEST_POLL_NS);
  }
}

static bool frameValidate(const LGMPMessage * msg, KVMFRFrame * fi)
{
  if (msg->size < sizeof(*fi))
  {
    DEBUG_ERROR("Frame message is too small: %u bytes", msg->size);
    return false;
  }

  /* snapshot the header, the producer owns the shared copy */
  memcpy(fi, msg->mem, sizeof(*fi));

  if (fi->type        != FRAME_TYPE_BGRA ||
      fi->rotation    != FRAME_ROT_0     ||
      fi->frameSerial != 1               ||
      fi->dataWidth   != TEST_WIDTH      ||
      fi->dataHeight  != TEST_HEIGHT     ||
      fi->frameWidth  != TEST_WIDTH      ||
      fi->frameHeight != TEST_HEIGHT     ||
      fi->stride      != TEST_STRIDE     ||
      fi->pitch       != TEST_PITCH)
  {
    DEBUG_ERROR("Unexpected frame header: type %u, %ux%u, stride %u, "
        "pitch %u", (unsigned)fi->type, fi->dataWidth, fi->dataHeight,
        fi->stride, fi->pitch);
    return false;
  }

  if (fi->damageRectsCount > KVMFR_MAX_DAMAGE_RECTS)
  {
    DEBUG_ERROR("Invalid damage rect count: %u", fi->damageRectsCount);
    return false;
  }

  const uint64_t end = (uint64_t)fi->offset + sizeof(KVMFRFrameBuffer) +
    (uint64_t)fi->pitch * fi->dataHeight;
  if (fi->offset < sizeof(KVMFRFrame) || end > msg->size)
  {
    DEBUG_ERROR("Frame buffer is out of bounds: offset %u, message %u bytes",
        fi->offset, msg->size);
    return false;
  }

  return true;
}

static bool pixelsVerify(const uint8_t * pixels)
{
  for (unsigned y = 0; y < TEST_HEIGHT; ++y)
    for (unsigned x = 0; x < TEST_WIDTH; ++x)
    {
      uint32_t pixel;
      memcpy(&pixel, pixels + ((size_t)y * TEST_WIDTH + x) * TEST_BPP,
          sizeof(pixel));

      const uint32_t expected = testPixel(x, y);
      if (pixel != expected)
      {
        DEBUG_ERROR("Pixel mismatch at %u,%u: got 0x%08x, expected 0x%08x",
            x, y, pixel, expected);
        return false;
      }
    }

  return true;
}

int main(void)
{
  debug_init();

  int                  ret         = EXIT_FAILURE;
  struct SharedRegion  region      = { 0 };
  struct SessionData   session;
  struct Producer      producer    = { 0 };
  LGThread           * thread      = NULL;
  PLGMPClient          client      = NULL;
  PLGMPClientQueue     clientQueue = NULL;
  uint8_t            * source      = NULL;
  uint8_t            * pixels      = NULL;
  LGMP_STATUS          status;

  atomic_init(&producer.viewerID , 0);
  atomic_init(&producer.stop     , false);
  atomic_init(&producer.cancel   , false);
  atomic_init(&producer.writeDone, false);
  producer.deadline = nanotime() + TEST_TIMEOUT_NS;

  const long pageSize = sysinfo_getPageSize();
  if (pageSize <= 0 || (pageSize & (pageSize - 1)) ||
      (size_t)pageSize < sizeof(KVMFRFrame) + sizeof(KVMFRFrameBuffer))
  {
    DEBUG_ERROR("Unusable page size: %ld", pageSize);
    goto out;
  }
  const uint32_t alignSize = (uint32_t)pageSize;

  if (!regionCreate(&region, TEST_SHM_SIZE) || !sessionBuild(&session))
    goto out;

  /* producer: the guest capture host */
  if ((status = lgmpHostInit(region.mem, (uint32_t)region.size,
      &producer.host, session.size, session.data)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostInit failed: %s", lgmpStatusString(status));
    goto out;
  }

  const struct LGMPQueueConfig frameQueueConfig =
  {
    .queueID     = LGMP_Q_FRAME,
    .numMessages = LGMP_Q_FRAME_LEN,
    .subTimeout  = 1000
  };

  if ((status = lgmpHostQueueNew(producer.host, frameQueueConfig,
      &producer.queue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueNew failed: %s", lgmpStatusString(status));
    goto out;
  }

  /* the pixel data starts on the next alignment boundary, as in the host */
  const uint32_t frameMemorySize = alignSize +
    ((TEST_FRAME_SIZE + alignSize - 1) & ~(alignSize - 1));
  if ((status = lgmpHostMemAllocAligned(producer.host, frameMemorySize,
      alignSize, &producer.frameMemory)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostMemAllocAligned failed: %s",
        lgmpStatusString(status));
    goto out;
  }

  const unsigned alignOffset = alignSize - sizeof(KVMFRFrameBuffer);
  producer.frame = lgmpHostMemPtr(producer.frameMemory);
  memset(producer.frame, 0, sizeof(*producer.frame));
  producer.frame->offset = alignOffset;
  producer.frameBuffer   = (KVMFRFrameBuffer *)
    ((uint8_t *)producer.frame + alignOffset);

  source = alignedAlloc(TEST_FRAME_SIZE, 64);
  pixels = malloc((size_t)TEST_WIDTH * TEST_BPP * TEST_HEIGHT);
  if (!source || !pixels)
  {
    DEBUG_ERROR("Out of memory");
    goto out;
  }

  memset(source, TEST_PAD_BYTE, TEST_FRAME_SIZE);
  for (unsigned y = 0; y < TEST_HEIGHT; ++y)
    for (unsigned x = 0; x < TEST_WIDTH; ++x)
    {
      const uint32_t pixel = testPixel(x, y);
      memcpy(source + (size_t)y * TEST_PITCH + (size_t)x * TEST_BPP, &pixel,
          sizeof(pixel));
    }
  producer.source = source;

  if (!lgCreateThread("lgProducer", producerThread, &producer, &thread))
  {
    DEBUG_ERROR("Failed to create the producer thread");
    goto out;
  }

  /* viewer: the Windows client */
  if ((status = lgmpClientInit(region.mem, region.size, &client)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpClientInit failed: %s", lgmpStatusString(status));
    goto out;
  }

  uint32_t   udataSize     = 0;
  uint8_t  * udata         = NULL;
  uint32_t   clientID      = 0;
  uint32_t   remoteVersion = 0;
  while ((status = lgmpClientSessionInit(client, &udataSize, &udata,
      &clientID, &remoteVersion)) != LGMP_OK)
  {
    if (status == LGMP_ERR_INVALID_VERSION)
    {
      DEBUG_ERROR("LGMP version mismatch: expected %u, got %u",
          LGMP_PROTOCOL_VERSION, remoteVersion);
      goto out;
    }

    if (nanotime() > producer.deadline)
    {
      DEBUG_ERROR("lgmpClientSessionInit failed: %s",
          lgmpStatusString(status));
      goto out;
    }
    nsleep(TEST_POLL_NS);
  }

  if (!sessionValidate(udata, udataSize))
    goto out;

  atomic_store(&producer.viewerID, clientID);

  while ((status = lgmpClientSubscribe(client, LGMP_Q_FRAME, &clientQueue))
      != LGMP_OK)
  {
    if (nanotime() > producer.deadline)
    {
      DEBUG_ERROR("lgmpClientSubscribe failed: %s",
          lgmpStatusString(status));
      goto out;
    }
    nsleep(TEST_POLL_NS);
  }

  LGMPMessage msg;
  while ((status = lgmpClientProcess(clientQueue, &msg)) != LGMP_OK)
  {
    if (status != LGMP_ERR_QUEUE_EMPTY || nanotime() > producer.deadline)
    {
      DEBUG_ERROR("lgmpClientProcess failed: %s", lgmpStatusString(status));
      goto out;
    }
    nsleep(TEST_POLL_NS);
  }

  KVMFRFrame fi;
  if (!frameValidate(&msg, &fi))
    goto out;

  const KVMFRFrameBuffer * fb = (const KVMFRFrameBuffer *)
    ((const uint8_t *)msg.mem + fi.offset);

  /* informational: whether the read started while pixels were still being
   * written; the pixel check below is the pass criterion either way */
  const bool overlapped = !atomic_load(&producer.writeDone);

  bool readOk = framebuffer_read(fb, pixels, TEST_WIDTH * TEST_BPP,
      fi.dataHeight, fi.dataWidth, TEST_BPP, fi.pitch);
  if (!readOk)
  {
    /* a descheduled producer can exceed the framebuffer spin limit; wait
     * for the complete frame within the test deadline and read it again */
    while (!framebuffer_wait(fb, (size_t)fi.pitch * fi.dataHeight))
      if (nanotime() > producer.deadline)
      {
        DEBUG_ERROR("Timed out waiting for the frame data");
        goto out;
      }

    readOk = framebuffer_read(fb, pixels, TEST_WIDTH * TEST_BPP,
        fi.dataHeight, fi.dataWidth, TEST_BPP, fi.pitch);
  }

  if (!readOk)
  {
    DEBUG_ERROR("framebuffer_read failed");
    goto out;
  }

  if ((status = lgmpClientMessageDone(clientQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpClientMessageDone failed: %s", lgmpStatusString(status));
    goto out;
  }

  if (!pixelsVerify(pixels))
    goto out;

  atomic_store(&producer.stop, true);
  int producerResult = 1;
  const bool joined = lgJoinThread(thread, &producerResult);
  thread = NULL;
  if (!joined || producerResult != 0)
  {
    DEBUG_ERROR("The producer thread failed");
    goto out;
  }

  printf("Looking Glass Windows client loopback\n");
  printf("  LGMP protocol : %u (client ID %u)\n",
      LGMP_PROTOCOL_VERSION, clientID);
  printf("  KVMFR session : version %u, host \"%s\"\n",
      KVMFR_VERSION, TEST_HOSTVER);
  printf("  Frame         : %ux%u BGRA, stride %u, pitch %u, offset %u\n",
      fi.dataWidth, fi.dataHeight, fi.stride, fi.pitch, fi.offset);
  printf("  Streamed read : %s\n", overlapped ? "yes" : "no");
  printf("  Result        : PASS, %u pixels checked\n",
      TEST_WIDTH * TEST_HEIGHT);
  ret = EXIT_SUCCESS;

out:
  if (thread)
  {
    int unused;
    atomic_store(&producer.cancel, true);
    lgJoinThread(thread, &unused);
  }

  if (clientQueue)
    lgmpClientUnsubscribe(&clientQueue);
  if (client)
    lgmpClientFree(&client);
  if (producer.frameMemory)
    lgmpHostMemFree(&producer.frameMemory);
  if (producer.host)
    lgmpHostFree(&producer.host);

  free(pixels);
  if (source)
    alignedFree(source);
  regionFree(&region);

  if (ret != EXIT_SUCCESS)
    fprintf(stderr, "Looking Glass Windows client loopback: FAIL\n");

  return ret;
}
