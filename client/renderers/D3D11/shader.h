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

#ifndef _H_LG_D3D11_SHADER_
#define _H_LG_D3D11_SHADER_

#include <stdbool.h>
#include <d3d11.h>

/* Shaders are HLSL that is compiled when the renderer starts, with the
 * compiler that Windows has (d3dcompiler_47.dll, which every Windows 10 has).
 * That keeps a compiler and its output out of the build, and the source of a
 * shader is what is read in a review. The compiler is loaded for as long as the
 * compiler object is open, and not before. */

typedef struct D3D11ShaderCompiler D3D11ShaderCompiler;

/* returns NULL, having said why, if there is no compiler to use */
D3D11ShaderCompiler * d3d11Shader_open(void);
void d3d11Shader_close(D3D11ShaderCompiler * compiler);

/* Compile and make a shader of a source, from the function that is named. The
 * name is only for a message. A vertex shader's bytecode is given back (to be
 * released with ID3D10Blob_Release) if bytecode is not NULL, as an input layout
 * is made from it. */
bool d3d11Shader_vertex(D3D11ShaderCompiler * compiler, ID3D11Device * device,
    const char * name, const char * source, const char * function,
    ID3D11VertexShader ** shader, ID3D10Blob ** bytecode);

bool d3d11Shader_pixel(D3D11ShaderCompiler * compiler, ID3D11Device * device,
    const char * name, const char * source, const char * function,
    ID3D11PixelShader ** shader);

#endif
