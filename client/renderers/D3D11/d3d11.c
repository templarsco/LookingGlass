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
 * Windows that does not have it only loses this renderer.
 *
 * A frame is copied, a row at a time, from the shared memory it was received in
 * into a dynamic texture, which one triangle then draws into the part of the
 * window that the core says the guest's screen goes in. The frame is read when
 * it is drawn, as the OpenGL renderer does, and not when it arrives. */

#define COBJMACROS
#define CINTERFACE

#include "interface/renderer.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/debug.h"
#include "common/framebuffer.h"
#include "common/locking.h"
#include "common/option.h"
#include "common/stringlist.h"
#include "common/time.h"
#include "common/util.h"

#include "shader.h"

#define BUFFER_COUNT 2

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

/* what the core holds on to for a frame until it can no longer be read */
struct FrameRelease
{
  LG_FrameReleaseFn fn;
  void            * opaque;
  uint64_t          handle;
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
  UINT                  maxTextureSize;

  HWND                     window;
  IDXGISwapChain1        * swapChain;
  ID3D11RenderTargetView * backBufferView;
  UINT                     backWidth, backHeight;

  // what draws a frame
  ID3D11VertexShader    * frameVS;
  ID3D11PixelShader     * framePS;
  ID3D11SamplerState    * pointSampler;
  ID3D11SamplerState    * linearSampler;
  ID3D11RasterizerState * rasterizer;

  // where the guest's screen goes in the window, in pixels
  LG_RendererRect destRect;

  // the format of the frames that arrive, from the frame thread
  LG_Lock           formatLock;
  LG_RendererFormat format;
  bool              reconfigure;

  /* The textures that frames are copied into, which are made for that format.
   * One is drawn while the next frame is copied into the other, which is only
   * drawn when all of it has been copied. */
  bool                       configured;
  unsigned                   texWidth, texHeight;
  size_t                     sourceBytes; // a pixel of a frame, in bytes
  bool                       expand;      // 3 bytes a pixel, 4 in the texture
  ID3D11Texture2D          * frameTexture[BUFFER_COUNT];
  ID3D11ShaderResourceView * frameView[BUFFER_COUNT];
  unsigned                   texWIndex, texRIndex;
  bool                       frameReady;  // a frame is in the texture to read

  // the frame that arrived and has not been copied yet
  LG_Lock                    frameLock;
  const KVMFRFrameBuffer   * frame;
  LG_RendererFrameToken      pendingFrameToken;
  struct FrameRelease        frameRelease;
  atomic_bool                frameUpdate;
};

/* what the frames can be, and how a texture holds them */
static bool frameFormat(KVMFRFrameType type, DXGI_FORMAT * format,
    size_t * sourceBytes, bool * expand)
{
  switch (type)
  {
    case FRAME_TYPE_BGRA:
      *format = DXGI_FORMAT_B8G8R8A8_UNORM;     *sourceBytes = 4; break;
    case FRAME_TYPE_RGBA:
      *format = DXGI_FORMAT_R8G8B8A8_UNORM;     *sourceBytes = 4; break;
    case FRAME_TYPE_RGBA10:
      *format = DXGI_FORMAT_R10G10B10A2_UNORM;  *sourceBytes = 4; break;
    case FRAME_TYPE_RGBA16F:
      *format = DXGI_FORMAT_R16G16B16A16_FLOAT; *sourceBytes = 8; break;

    // Direct3D has no 3 byte format, so these are given a fourth byte
    case FRAME_TYPE_RGB_24:
      *format = DXGI_FORMAT_R8G8B8A8_UNORM;     *sourceBytes = 3; break;
    case FRAME_TYPE_BGR_32:
      *format = DXGI_FORMAT_B8G8R8A8_UNORM;     *sourceBytes = 3; break;

    default:
      return false;
  }

  *expand = *sourceBytes == 3;
  return true;
}

static struct FrameRelease takePendingFrameLocked(struct Inst * this)
{
  const struct FrameRelease release = this->frameRelease;

