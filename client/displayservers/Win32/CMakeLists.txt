cmake_minimum_required(VERSION 3.10)
project(displayserver_Win32 LANGUAGES C)

add_library(displayserver_Win32 STATIC
  win32.c
  clipboard.c
  clipboard_core.c
  clipboard_format.c
  clipboard_win32.c
  input_event.c
  keymap.c
  vblank.c
  vblank_source.c
)

# per monitor DPI awareness v2 and the DPI aware window APIs need Windows 10
target_compile_definitions(displayserver_Win32
  PRIVATE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

target_link_libraries(displayserver_Win32
  lg_common
  lg_resources
  user32
  gdi32
  dwmapi
  opengl32
)

target_include_directories(displayserver_Win32
  PRIVATE
    .
)
