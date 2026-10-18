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

/* The Direct3D 11 renderer, for Windows.
 *
 * It draws into the window with a flip model swap chain, which the window
 * manager shows without copying it, and it needs no OpenGL driver. On a PC
 * that has none, as a virtual machine or a remote session does not, it draws
 * with Windows' own software rasterizer (WARP).
 *
 * d3d11.dll is loaded when the renderer is made, and not linked in, so a
 * Windows that does not have it only loses this renderer. */

#define COBJMACROS
#define CINTERFACE

#include "interface/renderer.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/debug.h"
#include "common/option.h"
#include "common/stringlist.h"
#include "common/time.h"
#include "common/util.h"

enum Adapter
{
  ADAPTER_AUTO,     // the GPU, and WARP if there is none that works
  ADAPTER_HARDWARE, // the GPU only
  ADAPTER_WARP,     // the software rasterizer only
};

static const struct
{
  const char  * name;
  enum Adapter  adapter;
}
adapters[] =
{
  { "auto"    , ADAPTER_AUTO     },
  { "hardware", ADAPTER_HARDWARE },
  { "warp"    , ADAPTER_WARP     },
};

static bool parseAdapter(const char * name, enum Adapter * adapter)
{
  for (size_t i = 0; name && i < ARRAYSIZE(adapters); ++i)
    if (strcasecmp(name, adapters[i].name) == 0)
    {
      if (adapter)
        *adapter = adapters[i].adapter;
      return true;
    }
  return false;
}

static bool adapterValidate(struct Option * opt, const char ** error)
{
  if (parseAdapter(opt->value.x_string, NULL))
    return true;

  *error = "The adapter must be auto, hardware or warp";
  return false;
}

static StringList adapterValues(struct Option * opt)
{
  StringList sl = stringlist_new(false);
  if (!sl)
    return NULL;

  // this typecast is safe as the stringlist doesn't own the values
  for (size_t i = 0; i < ARRAYSIZE(adapters); ++i)
    stringlist_push(sl, (char *)adapters[i].name);
  return sl;
}

static struct Option d3d11_options[] =
{
  {
    .module         = "d3d11",
    .name           = "adapter",
    .description    = "The adapter to draw with: auto (the GPU, or WARP if "
                      "there is none), hardware or warp (the software "
                      "rasterizer)",
    .type           = OPTION_TYPE_STRING,
    .value.x_string = "auto",
    .validator      = adapterValidate,
    .getValues      = adapterValues,
  },
  {
    .module       = "d3d11",
    .name         = "vsync",
    .description  = "Wait for the display's vertical blank to show a frame",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = false,
  },
  {0}
};

struct Inst
{
  LG_Renderer base; // the core's view of the renderer, which must be first

  enum Adapter adapter;
  bool         vsync;

  HMODULE                 library;
  PFN_D3D11_CREATE_DEVICE createDevice;

  ID3D11Device        * device;
  ID3D11DeviceContext * context;
  D3D_FEATURE_LEVEL     level;
  bool                  software; // the device is WARP

  HWND                     window;
  IDXGISwapChain1        * swapChain;
  ID3D11RenderTargetView * backBufferView;
  UINT                     backWidth, backHeight;

  // the update that was accepted and not consumed by a render yet
  atomic_uint_least64_t pendingToken;
};

/* the renderer */

static void d3d11_setup(void)
{
  option_register(d3d11_options);
}

static const char * d3d11_getName(void)
{
  return "D3D11";
}