  this->frame             = NULL;
  this->pendingFrameToken = LG_RENDERER_FRAME_TOKEN_NONE;
  this->frameRelease      = (struct FrameRelease) {};
  atomic_store_explicit(&this->frameUpdate, false, memory_order_release);
  return release;
}

static void invokeFrameRelease(const struct FrameRelease release)
{
  if (release.fn)
    release.fn(release.opaque, release.handle);
}

static void releasePendingFrame(struct Inst * this)
{
  LG_LOCK(this->frameLock);
  const struct FrameRelease release = takePendingFrameLocked(this);
  LG_UNLOCK(this->frameLock);

  invokeFrameRelease(release);
}

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

  LG_LOCK_INIT(this->formatLock);
  LG_LOCK_INIT(this->frameLock );

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

  // what the levels that this makes a device on allow a texture to be
  this->maxTextureSize = this->level >= D3D_FEATURE_LEVEL_11_0 ? 16384 : 8192;
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

static void deconfigure(struct Inst * this)
{
  for (int i = 0; i < BUFFER_COUNT; ++i)
  {
    if (this->frameView[i])
    {
      ID3D11ShaderResourceView_Release(this->frameView[i]);
      this->frameView[i] = NULL;
    }

    if (this->frameTexture[i])
    {
      ID3D11Texture2D_Release(this->frameTexture[i]);
      this->frameTexture[i] = NULL;
    }
  }

  this->texWIndex  = 0;
  this->texRIndex  = 0;
  this->frameReady = false;
  this->configured = false;
}

static void releasePipeline(struct Inst * this)
{
  if (this->frameVS)
  {
    ID3D11VertexShader_Release(this->frameVS);
    this->frameVS = NULL;
  }

  if (this->framePS)
  {
    ID3D11PixelShader_Release(this->framePS);
    this->framePS = NULL;
  }

  if (this->pointSampler)
  {
    ID3D11SamplerState_Release(this->pointSampler);
    this->pointSampler = NULL;
  }

  if (this->linearSampler)
  {
    ID3D11SamplerState_Release(this->linearSampler);
    this->linearSampler = NULL;
  }

  if (this->rasterizer)
  {
    ID3D11RasterizerState_Release(this->rasterizer);
    this->rasterizer = NULL;
  }
}

static void d3d11_deinitialize(LG_Renderer * renderer)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  releasePendingFrame(this);
  deconfigure(this);
  releasePipeline(this);
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

/* One triangle that covers the viewport, made from the number of the vertex, so
 * that nothing is fed to the shader, and the frame on it, which the viewport has
 * put where it goes. The alpha of a frame is not shown. */
static const char frameShader[] =
  "Texture2D    frame : register(t0);\n"
  "SamplerState smp   : register(s0);\n"
  "\n"
  "struct VSOut\n"
  "{\n"
  "  float4 pos : SV_Position;\n"
  "  float2 uv  : TEXCOORD0;\n"
  "};\n"
  "\n"
  "VSOut vs(uint id : SV_VertexID)\n"
  "{\n"
  "  VSOut o;\n"
  "  o.uv  = float2((id << 1) & 2, id & 2);\n"
  "  o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
  "  return o;\n"
  "}\n"
  "\n"
  "float4 ps(VSOut i) : SV_Target\n"
  "{\n"
  "  return float4(frame.Sample(smp, i.uv).rgb, 1.0);\n"
  "}\n";

static bool makeSampler(struct Inst * this, D3D11_FILTER filter,
    ID3D11SamplerState ** sampler)
{
  const D3D11_SAMPLER_DESC desc =
  {
    .Filter         = filter,
    .AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP,
    .AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP,
    .AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP,
    .ComparisonFunc = D3D11_COMPARISON_NEVER,
    .MaxLOD         = D3D11_FLOAT32_MAX,
  };

  const HRESULT hr = ID3D11Device_CreateSamplerState(this->device, &desc,
      sampler);
  if (FAILED(hr))
    DEBUG_ERROR("Could not make a sampler (0x%08lx)", (unsigned long)hr);
  return SUCCEEDED(hr);
}

