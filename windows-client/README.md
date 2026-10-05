# Looking Glass Windows Client

This is Limiar's native Looking Glass viewer for a physical Windows PC. The
plan and acceptance gates are in [LIMIAR-WINDOWS.md](../LIMIAR-WINDOWS.md).

Two steps of that plan exist so far:

- Step 1: LGProtocol, LGMP and the `common` library build for the Windows
  client, with the bounded loopback test in this directory.
- Step 2: the Looking Glass client in [client](../client) builds for
  Windows with a Win32 display server. It shows frames in a native window
  and handles per-monitor DPI, resizing and fullscreen, focus, pointer
  capture with raw relative mouse input, a keyboard grab while captured,
  keyboard state and reconnects.

The client cannot show a guest yet. On Windows it only has the `test`
transport, which generates frames inside the client. The local endpoint is
step 3 and the guest transport is step 5.

## Win32 Client

### Building

Cross-compile on Linux with MinGW-w64, after
`git submodule update --init --recursive`:

```sh
mkdir client/build-windows
cd client/build-windows
cmake -DCMAKE_TOOLCHAIN_FILE=../../host/toolchain-mingw64.cmake \
  -DOPTIMIZE_FOR_NATIVE=OFF ..
make
```

Build natively on Windows in an MSYS2 MINGW64 shell with the `make`,
`mingw-w64-x86_64-cmake` and `mingw-w64-x86_64-gcc` packages:

```sh
mkdir client/build
cd client/build
cmake -G "MSYS Makefiles" -DOPTIMIZE_FOR_NATIVE=OFF ..
make
```

MSYS2 is needed rather than a plain MinGW-w64 install because the build
embeds resources with `cp` and `dd`.

The MinGW runtime is linked statically, so `looking-glass-client.exe` needs
no extra DLLs. It renders with the OpenGL renderer through WGL. EGL, audio,
clipboard file transfer, evdev input, LGMP and SPICE are left out of the
Windows build.

### Running

`looking-glass-client.exe` starts on the `test` transport and shows an
animated test pattern. Some useful options:

- `test:input=yes` accepts the input the client would send to a guest and
  logs every event, which shows what the keyboard and mouse handling does.
- `test:frameCount=300 test:reconnect=yes` disconnects after 300 frames, so
  the client goes through its reconnect path.
- The escape key, Scroll Lock by default (`input:escapeKey`), captures and
  releases the pointer as on Linux. While the pointer is captured, the
  client also takes keys such as Alt+Tab and the Windows key, unless
  `input:grabKeyboard=no`.

Options can also go in `%APPDATA%\looking-glass\client.ini`.

### Tests

- The Win32 input layer and keymap have unit tests in
  [client/tests](../client/tests), which run on Linux next to the X11 and
  Wayland ones (`ctest -R display-input-win32`).
- [client_smoke_test.py](client_smoke_test.py) runs a client built with
  `-DENABLE_TESTS=ON` on test frame 4 in a borderless window, reads back
  what the client composed in that window and compares every pixel with
  the generated frame. CI runs it on Windows with Mesa's software OpenGL:

  ```sh
  python windows-client/client_smoke_test.py client/build/looking-glass-client.exe
  ```

  Under Wine, add `--runner wine64`. The Wine prefix needs a monospace
  font, passed after `--`, such as `-- "win:uiFont=DejaVu Sans Mono"`.
