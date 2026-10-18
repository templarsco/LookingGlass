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
 * End-to-end test for lg-hyperv-bridge:
 *
 *   Guest LGMP Host (named section)
 *     -> lg-hyperv-bridge.exe --guest
 *     -> TCP loopback
 *     -> lg-hyperv-bridge.exe --host
 *     -> Host LGMP Client (named section)
 *
 * Verifies:
 *   1. Host section advertises KVMFR_FEATURE_SETCURSORPOS | KVMFR_FEATURE_INPUT
 *   2. Full frame relay (BRIDGE_PKT_FRAME_FULL)
 *   3. Damage-rect delta frame relay (BRIDGE_PKT_FRAME_RECTS)
 *   4. Cursor position + shape relay (LGMP_Q_POINTER)
 *   5. KVMFRSetCursorPos return (Host -> Guest LGMP_Q_POINTER + input log)
 *   6. KVMFRInputStatus + LGMP stream binding + KVMFRInputMessage relay
 *      (CLAIM, MOUSE_RELATIVE, KEYBOARD, RELEASE)
 */

#include "session.h"

#include "common/debug.h"
#include "common/framebuffer.h"
#include "common/ivshmem.h"
#include "common/option.h"

#include <LGProtocol/KVMFR.h>
#include <LGProtocol/KVMFRInput.h>
#include <LGProtocol/LGMPConfig.h>
#include <lgmp/client.h>
#include <lgmp/host.h>
#include <lgmp/stream.h>

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "FAIL at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      goto out; \
    } \
  } while (0)

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

static bool spawnProcess(const char * cmdLine, PROCESS_INFORMATION * pi)
{
  STARTUPINFOA si = { .cb = sizeof(si) };
  char mutableCmd[1024];
  snprintf(mutableCmd, sizeof(mutableCmd), "%s", cmdLine);
  return CreateProcessA(NULL, mutableCmd, NULL, NULL, TRUE,
      0, NULL, NULL, &si, pi) != 0;
}

static bool sendStreamMessage(PLGMPClientStream stream,
    const KVMFRInputMessage * msg)
{
  LGMPStreamBuffer buf = { 0 };
  for (unsigned i = 0; i < 200; ++i)
  {
    if (lgmpClientStreamWriteAcquire(stream, &buf) == LGMP_OK)
    {
      memcpy(buf.data, msg, sizeof(*msg));
      return lgmpClientStreamWriteCommit(stream, &buf, sizeof(*msg)) == LGMP_OK;
    }
    Sleep(5);
  }
  return false;
}

