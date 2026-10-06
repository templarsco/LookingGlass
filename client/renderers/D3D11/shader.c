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

#define COBJMACROS
#define CINTERFACE

#include "shader.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <stdlib.h>
#include <string.h>

#include "common/debug.h"

struct D3D11ShaderCompiler
{
  HMODULE     library;
  pD3DCompile compile;
};

D3D11ShaderCompiler * d3d11Shader_open(void)
{
  D3D11ShaderCompiler * compiler = calloc(1, sizeof(*compiler));
  if (!compiler)
  {
    DEBUG_ERROR("Out of memory");
    return NULL;
  }

  // from System32 only, as a compiler that is found elsewhere is not Windows'
  compiler->library = LoadLibraryExW(L"d3dcompiler_47.dll", NULL,
      LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (compiler->library)
    compiler->compile = (pD3DCompile)(void *)GetProcAddress(compiler->library,
        "D3DCompile");

  if (!compiler->compile)
  {
    DEBUG_ERROR("The shader compiler, d3dcompiler_47.dll, is not available");
    d3d11Shader_close(compiler);
    return NULL;
  }
  return compiler;
}

void d3d11Shader_close(D3D11ShaderCompiler * compiler)
{
  if (!compiler)
    return;

  if (compiler->library)
    FreeLibrary(compiler->library);
  free(compiler);
}

/* Shader model 4.0 is what feature level 10.0 has, and what every level above
 * it runs. */
static ID3D10Blob * build(D3D11ShaderCompiler * compiler, const char * name,
    const char * source, const char * function, const char * target)
{
  ID3D10Blob * code   = NULL;
  ID3D10Blob * errors = NULL;

  const HRESULT hr = compiler->compile(source, strlen(source), name, NULL, NULL,
      function, target, D3DCOMPILE_ENABLE_STRICTNESS |
      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Could not compile %s %s (0x%08lx)%s%.*s", name, function,
        (unsigned long)hr, errors ? ": " : "",
        errors ? (int)ID3D10Blob_GetBufferSize(errors) : 0,
        errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "");
    code = NULL;
  }

  if (errors)
    ID3D10Blob_Release(errors);
  return code;
}

bool d3d11Shader_vertex(D3D11ShaderCompiler * compiler, ID3D11Device * device,
    const char * name, const char * source, const char * function,
    ID3D11VertexShader ** shader, ID3D10Blob ** bytecode)
{
  ID3D10Blob * code = build(compiler, name, source, function, "vs_4_0");
  if (!code)
    return false;

  const HRESULT hr = ID3D11Device_CreateVertexShader(device,
      ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL,
      shader);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Could not make the vertex shader %s (0x%08lx)", name,
        (unsigned long)hr);
    ID3D10Blob_Release(code);
    return false;
  }

  if (bytecode)
    *bytecode = code;
  else
    ID3D10Blob_Release(code);
  return true;
}

bool d3d11Shader_pixel(D3D11ShaderCompiler * compiler, ID3D11Device * device,
    const char * name, const char * source, const char * function,
    ID3D11PixelShader ** shader)
{
  ID3D10Blob * code = build(compiler, name, source, function, "ps_4_0");
  if (!code)
    return false;

  const HRESULT hr = ID3D11Device_CreatePixelShader(device,
      ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL,
      shader);
  ID3D10Blob_Release(code);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Could not make the pixel shader %s (0x%08lx)", name,
        (unsigned long)hr);
    return false;
  }
  return true;
}