static bool d3d11_create(LG_Renderer ** renderer,
    const LG_RendererParams params, bool * needsOpenGL)
{
  struct Inst * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("Out of memory");
    return false;
  }

  if (!parseAdapter(option_get_string("d3d11", "adapter"), &this->adapter))
  {
    DEBUG_ERROR("d3d11:adapter must be auto, hardware or warp");
    free(this);
    return false;
  }
  this->vsync = option_get_bool("d3d11", "vsync");

  // a Windows without Direct3D 11, as one that has no desktop experience, is
  // one where this renderer is not available
  this->library = LoadLibraryExW(L"d3d11.dll", NULL,
      LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (this->library)
    this->createDevice = (PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(
        this->library, "D3D11CreateDevice");
  if (!this->createDevice)
  {
    DEBUG_ERROR("Direct3D 11 is not available");
    if (this->library)
      FreeLibrary(this->library);
    free(this);
    return false;
  }

  // the window is drawn into by Direct3D, and must not have an OpenGL context
  *needsOpenGL = false;
  *renderer    = &this->base;
  return true;
}

/* the device */

static HRESULT makeDevice(struct Inst * this, D3D_DRIVER_TYPE type)
{
  static const D3D_FEATURE_LEVEL levels[] =
  {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
  };

  HRESULT hr = this->createDevice(NULL, type, NULL, 0, levels,
      ARRAYSIZE(levels), D3D11_SDK_VERSION, &this->device, &this->level,
      &this->context);

  // a Windows that does not know feature level 11.1 refuses a list that has it
  if (hr == E_INVALIDARG)
    hr = this->createDevice(NULL, type, NULL, 0, levels + 1,
        ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, &this->device, &this->level,
        &this->context);

  this->software = SUCCEEDED(hr) && type == D3D_DRIVER_TYPE_WARP;
  return hr;
}

// what the device is made on, which a report of a problem needs
static void logAdapter(struct Inst * this)
{
  IDXGIDevice * dxgiDevice = NULL;
  if (FAILED(ID3D11Device_QueryInterface(this->device, &IID_IDXGIDevice,
          (void **)&dxgiDevice)))
    return;

  IDXGIAdapter  * adapter  = NULL;
  IDXGIAdapter1 * adapter1 = NULL;
  if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgiDevice, &adapter)) &&
      SUCCEEDED(IDXGIAdapter_QueryInterface(adapter, &IID_IDXGIAdapter1,
          (void **)&adapter1)))
  {
    DXGI_ADAPTER_DESC1 desc;
    char name[256] = "";
    if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter1, &desc)) &&
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name,
          sizeof(name), NULL, NULL))
    {
      // a GPU's driver can be the software one too, as in a virtual machine
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        this->software = true;

      DEBUG_INFO("Adapter      : %s (%04x:%04x)%s", name, desc.VendorId,
          desc.DeviceId, this->software ? ", software" : "");
    }
  }

  if (adapter1)
    IDXGIAdapter1_Release(adapter1);
  if (adapter)
    IDXGIAdapter_Release(adapter);
  IDXGIDevice_Release(dxgiDevice);
}

static bool d3d11_initialize(LG_Renderer * renderer)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  HRESULT hr = E_FAIL;
  if (this->adapter != ADAPTER_WARP)
  {
    hr = makeDevice(this, D3D_DRIVER_TYPE_HARDWARE);
    if (FAILED(hr))
      DEBUG_WARN("No Direct3D 11 device on a GPU (0x%08lx)%s",
          (unsigned long)hr,
          this->adapter == ADAPTER_AUTO ? ", trying WARP" : "");
  }

  if (FAILED(hr) && this->adapter != ADAPTER_HARDWARE)
    hr = makeDevice(this, D3D_DRIVER_TYPE_WARP);

  if (FAILED(hr))
  {
    DEBUG_ERROR("Could not make a Direct3D 11 device (0x%08lx)",
        (unsigned long)hr);
    return false;
  }

  logAdapter(this);
  DEBUG_INFO("Feature level: %u.%u", ((unsigned)this->level >> 12) & 0xF,
      ((unsigned)this->level >> 8) & 0xF);
  return true;
}

static void releaseBackBuffer(struct Inst * this)
{
  if (this->backBufferView)
  {
    ID3D11RenderTargetView_Release(this->backBufferView);
    this->backBufferView = NULL;
  }
}

static void d3d11_deinitialize(LG_Renderer * renderer)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  releaseBackBuffer(this);
  if (this->context)
  {
    // so that nothing refers to the swap chain's buffers when it goes
    ID3D11DeviceContext_ClearState(this->context);
    ID3D11DeviceContext_Flush(this->context);
  }
  if (this->swapChain)
    IDXGISwapChain1_Release(this->swapChain);
  if (this->context)
    ID3D11DeviceContext_Release(this->context);
  if (this->device)
    ID3D11Device_Release(this->device);
  if (this->library)
    FreeLibrary(this->library);
  free(this);
}

