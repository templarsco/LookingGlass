# Looking Glass Windows Client

This is Limiar's native Looking Glass viewer for a physical Windows PC. The
plan and acceptance gates are in [LIMIAR-WINDOWS.md](../LIMIAR-WINDOWS.md).

Three steps of that plan exist so far:

- Step 1: LGProtocol, LGMP and the `common` library build for the Windows
  client, with the bounded loopback test in this directory.
- Step 2: the Looking Glass client in [client](../client) builds for
  Windows with a Win32 display server. It shows frames in a native window
  and handles per-monitor DPI, resizing and fullscreen, focus, pointer
  capture with raw relative mouse input, a keyboard grab while captured,
  keyboard state and reconnects.
- Step 3: the client reads frames over LGMP from a named shared memory
  section on the PC, the Windows counterpart of the KVMFR device on Linux.
  It refuses a section that accounts other than the user's can open.

The client cannot show a guest yet, because nothing maps the section into a
virtual machine yet. That is step 5, IVSHMEM on Hyper-V. Until then the
client starts on the `test` transport, which generates frames inside the
client, and the LGMP path is exercised with the test producer in this
directory. The [HCS probe](#hcs-shared-memory-probe) checks on a Hyper-V PC
whether a VM can get the section at all.

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
clipboard file transfer, evdev input and SPICE are left out of the Windows
build.

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

### Shared Memory

With `app:transport=lgmp`, the client opens the named section in
`lgmp:shmDevice`, `Global\looking-glass` by default, and reads LGMP from it
as it reads `/dev/kvmfr0` or `/dev/shm/looking-glass` on Linux. Whatever maps
the section into the VM creates it; the client only opens it.

The section gives whoever can open it the guest's screen and the input sent
to the guest, so the client checks its owner and DACL first. The owner and
every account that may map or modify it must be the user, the user's logon
session, SYSTEM, the Administrators group, or a Hyper-V virtual machine
account (`NT VIRTUAL MACHINE\...`). Anything else, a NULL DACL included,
makes the client refuse the section and log which account can open it.

`lg-windows-client-producer` stands in for the guest on the PC. It creates
a section owned by the user with a DACL for the user and SYSTEM, and serves
the moving test pattern on it:

```bat
lg-windows-client-producer.exe Local\looking-glass
looking-glass-client.exe app:transport=lgmp lgmp:shmDevice=Local\looking-glass
```

`Local\` names live in the user's session. `Global\` names are visible to
services such as Hyper-V's, and creating one needs administrator rights.
The producer also takes `--size=WxH`, `--fps=N` and `--frames=N`, and
refuses to start if the section already exists, as that section would keep
someone else's DACL.

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
- With `--producer lg-windows-client-producer.exe`, the same frame comes
  from the producer over a named section and the LGMP transport instead,
  with a padded stride. Adding `--world` makes the producer let every
  account open the section, and the test then passes only if the client
  refuses it. CI runs both on Windows.

  ```sh
  python windows-client/client_smoke_test.py \
    --producer windows-client/build/lg-windows-client-producer.exe \
    client/build/looking-glass-client.exe
  ```

Focus changes, pointer capture, the keyboard grab, DPI scaling, fullscreen
and reconnects were checked by hand under Wine with Xvfb, not by an
automated test. Wine on Xvfb injects Scroll Lock state changes, so pick
another escape key there, such as `input:escapeKey=KEY_RIGHTCTRL`.

### HCS Shared Memory Probe

`lg-windows-client-hcs-probe` checks, on a PC with Hyper-V, the two ways
step 5 could give a VM the shared memory. It leaves nothing on the PC but
its output folder. Each case boots a disposable Linux VM through the Host
Compute Service, as WSL does, with WSL's kernel and the probe's own init,
[hcs_probe_guest.c](src/hcs_probe_guest.c). The init maps the memory,
checks a pattern the PC wrote at the start of every page, and exchanges one
write in each direction, all over the VM's serial port:

- `hdv`: the probe emulates an IVSHMEM PCI device (1af4:1110, like QEMU's
  ivshmem-plain) through the HCS device emulation API, with BAR2 backed by
  a section. It passes only if the guest finds the section in BAR2 and no
  guest access to BAR2 reaches the emulator, which means the guest reads
  and writes the section itself. This is the route where the guest's
  IVSHMEM driver works unchanged.
- `shm`: a HCS `SharedMemory` region maps a named section into guest
  memory, and the guest maps it at the address the HCS reports. One VM has
  the region in its configuration, one has it hidden from the guest's
  memory map, and one gets it added while it runs, as a VM that something
  else created would have to.

Run it from an elevated prompt:

```bat
lg-windows-client-hcs-probe.exe
```

It writes `report.json`, the VMs' serial logs, and the copies of the kernel
and the initrd that the VMs boot to a new folder next to itself.
`--only hdv` or `--only shm` runs one case, `--kernel` boots another x86_64
kernel with Hyper-V PCI support, and `--size-mib` sets the size of the
memory, 32 MiB by default. The report also lists the compute systems the
HCS already knows, such as WSL.

`--vm ID` checks an existing VM instead, such as one of Hyper-V Manager, by
its ID (`(Get-VM NAME).Id`): whether the HCS opens it and creates a device
host for it, and whether it lets the probe add a `SharedMemory` region,
which the probe removes again. The VM keeps running, and nothing in it
checks the region.

The init is built for x86_64 Linux, with the host's GCC when
cross-compiling from Linux or with Clang and LLD on Windows; CMake skips the
probe when it finds neither. CI boots the init under QEMU with QEMU's own
ivshmem-plain device, reached the same way as the emulated one, and plays
the probe's side of the protocol
([hcs_probe_guest_test.py](hcs_probe_guest_test.py)). It opens the serial
port only after the guest has booted, which can happen on Hyper-V, where
the output written before is lost; the init repeats its greeting until the
first command for that reason. The release workflow runs the packaged probe
with the probe itself standing in for the kernel, so no guest boots and
every case must fail cleanly into a well-formed report
([hcs_probe_smoke.py](hcs_probe_smoke.py)). The build workflow runs it with
WSL's kernel on GitHub's Windows Server 2025 runners, where Hyper-V runs
nested, and runs `--vm` on a disposable Hyper-V Manager VM without a disk.
It prints the report, the guests' kernel messages about devices, the
Hyper-V events and, when a VM worker process crashes, the functions on its
stack.

On those runners, Windows 10.0.26100, on September 28, 2026:

- `shm` works. The HCS takes the section by its NT name,
  `\BaseNamedObjects\<name>`; the Win32 name, `Global\<name>`, fails when
  the VM starts (0x800700a1). It maps the section right after the VM's
  memory and reports `GuestPhysicalAddress` as a page number. The guest and
  the PC see each other's writes. With `HiddenFromGuest`, the guest does not
  find the section there. The HCS does not add a region to a running VM
  (0x80070032, `ERROR_NOT_SUPPORTED`), so the region has to be in the VM's
  configuration when the VM is created.
- `hdv` does not work. The HCS offers the device only when the VM's
  configuration declares it under `FlexibleIov` with
  `"HostingModel": "External"`. The guest's reads and writes of the
  configuration space reach the emulator, but the host handles the BAR
  registers itself, and reads of BAR0 and BAR2 return all ones without
  reaching the emulator. Backing BAR2 by the section fails before the VM
  starts (`ERROR_INVALID_STATE`). Right after the start it succeeds, and
  then the VM's worker process, `vmwp.exe`, stops with a fast fail in
  `vmvpci.dll` when the guest enables the device's memory space: with
  Microsoft's public symbols, the dump shows
  `Vpci::Core::VirtualDevice::CreateNewRangesForSection` giving up with
  `E_UNEXPECTED`. The probe's first `hdv` VM ends that way. Once the guest
  is up, backing BAR2 succeeds with no effect.
- `--vm` on a Hyper-V Manager VM: the HCS lists it, with `"Owner": "VMMS"`,
  opens it and creates a device host for it, but refuses to add the region,
  of 32 MiB or of 1 GiB (0x8004102B), and logs that the memory's virtual
  quantity, limit and reservation are below their minimums.

The probe has not run on a PC with Limiar's VM yet.

### Releases

[windows-client-release](../.github/workflows/windows-client-release.yml)
publishes the client as a GitHub pre-release. Run it from the Actions tab
with a new tag such as `limiar-windows-v0.1.0`, or push a tag with that
prefix. It cross-compiles the client, the test frame producer and the HCS
probe, packages them with [package.py](package.py), runs the smoke tests on
the packaged executables on a Windows runner, with the test transport, over
LGMP and for the probe, and only then publishes:

- `looking-glass-client-<version>-windows-x64.zip`: the client, the
  producer, the probe, the README from [release/README.md](release/README.md),
  the GPL and the licenses of the code built into the executables.
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

The shared region is an unnamed mapping private to the test process, so it
exposes nothing to other processes. The client's named section is covered
by the smoke test above.

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

- No guest frame reaches the window. Nothing maps the shared memory section
  into Limiar's VM yet; that is step 5. The HCS probe has run only on
  GitHub's runners, not on a PC with Limiar's VM.
- The LGMP path copies each frame through the CPU into an OpenGL texture.
  There is no zero-copy import on Windows; DMA-BUF is Linux only.
- Resizing by dragging the window border has no automated test. The
  fullscreen toggle goes through the same resize path.
- There is no audio, clipboard or SPICE support on Windows.
- The client has run on Windows only in CI, with software OpenGL, and
  under Wine. No run on a physical Windows PC with a GPU driver is
  recorded yet.
- Run times are not performance results.