int main(int argc, char * argv[])
{
  (void)argc;
  debug_init();

  int                 ret          = EXIT_FAILURE;
  const DWORD         pid          = GetCurrentProcessId();
  const unsigned      port         = 34000u + (unsigned)(pid % 10000u);
  char                guestSec[128];
  char                hostSec[128];
  char                logPath[MAX_PATH];
  char                tempDir[MAX_PATH];
  char                exePath[MAX_PATH];
  char                bridgeExe[MAX_PATH];
  struct IVSHMEM      guestShm     = { 0 };
  struct IVSHMEM      hostShm      = { 0 };
  PLGMPHost           guestHost    = NULL;
  PLGMPHostQueue      guestFrameQ  = NULL;
  PLGMPHostQueue      guestPtrQ    = NULL;
  PLGMPMemory         guestFrameMem[2] = { NULL, NULL };
  PLGMPMemory         guestPtrMem  = NULL;
  PLGMPClient         hostClient   = NULL;
  PLGMPClientStream   inputStream  = NULL;
  PROCESS_INFORMATION piHost       = { 0 };
  PROCESS_INFORMATION piGuest      = { 0 };

  snprintf(guestSec, sizeof(guestSec), "Local\\lg-bridge-guest-%lu",
      (unsigned long)pid);
  snprintf(hostSec, sizeof(hostSec), "Local\\lg-bridge-host-%lu",
      (unsigned long)pid);

  GetTempPathA(sizeof(tempDir), tempDir);
  snprintf(logPath, sizeof(logPath), "%slg-bridge-input-%lu.log",
      tempDir, (unsigned long)pid);
  DeleteFileA(logPath);

  GetModuleFileNameA(NULL, exePath, sizeof(exePath));
  char * lastSlash = strrchr(exePath, '\\');
  if (lastSlash)
  {
    *lastSlash = '\0';
    snprintf(bridgeExe, sizeof(bridgeExe), "%s\\lg-hyperv-bridge.exe", exePath);
  }
  else
    snprintf(bridgeExe, sizeof(bridgeExe), "lg-hyperv-bridge.exe");

  option_register((struct Option[])
  {
    {
      .module         = "os",
      .name           = "shmFile",
      .description    = "Shared memory file",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = guestSec
    },
    {
      .module      = "os",
      .name        = "shmSize",
      .description = "Shared memory size",
      .type        = OPTION_TYPE_INT,
      .value.x_int = 16
    },
    { 0 }
  });

  CHECK(ivshmemInit(&guestShm));
  CHECK(ivshmemOpen(&guestShm));

  struct SessionData sessionInfo = { 0 };
  CHECK(sessionBuild(&sessionInfo, "bridge-test-guest", "D12", "TestGuest"));
  CHECK(lgmpHostInit(guestShm.mem, guestShm.size, &guestHost,
        sessionInfo.size, sessionInfo.data) == LGMP_OK);
  CHECK(lgmpHostQueueNew(guestHost, frameQueueConfig, &guestFrameQ) == LGMP_OK);
  CHECK(lgmpHostQueueNew(guestHost, pointerQueueConfig, &guestPtrQ) == LGMP_OK);

  const uint32_t width     = 64;
  const uint32_t height    = 64;
  const uint32_t pitch     = width * 4;
  const uint32_t frameSize = height * pitch;
  const uint32_t pageSize  = 4096;
  const uint32_t fbOffset  = pageSize - (uint32_t)sizeof(KVMFRFrameBuffer);
  const uint32_t allocSize = (fbOffset + sizeof(KVMFRFrameBuffer) + frameSize +
                              pageSize - 1u) & ~(pageSize - 1u);

  CHECK(lgmpHostMemAllocAligned(guestHost, allocSize, pageSize,
        &guestFrameMem[0]) == LGMP_OK);
  CHECK(lgmpHostMemAllocAligned(guestHost, allocSize, pageSize,
        &guestFrameMem[1]) == LGMP_OK);
  CHECK(lgmpHostMemAlloc(guestHost, sizeof(KVMFRCursor) + 64,
        &guestPtrMem) == LGMP_OK);

  char hostCmd[1024];
  char guestCmd[1024];
  snprintf(hostCmd, sizeof(hostCmd),
      "\"%s\" --host --section=%s --size=16 --tcp=127.0.0.1:%u "
      "--frames=2 --timeout=15",
      bridgeExe, hostSec, port);
  snprintf(guestCmd, sizeof(guestCmd),
      "\"%s\" --guest --section=%s --tcp=127.0.0.1:%u --no-inject "
      "--input-log=\"%s\" --frames=2 --timeout=15",
      bridgeExe, guestSec, port, logPath);

  CHECK(spawnProcess(hostCmd, &piHost));
  CHECK(spawnProcess(guestCmd, &piGuest));

  /* Wait for guest bridge to subscribe to guestHost queues */
  for (unsigned i = 0; i < 200; ++i)
  {
    lgmpHostProcess(guestHost);
    if (lgmpHostQueueHasSubs(guestFrameQ) && lgmpHostQueueHasSubs(guestPtrQ))
      break;
    Sleep(10);
  }
  CHECK(lgmpHostQueueHasSubs(guestFrameQ));
  CHECK(lgmpHostQueueHasSubs(guestPtrQ));

  /* Wait for host bridge to create and populate hostSec */
  for (unsigned i = 0; i < 200; ++i)
  {
    lgmpHostProcess(guestHost);
    if (ivshmemOpenDev(&hostShm, hostSec))
      break;
    Sleep(10);
  }
  CHECK(hostShm.mem != NULL);
  CHECK(lgmpClientInit(hostShm.mem, hostShm.size, &hostClient) == LGMP_OK);

  uint32_t  udataSize     = 0;
  uint32_t  clientID      = 0;
  uint32_t  remoteVersion = 0;
  uint8_t * udata         = NULL;
  for (unsigned i = 0; i < 200; ++i)
  {
    lgmpHostProcess(guestHost);
    if (lgmpClientSessionInit(hostClient, &udataSize, &udata,
          &clientID, &remoteVersion) == LGMP_OK)
      break;
    Sleep(10);
  }
  CHECK(udata != NULL);
  CHECK(udataSize >= sizeof(KVMFR));
  const KVMFR * hostKvmfr = (const KVMFR *)udata;
  CHECK((hostKvmfr->features & KVMFR_FEATURE_SETCURSORPOS) != 0);
  CHECK((hostKvmfr->features & KVMFR_FEATURE_INPUT) != 0);

  PLGMPClientQueue hostFrameQ = NULL;
  PLGMPClientQueue hostPtrQ   = NULL;
  PLGMPClientQueue hostInputQ = NULL;
  for (unsigned i = 0; i < 200; ++i)
  {
    lgmpHostProcess(guestHost);
    if (!hostFrameQ)
      lgmpClientSubscribe(hostClient, LGMP_Q_FRAME, &hostFrameQ);
    if (!hostPtrQ)
      lgmpClientSubscribe(hostClient, LGMP_Q_POINTER, &hostPtrQ);
    if (!hostInputQ)
      lgmpClientSubscribe(hostClient, LGMP_Q_INPUT, &hostInputQ);
    if (hostFrameQ && hostPtrQ && hostInputQ)
      break;
    Sleep(10);
  }
  CHECK(hostFrameQ && hostPtrQ && hostInputQ);

  /* Wait for host bridge to bind stream 0 to clientID and post KVMFRInputStatus */
  KVMFRInputStatus inputStatus = { 0 };
  bool             gotBound    = false;
  for (unsigned i = 0; i < 200; ++i)
  {
    lgmpHostProcess(guestHost);
    LGMPMessage msg;
    if (lgmpClientProcess(hostInputQ, &msg) == LGMP_OK)
    {
      memcpy(&inputStatus, msg.mem, sizeof(inputStatus));
      lgmpClientMessageDone(hostInputQ);
      if ((inputStatus.flags & KVMFR_INPUT_STATUS_AVAILABLE) &&
          (inputStatus.streamEndpoint[0].flags &
           KVMFR_INPUT_STREAM_ENDPOINT_BOUND) &&
          inputStatus.streamEndpoint[0].boundClientID == clientID)
      {
        gotBound = true;
        break;
      }
    }
    Sleep(10);
  }
  CHECK(gotBound);

  struct LGMPStreamDescriptor desc =
  {
    .magic      = inputStatus.streamEndpoint[0].stream.magic,
    .version    = inputStatus.streamEndpoint[0].stream.version,
    .size       = inputStatus.streamEndpoint[0].stream.size,
    .offset     = inputStatus.streamEndpoint[0].stream.offset,
    .regionSize = inputStatus.streamEndpoint[0].stream.regionSize,
    .direction  = inputStatus.streamEndpoint[0].stream.direction,
    .policy     = inputStatus.streamEndpoint[0].stream.policy,
    .slotCount  = inputStatus.streamEndpoint[0].stream.slotCount,
    .slotSize   = inputStatus.streamEndpoint[0].stream.slotSize,
  };
  uint32_t activeEpoch = 0;
  CHECK(lgmpClientStreamAttach(hostClient, &desc, &inputStream) == LGMP_OK);
  CHECK(lgmpClientStreamActivate(inputStream, &activeEpoch) == LGMP_OK);

  /* Send CLAIM, MOUSE_RELATIVE, KEYBOARD, RELEASE, and KVMFRSetCursorPos */
  KVMFRInputMessage inMsg = { 0 };
  inMsg.type       = KVMFR_INPUT_MESSAGE_CLAIM;
  inMsg.generation = 7;
  inMsg.sequence   = 1;
  CHECK(sendStreamMessage(inputStream, &inMsg));

  memset(&inMsg, 0, sizeof(inMsg));
  inMsg.type                          = KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE;
  inMsg.generation                    = 7;
  inMsg.sequence                      = 2;
  inMsg.payload.mouseRelative.deltaX  = 12;
  inMsg.payload.mouseRelative.deltaY  = -5;
  inMsg.payload.mouseRelative.wheel   = 1;
  inMsg.payload.mouseRelative.buttons = 1;
  CHECK(sendStreamMessage(inputStream, &inMsg));

  memset(&inMsg, 0, sizeof(inMsg));
  inMsg.type                          = KVMFR_INPUT_MESSAGE_KEYBOARD;
  inMsg.generation                    = 7;
  inMsg.sequence                      = 3;
  inMsg.payload.keyboard.modifiers    = 0x02; /* Left Shift */
  inMsg.payload.keyboard.keys[0]      = 4;    /* KEY_A */
  CHECK(sendStreamMessage(inputStream, &inMsg));

  memset(&inMsg, 0, sizeof(inMsg));
  inMsg.type       = KVMFR_INPUT_MESSAGE_RELEASE;
  inMsg.generation = 7;
  inMsg.sequence   = 4;
  CHECK(sendStreamMessage(inputStream, &inMsg));

  KVMFRSetCursorPos setPos =
  {
    .msg.type = KVMFR_MESSAGE_SETCURSORPOS,
    .x        = 42,
    .y        = 84
  };
  CHECK(lgmpClientSendData(hostPtrQ, &setPos, sizeof(setPos), NULL) == LGMP_OK);

  /* Post a pointer shape + position on guestHost */
  {
    KVMFRCursor * cur = (KVMFRCursor *)lgmpHostMemPtr(guestPtrMem);
    memset(cur, 0, sizeof(*cur));
    cur->x      = 15;
    cur->y      = 25;
    cur->type   = CURSOR_TYPE_COLOR;
    cur->width  = 4;
    cur->height = 4;
    cur->pitch  = 16;
    uint8_t * shape = (uint8_t *)(cur + 1);
    for (unsigned i = 0; i < 64; ++i)
      shape[i] = (uint8_t)(0xA0u + i);
    CHECK(lgmpHostQueuePost(guestPtrQ,
          CURSOR_FLAG_POSITION | CURSOR_FLAG_SHAPE, guestPtrMem) == LGMP_OK);
  }

  /* Post Frame 1 (full frame filled with 0x33) on guestHost */
  {
    KVMFRFrame * f1 = (KVMFRFrame *)lgmpHostMemPtr(guestFrameMem[0]);
    memset(f1, 0, sizeof(*f1));
    f1->formatVer        = 1;
    f1->frameSerial      = 1;
    f1->type             = FRAME_TYPE_BGRA;
    f1->screenWidth      = width;
    f1->screenHeight     = height;
    f1->dataWidth        = width;
    f1->dataHeight       = height;
    f1->frameWidth       = width;
    f1->frameHeight      = height;
    f1->stride           = width;
    f1->pitch            = pitch;
    f1->offset           = fbOffset;
    f1->damageRectsCount = 0;

    KVMFRFrameBuffer * fb = (KVMFRFrameBuffer *)((uint8_t *)f1 + fbOffset);
    framebuffer_prepare(fb);
    CHECK(lgmpHostQueuePost(guestFrameQ, 0, guestFrameMem[0]) == LGMP_OK);

    uint8_t * buf = (uint8_t *)_aligned_malloc(frameSize, 64);
    CHECK(buf != NULL);
    memset(buf, 0x33, frameSize);
    framebuffer_write(fb, buf, frameSize);
    _aligned_free(buf);
  }

  /* Receive pointer and Frame 1 on hostClient */
  bool gotPtr    = false;
  bool gotFrame1 = false;
  for (unsigned i = 0; i < 200 && (!gotPtr || !gotFrame1); ++i)
  {
    lgmpHostProcess(guestHost);
    LGMPMessage msg;
    if (!gotPtr && lgmpClientProcess(hostPtrQ, &msg) == LGMP_OK)
    {
      const KVMFRCursor * c = (const KVMFRCursor *)msg.mem;
      const uint8_t * s     = (const uint8_t *)(c + 1);
      CHECK(c->x == 15 && c->y == 25 && c->width == 4 && c->height == 4);
      CHECK(s[0] == 0xA0u && s[63] == 0xA0u + 63u);
      lgmpClientMessageDone(hostPtrQ);
      gotPtr = true;
    }
    if (!gotFrame1 && lgmpClientProcess(hostFrameQ, &msg) == LGMP_OK)
    {
      const KVMFRFrame * f = (const KVMFRFrame *)msg.mem;
      const KVMFRFrameBuffer * fb =
        (const KVMFRFrameBuffer *)((const uint8_t *)f + f->offset);
      CHECK(framebuffer_wait(fb, frameSize));
      const uint8_t * px = framebuffer_get_buffer(fb);
      CHECK(px[0] == 0x33 && px[frameSize - 1] == 0x33);
      lgmpClientMessageDone(hostFrameQ);
      gotFrame1 = true;
    }
    Sleep(10);
  }
  CHECK(gotPtr && gotFrame1);

  /* Post Frame 2 (damage rect at x=8, y=8, w=8, h=8 set to 0x77) */
  {
    KVMFRFrame * f2 = (KVMFRFrame *)lgmpHostMemPtr(guestFrameMem[1]);
    memset(f2, 0, sizeof(*f2));
    f2->formatVer        = 1;
    f2->frameSerial      = 2;
    f2->type             = FRAME_TYPE_BGRA;
    f2->screenWidth      = width;
    f2->screenHeight     = height;
    f2->dataWidth        = width;
    f2->dataHeight       = height;
    f2->frameWidth       = width;
    f2->frameHeight      = height;
    f2->stride           = width;
    f2->pitch            = pitch;
    f2->offset           = fbOffset;
    f2->damageRectsCount = 1;
    f2->damageRects[0]   = (KVMFRFrameDamageRect)
    {
      .x = 8, .y = 8, .width = 8, .height = 8
    };

    KVMFRFrameBuffer * fb = (KVMFRFrameBuffer *)((uint8_t *)f2 + fbOffset);
    framebuffer_prepare(fb);
    CHECK(lgmpHostQueuePost(guestFrameQ, 0, guestFrameMem[1]) == LGMP_OK);

    uint8_t * buf = (uint8_t *)_aligned_malloc(frameSize, 64);
    CHECK(buf != NULL);
    memset(buf, 0x33, frameSize);
    for (uint32_t y = 8; y < 16; ++y)
      memset(buf + y * pitch + 8 * 4, 0x77, 8 * 4);
    framebuffer_write(fb, buf, frameSize);
    _aligned_free(buf);
  }

  /* Receive Frame 2 on hostClient and verify both preserved & damaged pixels */
  bool gotFrame2 = false;
  for (unsigned i = 0; i < 200 && !gotFrame2; ++i)
  {
    lgmpHostProcess(guestHost);
    LGMPMessage msg;
    if (lgmpClientProcess(hostFrameQ, &msg) == LGMP_OK)
    {
      const KVMFRFrame * f = (const KVMFRFrame *)msg.mem;
      const KVMFRFrameBuffer * fb =
        (const KVMFRFrameBuffer *)((const uint8_t *)f + f->offset);
      CHECK(f->frameSerial == 2);
      CHECK(f->damageRectsCount == 1);
      CHECK(framebuffer_wait(fb, frameSize));
      const uint8_t * px = framebuffer_get_buffer(fb);
      CHECK(px[0] == 0x33);
      CHECK(px[8 * pitch + 8 * 4] == 0x77);
      lgmpClientMessageDone(hostFrameQ);
      gotFrame2 = true;
    }
    Sleep(10);
  }
  CHECK(gotFrame2);

  /* Wait for both bridge processes to finish */
  CHECK(WaitForSingleObject(piGuest.hProcess, 10000) == WAIT_OBJECT_0);
  CHECK(WaitForSingleObject(piHost.hProcess, 10000) == WAIT_OBJECT_0);

  DWORD exitGuest = 1, exitHost = 1;
  GetExitCodeProcess(piGuest.hProcess, &exitGuest);
  GetExitCodeProcess(piHost.hProcess, &exitHost);
  CHECK(exitGuest == 0);
  CHECK(exitHost == 0);

  /* Verify guestHost received KVMFRSetCursorPos on LGMP_Q_POINTER */
  bool gotGuestCursorPos = false;
  lgmpHostProcess(guestHost);
  for (;;)
  {
    KVMFRSetCursorPos pos = { 0 };
    size_t sz = sizeof(pos);
    if (lgmpHostReadData(guestPtrQ, &pos, &sz) != LGMP_OK)
      break;

    lgmpHostAckData(guestPtrQ);
    if (sz == sizeof(pos) && pos.x == 42 && pos.y == 84)
      gotGuestCursorPos = true;
  }
  CHECK(gotGuestCursorPos);

  /* Verify the guest bridge input log recorded all events */
  FILE * lf = fopen(logPath, "r");
  CHECK(lf != NULL);
  char logContent[2048];
  size_t nRead = fread(logContent, 1, sizeof(logContent) - 1, lf);
  fclose(lf);
  logContent[nRead] = '\0';

  CHECK(strstr(logContent, "CLAIM gen=7 seq=1") != NULL);
  CHECK(strstr(logContent, "MOUSE_REL dx=12 dy=-5 wheel=1 buttons=0x01") != NULL);
  CHECK(strstr(logContent, "KEYBOARD mod=0x02 keys=4,0,0,0,0,0") != NULL);
  CHECK(strstr(logContent, "RELEASE gen=7 seq=4") != NULL);
  CHECK(strstr(logContent, "CURSOR_POS x=42 y=84") != NULL);

  ret = EXIT_SUCCESS;

out:
  if (inputStream)
    lgmpClientStreamDetach(&inputStream);
  if (hostClient)
    lgmpClientFree(&hostClient);
  ivshmemClose(&hostShm);

  if (piGuest.hProcess)
  {
    TerminateProcess(piGuest.hProcess, 0);
    CloseHandle(piGuest.hProcess);
    CloseHandle(piGuest.hThread);
  }
  if (piHost.hProcess)
  {
    TerminateProcess(piHost.hProcess, 0);
    CloseHandle(piHost.hProcess);
    CloseHandle(piHost.hThread);
  }

  for (unsigned i = 0; i < 2; ++i)
    if (guestFrameMem[i])
      lgmpHostMemFree(&guestFrameMem[i]);
  if (guestPtrMem)
    lgmpHostMemFree(&guestPtrMem);
  if (guestHost)
    lgmpHostFree(&guestHost);
  ivshmemClose(&guestShm);
  ivshmemFree(&guestShm);
  DeleteFileA(logPath);
  return ret;
}