/* the swap chain */

static bool makeBackBufferView(struct Inst * this)
{
  ID3D11Texture2D * texture = NULL;
  HRESULT hr = IDXGISwapChain1_GetBuffer(this->swapChain, 0,
      &IID_ID3D11Texture2D, (void **)&texture);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Could not get the back buffer (0x%08lx)", (unsigned long)hr);
    return false;
  }

  D3D11_TEXTURE2D_DESC desc;
  ID3D11Texture2D_GetDesc(texture, &desc);
  this->backWidth  = desc.Width;
  this->backHeight = desc.Height;

  hr = ID3D11Device_CreateRenderTargetView(this->device,
      (ID3D11Resource *)texture, NULL, &this->backBufferView);
  ID3D11Texture2D_Release(texture);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Could not make a view of the back buffer (0x%08lx)",
        (unsigned long)hr);
    return false;
  }
  return true;
}

/* The size of the window in pixels, which is what the buffers have, and not
 * what the core was told, which has been through the scale and may be a pixel
 * off. A minimized window has none. */
static bool clientSize(struct Inst * this, UINT * width, UINT * height)
{
  RECT rect;
  if (!GetClientRect(this->window, &rect) || rect.right <= 0 ||
      rect.bottom <= 0)
    return false;

  *width  = (UINT)rect.right;
  *height = (UINT)rect.bottom;
  return true;
}

static bool d3d11_renderStartup(LG_Renderer * renderer, bool useDMA)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  void * window = NULL;
  if (!app_getProp(LG_DS_NATIVE_WINDOW, &window) || !window)
  {
    DEBUG_ERROR("The display server has no window to draw in");
    return false;
  }
  this->window = (HWND)window;

  UINT width = 1, height = 1;
  clientSize(this, &width, &height);

  IDXGIDevice   * dxgiDevice = NULL;
  IDXGIAdapter  * adapter    = NULL;
  IDXGIFactory2 * factory    = NULL;
  HRESULT hr = ID3D11Device_QueryInterface(this->device, &IID_IDXGIDevice,
      (void **)&dxgiDevice);
  if (SUCCEEDED(hr))
    hr = IDXGIDevice_GetAdapter(dxgiDevice, &adapter);
  if (SUCCEEDED(hr))
    hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2,
        (void **)&factory);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Could not get the factory of the device (0x%08lx)",
        (unsigned long)hr);
    goto out;
  }

  /* A flip model swap chain: the window manager shows the buffer that is drawn
   * into, and does not copy it as it does for the model before it. */
  const DXGI_SWAP_CHAIN_DESC1 desc =
  {
    .Width       = width,
    .Height      = height,
    .Format      = DXGI_FORMAT_B8G8R8A8_UNORM,
    .SampleDesc  = { .Count = 1 },
    .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
    .BufferCount = 2,
    .Scaling     = DXGI_SCALING_STRETCH,
    .SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD,
    .AlphaMode   = DXGI_ALPHA_MODE_IGNORE,
  };

  hr = IDXGIFactory2_CreateSwapChainForHwnd(factory,
      (IUnknown *)this->device, this->window, &desc, NULL, NULL,
      &this->swapChain);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Could not make the swap chain (0x%08lx)", (unsigned long)hr);
    goto out;
  }

  // the client's own keys, and not Alt+Enter, make the window fullscreen
  IDXGIFactory2_MakeWindowAssociation(factory, this->window,
      DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

  if (!makeBackBufferView(this))
  {
    hr = E_FAIL;
    goto out;
  }

  DEBUG_INFO("Swap chain   : %ux%u, flip model", this->backWidth,
      this->backHeight);

out:
  if (factory)
    IDXGIFactory2_Release(factory);
  if (adapter)
    IDXGIAdapter_Release(adapter);
  if (dxgiDevice)
    IDXGIDevice_Release(dxgiDevice);
  return SUCCEEDED(hr);
}

/* the events of the core */

static void d3d11_onRestart(LG_Renderer * renderer)
{
}