static bool makePipeline(struct Inst * this)
{
  D3D11ShaderCompiler * compiler = d3d11Shader_open();
  if (!compiler)
    return false;

  const bool compiled =
    d3d11Shader_vertex(compiler, this->device, "frame", frameShader, "vs",
        &this->frameVS, NULL) &&
    d3d11Shader_pixel (compiler, this->device, "frame", frameShader, "ps",
        &this->framePS);
  d3d11Shader_close(compiler);
  if (!compiled)
    return false;

  // pixels stay the frame's own when it is not made smaller, as with OpenGL
  if (!makeSampler(this, D3D11_FILTER_MIN_MAG_MIP_POINT , &this->pointSampler ) ||
      !makeSampler(this, D3D11_FILTER_MIN_MAG_MIP_LINEAR, &this->linearSampler))
    return false;

  const D3D11_RASTERIZER_DESC desc =
  {
    .FillMode        = D3D11_FILL_SOLID,
    .CullMode        = D3D11_CULL_NONE,
    .DepthClipEnable = TRUE,
  };

  const HRESULT hr = ID3D11Device_CreateRasterizerState(this->device, &desc,
      &this->rasterizer);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Could not make the rasterizer state (0x%08lx)",
        (unsigned long)hr);
    return false;
  }
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

  if (!makeBackBufferView(this) || !makePipeline(this))
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
  struct Inst * this = UPCAST(struct Inst, renderer);
  releasePendingFrame(this);
}

static void d3d11_onResize(LG_Renderer * renderer, const int width,
    const int height, const double scale, const LG_RendererRect destRect,
    LG_RendererRotate rotate)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  // the core's units are the window's before its scale, and these are pixels
  if (destRect.valid)
  {
    this->destRect.valid = true;
    this->destRect.x     = (int)lround(destRect.x * scale);
    this->destRect.y     = (int)lround(destRect.y * scale);
    this->destRect.w     = (int)lround(destRect.w * scale);
    this->destRect.h     = (int)lround(destRect.h * scale);
  }

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
  struct Inst * this = UPCAST(struct Inst, renderer);

  DXGI_FORMAT texFormat;
  size_t      sourceBytes;
  bool        expand;
  if (!frameFormat(format.type, &texFormat, &sourceBytes, &expand))
  {
    DEBUG_ERROR("Unsupported frame type");
    return false;
  }

  if (format.frameWidth  > this->maxTextureSize ||
      format.frameHeight > this->maxTextureSize)
  {
    DEBUG_ERROR("A frame of %ux%u is larger than Direct3D 11 feature level "
        "%u.%u makes a texture (%u)", format.frameWidth, format.frameHeight,
        ((unsigned)this->level >> 12) & 0xF, ((unsigned)this->level >> 8) & 0xF,
        this->maxTextureSize);
    return false;
  }

  LG_LOCK(this->formatLock);
  this->format = format;

  // The packed rows of this type are the width of the frame, whatever the
  // surface that carried them was, as with OpenGL.
  if (format.type == FRAME_TYPE_BGR_32)
    this->format.dataWidth = format.frameWidth;

  this->reconfigure = true;
  LG_UNLOCK(this->formatLock);
  return true;
}

static bool d3d11_onFrame(LG_Renderer * renderer,
    const KVMFRFrameBuffer * frame, int dmaFD,
    const KVMFRFrameDamageRect * damage, int damageCount,
    LG_RendererFrameToken frameToken, LG_FrameReleaseFn releaseFn,
    void * releaseOpaque, uint64_t releaseHandle)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  // the frame is read when it is drawn, and from shared memory only
  if (!frame || dmaFD >= 0)
    return false;

  LG_LOCK(this->frameLock);
  const struct FrameRelease oldRelease = takePendingFrameLocked(this);
  this->frame             = frame;
  this->pendingFrameToken = frameToken;
  this->frameRelease      = (struct FrameRelease)
  {
    .fn     = releaseFn,
    .opaque = releaseOpaque,
    .handle = releaseHandle,
  };
  atomic_store_explicit(&this->frameUpdate, true, memory_order_release);
  LG_UNLOCK(this->frameLock);

  // a frame that was not drawn before this one came is let go of
  invokeFrameRelease(oldRelease);
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

