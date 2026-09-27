cmake_minimum_required(VERSION 3.10)
project(renderer_Opengl LANGUAGES C CXX)

if(WIN32)
  set(RENDERER_OPENGL_LIBS opengl32)
else()
  find_package(PkgConfig)
  pkg_check_modules(RENDERER_OPENGL REQUIRED IMPORTED_TARGET
    gl
  )
  set(RENDERER_OPENGL_LIBS PkgConfig::RENDERER_OPENGL)
endif()

add_library(renderer_OpenGL STATIC
  opengl.c
)

target_compile_definitions(renderer_OpenGL PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1)

target_link_libraries(renderer_OpenGL
  ${RENDERER_OPENGL_LIBS}
  lg_common

  lg_gui
)

target_include_directories(renderer_OpenGL
  PRIVATE
    src
)
