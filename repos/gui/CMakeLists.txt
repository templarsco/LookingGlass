cmake_minimum_required(VERSION 3.10)
project(lg_gui LANGUAGES CXX)

add_library(lg_gui STATIC
  cimgui/cimgui.cpp
  cimgui/imgui/imgui.cpp
  cimgui/imgui/imgui_draw.cpp
  cimgui/imgui/imgui_tables.cpp
  cimgui/imgui/imgui_widgets.cpp
  cimplot/cimplot.cpp
  cimplot/implot/implot.cpp
  cimplot/implot/implot_items.cpp
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lg_gui PRIVATE "-fno-fast-math")

  set_source_files_properties(cimgui/cimgui.cpp PROPERTIES
    COMPILE_FLAGS "-Wno-error=unused-function"
  )
  set_source_files_properties(cimplot/cimplot.cpp PROPERTIES
    COMPILE_FLAGS
      "-Wno-error=array-bounds -Wno-error=unused-function"
  )
endif()

target_compile_definitions(lg_gui PUBLIC
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1
  IMGUI_USE_WCHAR32
  IMGUI_USER_CONFIG="../cimconfig.h"
)

if(ENABLE_OPENGL)
  target_sources(lg_gui PRIVATE
    cimgui/imgui/backends/imgui_impl_opengl2.cpp
  )
  target_compile_definitions(lg_gui PUBLIC CIMGUI_USE_OPENGL2=1)

  # the backend calls OpenGL itself, and a library that is listed before the one
  # that needs it is not searched again by the linker
  if(WIN32)
    target_link_libraries(lg_gui PUBLIC opengl32)
  endif()
endif()

if(ENABLE_D3D11)
  set(IMGUI_DX11_BACKEND
    cimgui/imgui/backends/imgui_impl_dx11.cpp
  )
  target_sources(lg_gui PRIVATE ${IMGUI_DX11_BACKEND})

  # The backend compiles its shaders with D3DCompile, which would make the
  # client need d3dcompiler_47.dll to start at all. Its call goes to a function
  # of the renderer's instead, which loads that DLL when it is first used.
  set_source_files_properties(${IMGUI_DX11_BACKEND} PROPERTIES
    COMPILE_DEFINITIONS D3DCompile=lgD3DCompile
  )
endif()

if(ENABLE_EGL)
  set(IMGUI_OPENGL3_BACKEND
    cimgui/imgui/backends/imgui_impl_opengl3.cpp
  )
  target_sources(lg_gui PRIVATE ${IMGUI_OPENGL3_BACKEND})
  set_source_files_properties(${IMGUI_OPENGL3_BACKEND} PROPERTIES
    COMPILE_DEFINITIONS IMGUI_IMPL_OPENGL_ES3
  )
  target_compile_definitions(lg_gui PUBLIC CIMGUI_USE_OPENGL3=1)
endif()

target_compile_definitions(lg_gui PUBLIC IMGUI_IMPL_API=extern\t"C"\t)
if(WIN32)
  # lg_gui is linked statically, so the executable must not export its API
  target_compile_definitions(lg_gui PUBLIC CIMGUI_NO_EXPORT)

  # the PE linker resolves the demo window calls in the cimgui wrappers even
  # though nothing uses them, so link the empty versions of those windows
  set(LG_GUI_DEMO_SOURCES
    cimgui/imgui/imgui_demo.cpp
    cimplot/implot/implot_demo.cpp
  )
  target_sources(lg_gui PRIVATE ${LG_GUI_DEMO_SOURCES})
  set_source_files_properties(${LG_GUI_DEMO_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS IMGUI_DISABLE_DEMO_WINDOWS
  )
endif()

target_include_directories(lg_gui PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/cimgui"
  "${CMAKE_CURRENT_SOURCE_DIR}/cimgui/imgui"
  "${CMAKE_CURRENT_SOURCE_DIR}/cimplot"
  "${CMAKE_CURRENT_SOURCE_DIR}/cimplot/implot"
)

set_target_properties(lg_gui PROPERTIES
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
)
