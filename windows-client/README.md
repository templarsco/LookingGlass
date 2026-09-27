# Looking Glass Windows Client

This is Limiar's native Looking Glass viewer for a physical Windows PC. The
plan and acceptance gates are in [LIMIAR-WINDOWS.md](../LIMIAR-WINDOWS.md).

The directory currently covers step 1 of that plan: LGProtocol, LGMP and
the `common` library built for the Windows client, with a bounded loopback
test. There is no window, input handling, local endpoint or guest
integration yet.

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

## Building

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

- No frame reaches a window. The Win32 display server is step 2.
- No second process or VM is involved. The local endpoint is step 3 and
  the guest transport is step 5.
- Run times are not performance results.
