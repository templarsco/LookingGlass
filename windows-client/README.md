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
  It refuses a section that any account but the user, the user's logon
  session, SYSTEM, the Administrators and Hyper-V virtual machines can map
  or modify. Any VM's account is accepted, not one VM's: the creator of the
  section decides which VM gets it, and should grant that VM's account only.

The client cannot show a guest yet. That is step 5, IVSHMEM on Hyper-V,
which is under way: the [HCS probe](#hcs-shared-memory-probe) checks on a
Hyper-V PC whether a VM can get a section of the PC, and a Windows guest's
IVSHMEM driver maps it through
[lg-hyperv-ivshmem](#ivshmem-device-for-hyper-v-guests), but no Looking
Glass host has served frames that way yet. Until then the client starts on
the `test` transport, which generates frames inside the client, and the
LGMP path is exercised with the test producer in this directory.

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
memory, 32 MiB by default. The report also counts the compute systems the
HCS already knows, such as WSL, with the type, owner and state of each, and
none of their names or IDs, which identify the VMs of the PC.

`--vm ID` checks an existing VM instead, such as one of Hyper-V Manager, by
its ID (`(Get-VM NAME).Id`): whether the HCS opens it and creates a device
host for it, and whether it lets the probe add a `SharedMemory` region,
which the probe removes again. The VM keeps running, and nothing in it
checks the region.

`--windows-disk PATH` boots a disposable Windows guest instead, with a
`SharedMemory` region, from a disk that
[hcs_probe_windows_disk.ps1](hcs_probe_windows_disk.ps1) makes from a
Windows ISO. The guest runs [lg-hyperv-ivshmem](#ivshmem-device-for-hyper-v-guests)
on COM1 at every start. The probe tells it where the HCS put the region,
it installs the IVSHMEM driver on a device over the region, and the checks
read and write through the driver's mapping, as the Looking Glass host
does. The guest's `lg-hyperv-ivshmem adapters` also lists its display
adapters, and whether Direct3D 11 makes a device on each, for the report.
The guest writes to the disk. With `-Logs`, the script shows what
the guest logged on the disk. With `--client COMMAND` too, the guest then
installs the Looking Glass IDD that the script's `-Idd` put on the disk,
and the probe runs COMMAND with `{section}` replaced by the section's name
and passes if it exits with 0, such as
[client_smoke_test.py](client_smoke_test.py) `--section {section}`, which
passes once the client composes a frame that the IDD served.

`--gpu INTERFACE` gives the Windows guest a partition of the PC's GPU
(GPU-PV) too, by the Name that `Get-VMHostPartitionableGpu` shows: the
probe asks the HCS for it with a modify request on
`VirtualMachine/ComputeTopology/Gpu` as soon as the VM runs, since the HCS
refuses it before (0x80041001). The GPU counts once Direct3D 11 works in
the guest on an adapter that is not Microsoft's, which the probe asks a
few times, since the partition may show up late. The guest loads the
GPU's user-mode driver from its `HostDriverStore`, where the disk script's
`-GpuPv INTERFACE` copies the driver packages of that GPU from the PC's
DriverStore: the package of the driver's service, and the packages of its
OpenGL libraries, which may be another one. It resolves each file within
the DriverStore only, refuses links, and checks every copy's size and
SHA-256 against the PC's file. `-ListGpuPackages` only lists what it would
copy.

`--hcl TYPE` tries VMs with the HCS's paravisor setting,
`SecuritySettings.Isolation.HclEnabled`, and the shared memory, which
Limiar's VMs would need together. The VMs boot no disk, so their firmware
only waits for one, and a reset stops them. `auto` tries each isolation
type that the probe knows, `GuestStateOnly`, `VirtualizationBasedSecurity`
and none, each without a guest state file and with an empty one of each
kind the HCS makes (`HcsCreateEmptyGuestStateFile`), `BlockStorage` and
`FileMode`. A VM that the HCS starts but that stops is tried again without
the region, which tells whether the region stops it. With
`--windows-disk`, the Windows guest then boots with the first settings
that kept a VM running, and must find the region. The HCS has no
documented setting for a paravisor image of one's own, such as an OpenHCL
build: `HclEnabled` loads Windows' own. This case only reports; it does
not count for the exit code.

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
stack. Its `windows-client-guest` job makes disks from Microsoft's
evaluation ISOs of Windows Server 2025 and Windows 11 Enterprise LTSC and
runs `--windows-disk` on each, with the IVSHMEM driver that Looking Glass's
host installer bundles, and fails unless the checks pass. The Windows 11
guest then installs the IDD that the `idd` job builds and signs with the
WDK's test certificate, which the guest trusts, and the client on the
runner must compose a frame from it.

On those runners, Windows 10.0.26100, on September 28 and 29, 2026:

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
- `--windows-disk`, with Windows Server 2025's Server Core and Windows 11
  Enterprise LTSC, both 10.0.26100, and a 32 MiB region: the IVSHMEM
  driver installs unchanged on the device that `lg-hyperv-ivshmem` makes,
  starts, gets the forced ranges, and maps the shared memory write-combined
  as peer 0. Through that mapping the guest reads the PC's pattern on every
  page, and each side sees the other's write. Windows lists the region as
  reserved memory, not as RAM. The driver is attestation-signed; Windows
  Server logs a Code Integrity event about WHQL driver enforcement for it
  (3084) and loads it all the same.
- In both guests, `lg-hyperv-ivshmem find` finds the region by itself,
  where the HCS says it put it: the Loader Reserved range that starts where
  RAM ends, at 0x108000000, 32 MiB in Windows Server and 128 MiB in
  Windows 11.
- With a 128 MiB region, the Windows 11 guest then installs the Looking
  Glass IDD, which the guest trusts without test signing. The IDD opens the
  IVSHMEM device, finds no hardware render adapter and renders in software,
  and serves a 1920x1080 display over LGMP. The client on the runner opens
  the section and composes the IDD's frames in its window.
- The Windows guest's VM does not survive Windows restarting itself, which
  setup does once after the first boot: the Dynamic Memory Controller fails
  its post reset (0x8007054F), the VM fails to start after the reset, and
  the HCS reports a `ResetFailed` exit. The probe then starts a new VM with
  the same section and disk, which passes. With the guest's memory
  physically backed (`AllowOvercommit` false) instead of by the worker's
  virtual memory, the HCS does not create a VM with the region at all: the
  Dynamic Memory Controller fails to initialize (0x80070032).
- `--hcl auto`, on September 29: the HCS refuses a VM with
  `VirtualizationBasedSecurity` ("Failed to create partition: The
  parameter is incorrect"), one with no isolation type (the virtual BIOS
  fails to initialize), and one with `GuestStateOnly` but no guest state
  file, all with 0x80070057. With `GuestStateOnly` and an empty guest
  state file of either kind, it creates and starts the VM, puts the region
  at 0x108000000 as without the paravisor, and logs that it loads the IGVM
  file from the default location. A few milliseconds after the start, the
  firmware reports a fatal error (event 18610, error codes 0x1A, 0x2, 0x0,
  0x4), the virtual processor triple faults, and the VM fails to reset and
  stops. The same VM without the region keeps running: the paravisor's
  firmware sets up its first boot, finds no bootable device and waits. So
  the region and Windows' paravisor stop the VM together, while each runs
  without the other.

The probe has not run on a PC with Limiar's VM yet.

### PC Test

[hcs_probe_pc.ps1](hcs_probe_pc.ps1) runs the whole check on a PC with
Hyper-V and a GPU that Hyper-V can partition, in an elevated Windows
PowerShell. It downloads Microsoft's evaluation ISO of Windows 11
Enterprise LTSC and the IVSHMEM driver, makes a new disk with the disk
script and `-GpuPv`, and runs the probe with `--windows-disk`, `--gpu`,
`--hcl auto` and a `--client` command that shows the guest's display in a
window and saves its first frame. Then it collects the guest's logs from
the disk, the Hyper-V events about the probe's VMs only, and a summary,
and zips them with the report. It changes nothing on the PC but its work
folder: no other VM, no certificate, no boot setting and no driver of the
PC. `-Plan` says what a run would do and changes nothing, with no
elevation, `-Watch` leaves the client running until its window is closed,
`-Reuse` boots the last disk again, and `-NoGpu` runs without a GPU, while
`-Gpu` names the one to partition on a PC that has several.

The IVSHMEM driver is a download that goes into the guest, so the script shows
its SHA-256 and the ISO's, takes `-IvshmemSha256` and `-IsoSha256` to stop
unless they match, and stops unless every driver catalog is validly signed
(`-AllowUnverifiedDriver` accepts one that is not). The report lists the HCS's
compute systems as a count with each one's type, owner and state, as
[hcs_systems.c](src/hcs_systems.c) summarizes them, because it names no VM of
the PC; the script also hides the user and machine names in the text files
that it zips.

The `windows-client-pc-test-bundle` job cross-compiles the client with
test capture, the probe and `lg-hyperv-ivshmem`, and bundles them with the
IDD's package, the scripts, the README from
[release/PC-TEST.md](release/PC-TEST.md) and the licenses
([package_pc_test.py](package_pc_test.py)) as the
`windows-client-pc-test` artifact. The `windows-client-pc-test` job runs
the bundle's script with `-NoGpu` on a Windows runner, as the README says
to run it, and checks the zip it leaves.

### IVSHMEM Device for Hyper-V Guests

`lg-hyperv-ivshmem` ([hyperv_ivshmem.c](src/hyperv_ivshmem.c)) runs in a
Windows guest of Hyper-V and gives it an IVSHMEM device over the memory
that the PC shares with the VM through a HCS `SharedMemory` region, so that
the IVSHMEM driver and the Looking Glass host run unchanged, as they do
under QEMU. The IVSHMEM driver does not look at the PCI bus: it takes its
device's first memory resource, 256 bytes, as the registers of QEMU's
ivshmem-plain, and the next one as the shared memory. So the tool makes a
root-enumerated device with the IVSHMEM hardware IDs and a forced
configuration of two ranges of the region: a page that the PC keeps
zeroed, which reads as an ivshmem-plain without interrupts that is peer 0,
and the shared memory. It is linked statically, since the guest has no
MinGW runtime, and is not in the release package yet.

Run it as an administrator in the guest:

```bat
lg-hyperv-ivshmem.exe install INF
```

INF is the IVSHMEM driver's INF. The guest finds the region by itself: the
HCS puts it right after the VM's memory, and Hyper-V's firmware reports it
as reserved memory, which Windows keeps as Loader Reserved, so it is the
reserved range that starts where the highest range of RAM ends. `find`
shows that range, whose last page becomes the registers. `adapters` shows
the display adapters, as Windows' devices and as DXGI adapters, and
whether Direct3D 11 makes a device on each.
`install REGISTERS MEMORY SIZE INF` takes the ranges instead: REGISTERS is
the guest physical address of the zeroed page, and MEMORY and SIZE are the
shared memory's. `status` shows the device and its resources, `memory` the
RAM that Windows uses, which the device must stay out of, and `remove`
removes the device. The ranges must be whole pages that x64 can address and
that stay out of that RAM, and `install` refuses them otherwise, however the
numbers are written: a size that is a negative number, or that runs past the
top of the address space, is refused and not taken for a range that ends
at a small address. `check REGISTERS MEMORY SIZE` says whether `install`
would take them and changes nothing, and `selftest` checks what `install`
takes and refuses on made-up RAM, without a device or an administrator,
which CI runs. The device stays across restarts with the ranges it was
given, and Windows starts it before anything here runs again. The HCS puts
the region after the VM's memory, so if the VM's memory changes in size, the
ranges are stale until `install` runs again, and what Windows does with a
device on ranges that are no longer the region, or that are RAM now, was not
tried. Give the VM the same memory for as long as its guest keeps the device,
or `remove` the device first.

The probe sends the address that the HCS reports in the VM's
`SharedMemoryRegion` property over the serial port, which
`lg-hyperv-ivshmem serial COM1 INF` answers, and checks that the guest
found the region there by itself.

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
- The main thread acts as the viewer. It checks the session and the frame
  with checks of its own, which are simpler than the client LGMP
  transport's, subscribes to the frame queue, reads the frame through
  `common/framebuffer` and compares every pixel. So this says that the
  protocol and the framebuffer code work on Windows, and nothing about the
  transport's validation of a hostile host.

The frame is 1280x720 BGRA with a padded stride of 1296 pixels, so a
pitch/width mix-up fails. The test also fails if the host sees the frame
released only because it dropped a stalled viewer. Its waits are bounded,
and ctest stops it after 60 seconds.

The shared region is an unnamed mapping private to the test process, so it
exposes nothing to other processes. The client's named section is covered
by the smoke test above.

`lg-windows-client-section-test` checks which named sections the client
takes. It makes sections with different security in its own process and opens
each as the client does: it must take one that only this user and the system
can open, with no integrity label or one of medium integrity, and refuse one
with a label below medium, as a sandboxed process of the user's would make
with a name that the client opens, and one that every account, or every
authenticated account, can write or map. ctest runs it on Windows.

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
  GitHub's runners, not on a PC with Limiar's VM, and the Looking Glass
  host has not run in a Hyper-V guest yet.
- The LGMP path copies each frame through the CPU into an OpenGL texture.
  There is no zero-copy import on Windows; DMA-BUF is Linux only.
- Resizing by dragging the window border has no automated test. The
  fullscreen toggle goes through the same resize path.
- There is no audio, clipboard or SPICE support on Windows.
- The client has run on Windows only in CI, with software OpenGL, and
  under Wine. No run on a physical Windows PC with a GPU driver is
  recorded yet.
- Run times are not performance results.
