# Looking Glass client for Windows, Limiar build @VERSION@

This is an experimental, unofficial build of the Looking Glass client from
Limiar's fork, https://github.com/templarsco/LookingGlass. It is not an
official Looking Glass release, and the Looking Glass project does not
support it.

## What works

The client runs in a native Windows window and renders with OpenGL. It
handles per-monitor DPI, resizing and fullscreen, focus changes, pointer
capture with raw mouse input, a keyboard grab while the pointer is
captured, and reconnects.

It can read frames over LGMP from a named shared memory section on the PC,
the Windows counterpart of the KVMFR device on Linux. It refuses a section
that any account but you, SYSTEM, the Administrators and Hyper-V virtual
machines can map or modify; it accepts any virtual machine's account, so
the one that makes the section decides which machine gets it.

## What does not work yet

- It cannot show a virtual machine on your PC yet. In CI it showed a
  Windows 11 virtual machine's display, which the Looking Glass IDD served
  over a Host Compute Service shared memory region, but that VM was one
  that a test created; nothing in this package sets up such a VM, and the
  guest's side, `lg-hyperv-ivshmem`, is not in it. So the client starts on
  its built-in test transport, which draws a moving test pattern. There is
  no SPICE on Windows.
- Frames are copied through the CPU; there is no zero-copy path on Windows.
- There is no audio, clipboard or file transfer.
- It has run on Windows only in CI, with Mesa's software OpenGL, and under
  Wine. It has not been checked on a physical Windows PC yet.

## Requirements

- 64-bit Windows 10 version 1703 or later.
- A graphics driver with OpenGL 3.2, or OpenGL 2.0 with `GL_ARB_sync`.

The program is not code signed, so Windows SmartScreen may warn before it
runs.

## Running

Start `looking-glass-client.exe`. A window with a moving test pattern opens.

- Scroll Lock captures and releases the mouse. Scroll Lock+F toggles
  fullscreen, Scroll Lock+O opens the settings overlay and Scroll Lock+Q
  quits.
- `looking-glass-client.exe test:input=yes` logs the keyboard and mouse
  input that the client would send to a virtual machine.
- `looking-glass-client.exe --help` lists every option. Options can also go
  in `%APPDATA%\looking-glass\client.ini`.

`lg-windows-client-producer.exe` is a test tool that stands in for a
virtual machine: it creates a shared memory section that only you can open
and serves the same moving test pattern on it. To see frames arrive over
shared memory, run it in one Command Prompt and the client in another:

```
lg-windows-client-producer.exe Local\looking-glass
looking-glass-client.exe app:transport=lgmp lgmp:shmDevice=Local\looking-glass
```

`lg-windows-client-hcs-probe.exe` checks whether this PC can give a
Hyper-V virtual machine shared memory the way IVSHMEM does under QEMU,
either as an emulated IVSHMEM PCI device or as a Host Compute Service
shared memory region. It boots small disposable Linux VMs with WSL's kernel,
checks from inside them that the PC and the VM see each other's writes, and
leaves nothing on the PC but its output folder. It needs Hyper-V, WSL and an
elevated Command Prompt:

```
lg-windows-client-hcs-probe.exe
```

It prints what works and writes `report.json` and the VMs' serial logs to
a new folder next to it. The report also lists the virtual machines and
containers the Host Compute Service already runs.

On GitHub's Windows Server 2025 runners, where Hyper-V runs nested, the
shared memory region works and the emulated IVSHMEM device does not: the
worker process of the probe's first VM with the device crashes when the
guest enables the device, which Windows records as an application error.
That VM is the probe's own; no other VM is affected. The probe has not run
on a PC with Limiar's VM yet.

`lg-windows-client-hcs-probe.exe --vm ID` instead checks an existing VM,
such as one of Hyper-V Manager, by its ID (`(Get-VM NAME).Id` in
PowerShell): whether the Host Compute Service opens it and lets this PC add
shared memory to it, which the probe removes again. The VM keeps running.

Before this build was published, the same `looking-glass-client.exe` drew a
known test frame in a window on a Windows runner, once from its test
transport and once from `lg-windows-client-producer.exe` over shared
memory, and every pixel was checked (`windows-client/client_smoke_test.py`
in the source).

## License and source code

Looking Glass is licensed under the GNU General Public License, version 2
or later; see `LICENSE.txt`. The complete source code of this build,
including every submodule, is published with it as
`looking-glass-@VERSION@-source.tar.gz`. The build is commit
@COMMIT@ of https://github.com/templarsco/LookingGlass.

`licenses` holds the licenses of the code built into the programs: Dear
ImGui, cimgui, ImPlot, cimplot, NanoSVG, the MinGW-w64 runtime and the GCC
runtime libraries. The build contains no proprietary components and no
firmware.