- [client_format_test.py](client_format_test.py) makes the same check for a
  matrix of formats, sizes, strides and upload paths: packed and padded
  strides, odd sizes, the 24-bit formats, a 1080p frame, and sequences that
  cycle the renderer's upload buffers, each with `opengl:amdPinnedMem` on
  and off. Every case runs with its own empty configuration directory, so
  the user's `client.ini` cannot change what is tested. HDR formats are not
  covered, as the OpenGL renderer has no HDR-to-SDR path.

  ```sh
  python windows-client/client_format_test.py client/build/looking-glass-client.exe
  ```

  CI runs it with Mesa's software OpenGL, which has no
  `GL_AMD_pinned_memory`, so the pinned cases only exercise the ordinary
  upload path there, and the test says so. That is not enough, because the
  pinned path once failed only on hardware: on an RX 9070 XT (OpenGL 4.6
  Compatibility Profile Context 26.9.1.260826), the renderer's
  `glBufferSubData` into the buffer that the driver pins left it black,
  without a GL error, so the client showed black frames by default. The
  renderer now writes to the pinned memory directly, and a PC with an AMD
  GPU should run the test with `--require-pinned`, which fails a case that
  asked for the pinned path and did not get it:

  ```sh
  python windows-client/client_format_test.py --require-pinned \
    client/build/looking-glass-client.exe
  ```

Focus changes, pointer capture, the keyboard grab, DPI scaling, fullscreen
and reconnects were checked by hand under Wine with Xvfb, not by an
automated test. Wine on Xvfb injects Scroll Lock state changes, so pick
another escape key there, such as `input:escapeKey=KEY_RIGHTCTRL`.

### Releases

[windows-client-release](../.github/workflows/windows-client-release.yml)
publishes the client as a GitHub pre-release. Run it from the Actions tab
with a new tag such as `limiar-windows-v0.1.0`, or push a tag with that
prefix. It cross-compiles the client, packages it with
[package.py](package.py), runs the smoke test on the packaged executable on
a Windows runner, and only then publishes:

- `looking-glass-client-<version>-windows-x64.zip`: the executable, the
  README from [release/README.md](release/README.md), the GPL and the
  licenses of the code built into the executable.
- `looking-glass-<version>-source.tar.gz`: the complete source of the
  build, including every submodule.
- `SHA256SUMS`.

[release/README.md](release/README.md) is also the release notes, so update
it when what works on Windows changes.

## Loopback Test

`lg-windows-client-loopback` runs both sides of the frame transport in one
process:

- A producer thread acts as the guest capture host. It publishes a KVMFR
  session, posts one frame on the LGMP frame queue and then streams the
  pixels through the framebuffer write pointer, using the frame layout and
  post-then-write order of `host/src/app.c`.
- The main thread acts as the viewer. It validates the session with the
  same checks as the client LGMP transport, subscribes to the frame queue,
  reads the frame through `common/framebuffer` and compares every pixel.

The frame is 1280x720 BGRA with a padded stride of 1296 pixels, so a
pitch/width mix-up fails. The test also fails if the host sees the frame
released only because it dropped a stalled viewer. Its waits are bounded,
and ctest stops it after 60 seconds.

The shared region is an unnamed mapping private to the test process. It is
not the local endpoint planned for step 3 and exposes nothing to other
processes.

### Building the Loopback Test

Only the LGMP and LGProtocol submodules are needed:

```sh
git submodule update --init repos/LGMP repos/LGProtocol
```

Cross-compile on Linux with MinGW-w64:

```sh
mkdir windows-client/build
cd windows-client/build
cmake -DCMAKE_TOOLCHAIN_FILE=../../host/toolchain-mingw64.cmake ..
make
```

Build and test natively on Windows with MinGW-w64:

```powershell
mkdir windows-client\build
cd windows-client\build
cmake -G "MinGW Makefiles" ..
mingw32-make
ctest --output-on-failure
```

The test also builds and runs on Linux (`cmake ..`, `make`, `ctest`), which
keeps the shared code covered there.

## Not Covered Yet

- No guest frame reaches the window. Windows has no transport besides
  `test`; the local endpoint is step 3 and the guest transport is step 5.
- Resizing by dragging the window border has no automated test. The
  fullscreen toggle goes through the same resize path.
- There is no audio, clipboard or SPICE support on Windows.
- The client has run on Windows only in CI, with software OpenGL, and
  under Wine. No run on a physical Windows PC with a GPU driver is
  recorded yet.
- Run times are not performance results.