static void d3d11_onResize(LG_Renderer * renderer, const int width,
    const int height, const double scale, const LG_RendererRect destRect,
    LG_RendererRotate rotate)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  UINT pixelsW, pixelsH;
  if (!this->swapChain || !clientSize(this, &pixelsW, &pixelsH) ||
      (pixelsW == this->backWidth && pixelsH == this->backHeight))
    return;

  // the buffers can only change when nothing refers to them
  releaseBackBuffer(this);
  ID3D11DeviceContext_ClearState(this->context);
  ID3D11DeviceContext_Flush(this->context);

  const HRESULT hr = IDXGISwapChain1_ResizeBuffers(this->swapChain, 0,
      pixelsW, pixelsH, DXGI_FORMAT_UNKNOWN, 0);
  if (FAILED(hr))
  {
    // without a view of the back buffer, the next render says so and ends
    DEBUG_ERROR("Could not resize the swap chain to %ux%u (0x%08lx)", pixelsW,
        pixelsH, (unsigned long)hr);
    return;
  }

  makeBackBufferView(this);
}

static bool d3d11_onFontUpdate(LG_Renderer * renderer)
{
  return true;
}

static bool d3d11_onMouseShape(LG_Renderer * renderer,
    const LG_RendererCursor cursor, const int width, const int height,
    const int pitch, const uint8_t * data)
{
  return true;
}

static bool d3d11_onMouseEvent(LG_Renderer * renderer, const bool visible,
    int x, int y, const int hx, const int hy)
{
  return true;
}

static bool d3d11_onFrameFormat(LG_Renderer * renderer,
    const LG_RendererFormat format)
{
  return true;
}

static bool d3d11_onFrame(LG_Renderer * renderer,
    const KVMFRFrameBuffer * frame, int dmaFD,
    const KVMFRFrameDamageRect * damage, int damageCount,
    LG_RendererFrameToken frameToken, LG_FrameReleaseFn releaseFn,
    void * releaseOpaque, uint64_t releaseHandle)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  // the frame is not drawn yet, so it is not sampled and is let go of at once
  if (releaseFn)
    releaseFn(releaseOpaque, releaseHandle);

  atomic_store_explicit(&this->pendingToken, frameToken,
      memory_order_release);
  return true;
}

/* drawing */

static bool presentFailed(struct Inst * this, HRESULT hr)
{
  DEBUG_ERROR("Present failed (0x%08lx)", (unsigned long)hr);
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    DEBUG_ERROR("The GPU was removed or reset (0x%08lx)",
        (unsigned long)ID3D11Device_GetDeviceRemovedReason(this->device));
  return false;
}

static bool d3d11_render(LG_Renderer * renderer, LG_RendererRotate rotate,
    LG_RendererFrameToken frameTokenLimit, const bool invalidateWindow,
    void (*preSwap)(void * udata), void * udata,
    LG_RendererFrameTiming * timing)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  if (!this->backBufferView)
  {
    DEBUG_ERROR("There is no back buffer to draw in");
    return false;
  }

  // consume the update that has come, as one that is not drawn yet
  uint_least64_t pending = atomic_load_explicit(&this->pendingToken,
      memory_order_acquire);
  if (pending != LG_RENDERER_FRAME_TOKEN_NONE && pending <= frameTokenLimit &&
      atomic_compare_exchange_strong(&this->pendingToken, &pending,
        LG_RENDERER_FRAME_TOKEN_NONE))
    timing->frameToken = pending;

  // the colour of what is behind the frame
  static const float clear[4] = { 0.03f, 0.03f, 0.05f, 1.0f };
  ID3D11DeviceContext_OMSetRenderTargets(this->context, 1,
      &this->backBufferView, NULL);
  ID3D11DeviceContext_ClearRenderTargetView(this->context,
      this->backBufferView, clear);

  preSwap(udata);

  const uint64_t swapStart = nanotime();
  const HRESULT hr = IDXGISwapChain1_Present(this->swapChain,
      this->vsync ? 1 : 0, 0);
  timing->swapTime = nanotime() - swapStart;

  // a window that nobody can see is a success, DXGI_STATUS_OCCLUDED
  return SUCCEEDED(hr) ? true : presentFailed(this, hr);
}

/* what the tests read back: the composed window, as OpenGL gives it */