/* Make the textures for the frames that the core said it will send. They are
 * dynamic, so that a frame is copied straight into the memory that the GPU
 * reads it from and a frame still being drawn does not hold up the next. */
static bool configure(struct Inst * this)
{
  LG_LOCK(this->formatLock);
  if (!this->reconfigure)
  {
    LG_UNLOCK(this->formatLock);
    return true;
  }

  deconfigure(this);

  bool ok = false;
  DXGI_FORMAT texFormat;
  if (!frameFormat(this->format.type, &texFormat, &this->sourceBytes,
        &this->expand))
    goto out;

  const D3D11_TEXTURE2D_DESC desc =
  {
    .Width          = this->format.frameWidth,
    .Height         = this->format.frameHeight,
    .MipLevels      = 1,
    .ArraySize      = 1,
    .Format         = texFormat,
    .SampleDesc     = { .Count = 1 },
    .Usage          = D3D11_USAGE_DYNAMIC,
    .BindFlags      = D3D11_BIND_SHADER_RESOURCE,
    .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
  };

  HRESULT hr = S_OK;
  for (int i = 0; SUCCEEDED(hr) && i < BUFFER_COUNT; ++i)
  {
    hr = ID3D11Device_CreateTexture2D(this->device, &desc, NULL,
        &this->frameTexture[i]);
    if (SUCCEEDED(hr))
      hr = ID3D11Device_CreateShaderResourceView(this->device,
          (ID3D11Resource *)this->frameTexture[i], NULL, &this->frameView[i]);
  }
  if (FAILED(hr))
  {
    DEBUG_ERROR("Could not make a texture for %ux%u frames (0x%08lx)",
        this->format.frameWidth, this->format.frameHeight, (unsigned long)hr);
    deconfigure(this);
    goto out;
  }

  this->texWidth    = this->format.frameWidth;
  this->texHeight   = this->format.frameHeight;
  this->configured  = true;
  this->reconfigure = false;
  ok = true;

out:
  LG_UNLOCK(this->formatLock);
  return ok;
}

/* Where a frame is copied to, a row at a time. */
struct Upload
{
  uint8_t * dst;
  size_t    rowPitch;
  size_t    rowBytes;    // the bytes of a row that are in the texture
  size_t    width;       // the pixels of a row
  size_t    sourceBytes; // the bytes of a pixel in the frame
  bool      expand;
  size_t    row;
  size_t    rows;
  bool      failed;      // a row was not what the core said, as opposed to late
};

static bool uploadRow(void * opaque, const void * src, size_t size)
{
  struct Upload * upload = opaque;

  // the frame's rows are what the core said, which is checked before they are
  if (upload->row >= upload->rows || size != upload->width * upload->sourceBytes)
  {
    upload->failed = true;
    return false;
  }

  uint8_t * dst = upload->dst + upload->row++ * upload->rowPitch;
  if (!upload->expand)
    memcpy(dst, src, size);
  else
  {
    const uint8_t * source = src;
    for (size_t x = 0; x < upload->width; ++x, source += 3, dst += 4)
    {
      dst[0] = source[0];
      dst[1] = source[1];
      dst[2] = source[2];
      dst[3] = 0xFF;
    }
  }
  return true;
}

enum Upload_Result
{
  UPLOAD_NONE,   // there was no frame to copy
  UPLOAD_DONE,   // a frame is in the texture
  UPLOAD_RETRY,  // the guest is still writing it, so it will be copied later
  UPLOAD_FAILED
};

