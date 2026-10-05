# Limiar Windows Client Workstream

This is Limiar's development fork of Looking Glass. Its goal is a native
Windows viewer on the physical PC, not just the Windows guest capturer.
The Windows client shows its own synthetic test frames, or frames that a
test producer serves on a shared memory section of the PC. Such a section
has reached disposable test VMs so far, not Limiar's VM, and no guest has
served frames on one, so the client cannot display a guest. Creating this
fork is not a claim of working guest display, GPU sharing, or 240 Hz
performance.

Upstream starting revision:
`236efcb155f952f5d7d9fcd5891a3060ad254e68`.

Limiar development is on `limiar/windows-client` and is merged into
`master`, so `master` no longer matches upstream; the upstream starting
revision above is the baseline. Upstream licenses, copyright notices and
submodule origins remain intact. This component is separate from Limiar's
MIT/Apache-licensed launcher. The Windows client build and its tests live
in [windows-client](windows-client/README.md).

## Implementation Sequence

1. Build the shared protocol and applicable common Windows code with pinned
   dependencies. Preserve Linux builds. Do not assume that B7 and current
   IDD/LGProtocol components are wire-compatible.
   Status, September 27, 2026: LGProtocol, LGMP and `common` build for the
   Windows client with MinGW-w64, cross and native. A bounded KVMFR frame
   loopback test runs in CI on Windows and Linux. No window, local endpoint
   or guest is involved yet.
2. Add a Win32 display/input implementation for the existing client
   abstraction: DPI, resize, focus, cursor capture/release, relative mouse,
   keyboard state and disconnect recovery.
   Status, September 27, 2026: the client builds for Windows, cross and
   native, with a Win32 display server that covers these items. Windows
   only has the synthetic `test` transport. CI shows a known test frame in
   a native window and checks every pixel, using Mesa's software OpenGL;
   the endpoint half of the first gate below waits for step 3. The Win32
   input layer has unit tests. Focus, capture, DPI and reconnect behavior
   was checked by hand under Wine, not yet on a physical Windows PC.
3. Add an explicit Windows local shared-memory endpoint with per-user ACLs
   and a negotiated protocol version. Do not reinterpret a Linux DMA-BUF
   file descriptor as a Windows handle. Never expose an unauthenticated
   capture/input endpoint.
   Status, September 28, 2026: the endpoint is a named section, the Windows
   counterpart of `/dev/kvmfr0`. The client's LGMP transport builds for
   Windows and opens it by name (`lgmp:shmDevice`, `Global\looking-glass` by
   default); the existing LGMP and KVMFR session checks negotiate the
   protocol versions. Before mapping it, the client refuses a section whose
   owner or DACL lets any account but the user, the user's logon session,
   SYSTEM, Administrators or a Hyper-V VM account map or modify it. CI serves
   a known frame from a test producer on such a section, with a padded
   stride, and checks every pixel in the client's window; it also checks
   that a section every account can open is refused. This meets the first
   acceptance gate. Frames are copied through the CPU; DMA-BUF import stays
   Linux only.
4. Present actual guest frames using a native Windows graphics path; verify
   nonblank content, correct stride/format and synchronization. A CPU-copy
   bring-up path must be reported as such, not advertised as zero-copy.