static bool d3d11_capture(LG_Renderer * renderer, LG_RendererCapture * capture)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  ID3D11Texture2D * back = NULL;
  if (!this->swapChain || FAILED(IDXGISwapChain1_GetBuffer(this->swapChain, 0,
          &IID_ID3D11Texture2D, (void **)&back)))
    return false;

  D3D11_TEXTURE2D_DESC desc;
  ID3D11Texture2D_GetDesc(back, &desc);
  desc.Usage          = D3D11_USAGE_STAGING;
  desc.BindFlags      = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags      = 0;

  ID3D11Texture2D * staging = NULL;
  uint8_t         * data    = NULL;
  const size_t      stride  = (size_t)desc.Width * 4;

  if (SUCCEEDED(ID3D11Device_CreateTexture2D(this->device, &desc, NULL,
          &staging)))
  {
    ID3D11DeviceContext_CopyResource(this->context,
        (ID3D11Resource *)staging, (ID3D11Resource *)back);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ID3D11DeviceContext_Map(this->context,
            (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)))
    {
      data = malloc(stride * desc.Height);
      for (UINT y = 0; data && y < desc.Height; ++y)
      {
        // from the bottom up, in the byte order RGBA
        const uint8_t * source = (const uint8_t *)mapped.pData +
          (size_t)y * mapped.RowPitch;
        uint8_t * target = data + (size_t)(desc.Height - 1 - y) * stride;
        for (UINT x = 0; x < desc.Width; ++x)
        {
          target[x * 4 + 0] = source[x * 4 + 2];
          target[x * 4 + 1] = source[x * 4 + 1];
          target[x * 4 + 2] = source[x * 4 + 0];
          target[x * 4 + 3] = source[x * 4 + 3];
        }
      }
      ID3D11DeviceContext_Unmap(this->context, (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
  }
  ID3D11Texture2D_Release(back);

  if (!data)
    return false;

  capture->width     = desc.Width;
  capture->height    = desc.Height;
  capture->stride    = stride;
  capture->dataSize  = stride * desc.Height;
  capture->format    = LG_CAPTURE_RGBA8;
  capture->hdr       = false;
  capture->hdrPQ     = false;
  capture->nativeHDR = false;
  capture->data      = data;
  return true;
}

/* what the overlays use, which are not drawn yet */

static char noTexture;

static void * d3d11_createTexture(LG_Renderer * renderer, int width,
    int height, uint8_t * data)
{
  // something that is not NULL, as the overlays take that for a failure
  return &noTexture;
}

static void d3d11_freeTexture(LG_Renderer * renderer, void * texture)
{
}

static void d3d11_swSurfaceConfigure(LG_Renderer * renderer, int width,
    int height)
{
}

static void d3d11_swSurfaceDrawFill(LG_Renderer * renderer, int x, int y,
    int width, int height, uint32_t color)
{
}

static void d3d11_swSurfaceDrawBitmap(LG_Renderer * renderer, int x, int y,
    int width, int height, int stride, uint8_t * data, bool topDown)
{
}

static void d3d11_swSurfaceShow(LG_Renderer * renderer, bool show)
{
}

const LG_RendererOps LGR_D3D11 =
{
  .getName             = d3d11_getName,
  .setup               = d3d11_setup,
  .create              = d3d11_create,
  .initialize          = d3d11_initialize,
  .deinitialize        = d3d11_deinitialize,
  .onRestart           = d3d11_onRestart,
  .onResize            = d3d11_onResize,
  .onFontUpdate        = d3d11_onFontUpdate,
  .onMouseShape        = d3d11_onMouseShape,
  .onMouseEvent        = d3d11_onMouseEvent,
  .onFrameFormat       = d3d11_onFrameFormat,
  .onFrame             = d3d11_onFrame,
  .renderStartup       = d3d11_renderStartup,
  .render              = d3d11_render,
  .capture             = d3d11_capture,
  .createTexture       = d3d11_createTexture,
  .freeTexture         = d3d11_freeTexture,
  .swSurfaceConfigure  = d3d11_swSurfaceConfigure,
  .swSurfaceDrawFill   = d3d11_swSurfaceDrawFill,
  .swSurfaceDrawBitmap = d3d11_swSurfaceDrawBitmap,
  .swSurfaceShow       = d3d11_swSurfaceShow,
};