static enum Upload_Result uploadFrame(struct Inst * this,
    LG_RendererFrameToken frameTokenLimit, LG_RendererFrameToken * consumed)
{
  *consumed = LG_RENDERER_FRAME_TOKEN_NONE;

  LG_LOCK(this->frameLock);
  if (!atomic_load_explicit(&this->frameUpdate, memory_order_acquire) ||
      this->pendingFrameToken > frameTokenLimit)
  {
    LG_UNLOCK(this->frameLock);
    return UPLOAD_NONE;
  }

  // a frame is not copied into a texture that is not made for its format, which
  // is left until the next render has made one
  LG_LOCK(this->formatLock);
  if (this->reconfigure || !this->configured)
  {
    LG_UNLOCK(this->formatLock);
    LG_UNLOCK(this->frameLock);
    return UPLOAD_NONE;
  }

  // The frame is the render thread's now, and stays the guest's to hand back
  // until it has been read, which the release below does, so neither lock is
  // held while it is.
  const KVMFRFrameBuffer      * frame             = this->frame;
  const LG_RendererFrameToken   pendingFrameToken = this->pendingFrameToken;
  const struct FrameRelease     release           = takePendingFrameLocked(this);

  const size_t pitch  = this->format.pitch;
  const size_t width  = this->texWidth;
  const size_t height = this->texHeight;
  const size_t rows   = this->format.dataHeight < height ?
    this->format.dataHeight : height;
  const size_t rowBytes = width * this->sourceBytes;
  ID3D11Texture2D * texture = this->frameTexture[this->texWIndex];

  struct Upload upload =
  {
    .width       = width,
    .sourceBytes = this->sourceBytes,
    .expand      = this->expand,
    .rows        = rows,
    .rowBytes    = width * (this->expand ? 4 : this->sourceBytes),
  };
  LG_UNLOCK(this->formatLock);
  LG_UNLOCK(this->frameLock);

  // the core checks the layout of a frame before it lets one through, and what
  // is checked here is what a read of the frame needs to be able to end
  if (!rows || !pitch || rowBytes > pitch || rows > UINT32_MAX / pitch)
  {
    invokeFrameRelease(release);
    DEBUG_ERROR("The frame has no rows that can be read");
    return UPLOAD_FAILED;
  }

  D3D11_MAPPED_SUBRESOURCE mapped;
  const HRESULT hr = ID3D11DeviceContext_Map(this->context,
      (ID3D11Resource *)texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (FAILED(hr) || mapped.RowPitch < upload.rowBytes)
  {
    if (SUCCEEDED(hr))
      ID3D11DeviceContext_Unmap(this->context, (ID3D11Resource *)texture, 0);

    invokeFrameRelease(release);
    DEBUG_ERROR("Could not map the texture for a frame (0x%08lx)",
        (unsigned long)hr);
    return UPLOAD_FAILED;
  }

  upload.dst      = mapped.pData;
  upload.rowPitch = mapped.RowPitch;

  /* A row is copied as soon as the guest has written it, as the guest may
   * still be writing the rest of a frame that is already here. A frame that
   * the guest is too slow with is only half copied: this texture is not the
   * one that is drawn, so what is shown stays whole, and the frame is tried
   * again at the next render. */
  const bool read = framebuffer_read_fn(frame, rows, width, this->sourceBytes,
      pitch, uploadRow, &upload) && upload.row == rows;

  // a frame that is cut short has nothing below its last row, and a texture
  // that was discarded has whatever a driver left in it
  for (size_t y = rows; read && y < height; ++y)
    memset(upload.dst + y * upload.rowPitch, 0, upload.rowBytes);

  ID3D11DeviceContext_Unmap(this->context, (ID3D11Resource *)texture, 0);

  if (read)
  {
    // this texture is the one that is drawn now, and the other is next
    this->texRIndex  = this->texWIndex;
    this->texWIndex  = (this->texWIndex + 1) % BUFFER_COUNT;
    this->frameReady = true;
    *consumed        = pendingFrameToken;
    invokeFrameRelease(release);
    return UPLOAD_DONE;
  }

  if (upload.failed)
  {
    invokeFrameRelease(release);
    DEBUG_ERROR("Could not read the frame");
    return UPLOAD_FAILED;
  }

  // not all there yet: keep it for the next render, unless a newer one has come
  LG_LOCK(this->frameLock);
  const bool newer = atomic_load_explicit(&this->frameUpdate,
      memory_order_acquire);
  if (!newer)
  {
    this->frame             = frame;
    this->pendingFrameToken = pendingFrameToken;
    this->frameRelease      = release;
    atomic_store_explicit(&this->frameUpdate, true, memory_order_release);
  }
  LG_UNLOCK(this->frameLock);

  if (newer)
    invokeFrameRelease(release);

  DEBUG_WARN("Retrying an incomplete frame");
  return UPLOAD_RETRY;
}

/* The frame goes where the core says the guest's screen is in the window, with
 * the viewport, so there is nothing to compute for it. */
static void drawFrame(struct Inst * this)
{
  if (!this->frameReady || !this->destRect.valid || this->destRect.w <= 0 ||
      this->destRect.h <= 0)
    return;

  const D3D11_VIEWPORT viewport =
  {
    .TopLeftX = (float)this->destRect.x,
    .TopLeftY = (float)this->destRect.y,
    .Width    = (float)this->destRect.w,
    .Height   = (float)this->destRect.h,
    .MaxDepth = 1.0f,
  };

  // pixels stay as they are unless the frame is made smaller to fit
  ID3D11SamplerState * sampler =
    (unsigned)this->destRect.w < this->texWidth ||
    (unsigned)this->destRect.h < this->texHeight ?
      this->linearSampler : this->pointSampler;

  ID3D11DeviceContext * context = this->context;
  ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
  ID3D11DeviceContext_RSSetState(context, this->rasterizer);
  ID3D11DeviceContext_OMSetBlendState(context, NULL, NULL, 0xFFFFFFFF);
  ID3D11DeviceContext_IASetInputLayout(context, NULL);
  ID3D11DeviceContext_IASetPrimitiveTopology(context,
      D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11DeviceContext_VSSetShader(context, this->frameVS, NULL, 0);
  ID3D11DeviceContext_PSSetShader(context, this->framePS, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(context, 0, 1,
      &this->frameView[this->texRIndex]);
  ID3D11DeviceContext_PSSetSamplers(context, 0, 1, &sampler);
  ID3D11DeviceContext_Draw(context, 3, 0);

  // the next frame is copied into this texture, which must not be in use as one
  ID3D11ShaderResourceView * none = NULL;
  ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &none);
}

static bool d3d11_render(LG_Renderer * renderer, LG_RendererRotate rotate,
    LG_RendererFrameToken frameTokenLimit, const bool invalidateWindow,
    void (*preSwap)(void * udata), void * udata,
    LG_RendererFrameTiming * timing)
{
  struct Inst * this = UPCAST(struct Inst, renderer);
  *timing = (LG_RendererFrameTiming) {};

  if (!this->backBufferView)
  {
    DEBUG_ERROR("There is no back buffer to draw in");
    return false;
  }

  if (!configure(this) ||
      uploadFrame(this, frameTokenLimit, &timing->frameToken) == UPLOAD_FAILED)
    return false;

  // behind the frame, where the window is larger than the screen it shows
  static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
  ID3D11DeviceContext_OMSetRenderTargets(this->context, 1,
      &this->backBufferView, NULL);
  ID3D11DeviceContext_ClearRenderTargetView(this->context,
      this->backBufferView, black);

  drawFrame(this);
  preSwap(udata);

  const uint64_t swapStart = nanotime();
  const HRESULT hr = IDXGISwapChain1_Present(this->swapChain,
      this->vsync ? 1 : 0, 0);
  timing->swapTime = nanotime() - swapStart;

  // for the display server's measure of how soon after a blank this was
  app_frameSubmitted();

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
  // what the frame was, as with OpenGL, which draws its values as they are
  LG_LOCK(this->formatLock);
  capture->hdr       = this->format.hdr;
  capture->hdrPQ     = this->format.hdrPQ;
  LG_UNLOCK(this->formatLock);

  capture->format    = LG_CAPTURE_RGBA8;
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
