cmake_minimum_required(VERSION 3.10)
project(renderer_D3D11 LANGUAGES C)

add_library(renderer_D3D11 STATIC
  d3d11.c
  shader.c
)

# d3d11.dll and d3dcompiler_47.dll are loaded when the renderer is made and
# started, and not linked in, so that a Windows that has neither only loses this
# renderer. dxguid has the interface identifiers and nothing to load.
target_link_libraries(renderer_D3D11
  dxguid
  lg_common
)

target_compile_definitions(renderer_D3D11
  PRIVATE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