5. Integrate the QEMU transport/guest device and the compatible Windows
   capture component. GPU delivery is a separate prerequisite owned by
   Limiar's VM backend, not something the viewer creates.
   Limiar selected native Hyper-V with OpenHCL on September 25, 2026, after
   this sequence was written. IVSHMEM is a QEMU device, so on September 28,
   2026 the plan became porting it to Hyper-V rather than replacing it:
   - The PC side is the section from step 3.
   - First route: the Host Compute Service's device emulation API (HDV)
     lets a process on the PC present a PCI device to a VM and back a BAR
     with a section (`HdvCreateSectionBackedMmioRange`). Presenting QEMU's
     ivshmem-plain (1af4:1110) with BAR2 backed by the section gives the
     guest a real IVSHMEM device, so the guest's IVSHMEM driver and the
     Looking Glass host run unchanged, without a custom guest driver or test
     signing.
   - Second route: the HCS `SharedMemory` device maps a named section into
     guest memory (schema 2.1: `SharedMemoryRegion` with `SectionName`,
     `StartOffset`, `Length` and `AllowGuestWrite`), and the guest physical
     address can be queried as `SharedMemoryRegionInfo`. Something in the VM
     then has to present that memory as a device; in a Windows guest,
     `lg-hyperv-ivshmem` does (see the status below).
   - Both routes need a VM that the HCS created or can open. Whether
     Limiar's native Hyper-V VM qualifies is open. Neither route is tested
     with GPU-PV or OpenHCL.
   - The host in the guest must be built from the same source as the client.
     This client speaks KVMFR 34 and LGMP 12, and the B7 release is older, so
     the client refuses a B7 host at the version check.
   Status, September 28, 2026: `lg-windows-client-hcs-probe` checks both
   routes with disposable Linux VMs whose init checks, from inside the VM,
   that the PC and the guest see each other's writes; see
   [windows-client](windows-client/README.md#hcs-shared-memory-probe). CI
   runs it on GitHub's Windows Server 2025 runners (10.0.26100), where
   Hyper-V runs nested:
   - The second route works there. The HCS maps the section, named
     `\BaseNamedObjects\<name>`, right after the VM's memory, and the PC
     and the guest see each other's writes. The region has to be in the
     VM's configuration when the HCS creates the VM: the HCS does not add
     one to a running VM (0x80070032).
   - The first route does not work yet. The HCS offers the emulated device
     only when the VM's configuration declares it under `FlexibleIov`, and
     the guest reaches its configuration space but not its BARs. Backing
     BAR2 by the section makes the VM's worker process fail fast in
     `vmvpci.dll` when the guest enables the device's memory space, or has
     no effect once the guest is up.
   - Neither route has run with Limiar's VM. Limiar's GPU-PV guide drives
     that VM with Hyper-V's PowerShell module, so the Hyper-V management
     service (VMMS) runs it, not the HCS directly. On the runners, the HCS
     lists and opens such a VM and creates a device host for it, but
     refuses to add the region to it (0x8004102B). So the second route
     needs a VM that the HCS creates with the region in its configuration,
     which is how Limiar will create its VM.
   Status, September 29, 2026: the second route reaches the unchanged
   IVSHMEM driver in Windows guests. `lg-hyperv-ivshmem` runs in the guest
   and makes a root-enumerated device with the IVSHMEM hardware IDs and a
   forced configuration over the region: a page that the PC keeps zeroed
   as the registers, which reads as an ivshmem-plain without interrupts
   that is peer 0, and the rest as the shared memory; see
   [windows-client](windows-client/README.md#ivshmem-device-for-hyper-v-guests).
   The probe's `--windows-disk` mode checks it on the runners in disposable
   Windows Server 2025 and Windows 11 Enterprise LTSC (24H2) guests with a
   32 MiB region, and 128 MiB for the IDD:
   - The IVSHMEM driver that Looking Glass's host installer bundles installs
     on the device unchanged, starts and maps the shared memory as peer 0.
     Through its mapping, the guest reads the pattern that the PC wrote on
     every page, and each side sees the other's write. Windows lists the
     region as reserved memory, not as RAM.
   - The VM does not survive Windows restarting itself: it fails to reset
     and stops. A new VM with the same section and disk passes, so Limiar
     would have to start its VM again when Windows restarts. With the
     guest's memory physically backed instead of by the VM worker's virtual
     memory, the HCS does not create a VM with the region at all.
   - The Looking Glass IDD serves the Windows 11 guest's display over the
     region: it installs, opens the IVSHMEM device, renders in software for
     want of a GPU, and the client on the PC composes its 1920x1080 frames.
   - The guest finds the region by itself, so the PC does not have to
     tell it where the region is: Windows keeps it as Loader Reserved
     memory that starts where RAM ends, which `lg-hyperv-ivshmem` looks
     for. Both guests found it where the HCS said it put it.
   - The HCS's paravisor setting (`HclEnabled`) loads Windows' own
     paravisor image; the HCS has no documented setting for one's own,
     such as an OpenHCL build. On the runners it creates and starts a VM
     with `GuestStateOnly` isolation, a guest state file and the region,
     and puts the region where it does without the paravisor, but the VM's
     firmware stops with a fatal error right after the start. The same VM
     without the region keeps running, so the region and Windows'
     paravisor do not go together there.
   - Open: whether the HCS gives the region to a VM with GPU-PV and
     OpenHCL, and the IDD in such a guest rendering on the GPU. The runners
     have no GPU, so this needs a PC:
     [hcs_probe_pc.ps1](windows-client/hcs_probe_pc.ps1) runs the whole
     check there, in disposable VMs of the HCS, with a partition of the
     PC's GPU and its driver in the guest, and with the paravisor setting;
     see [windows-client](windows-client/README.md#pc-test).
6. Add audio and complete reconnect/resolution-change handling. Measure
   frame pacing and input/display latency at 60/120/240 Hz on the actual
   host; a configured refresh rate is not a performance result.

## Initial Acceptance Gates

- One known test frame crosses the local endpoint and reaches a native
  Windows window with checked pixel contents.
- An animated producer survives resize, focus changes and reconnects.
- A real VM frame stream works without host desktop capture as a substitute.
- Input is released on disconnect/focus loss and cannot target another VM.
- The Windows client, Linux client and guest components retain their own
  bounded tests and reproducible build/version records.

This workstream does not wait for upstream to publish a Windows client.
Useful fixes should remain reviewable for contribution upstream.

Related VM project: [Limiar](https://github.com/templarsco/limiar).
