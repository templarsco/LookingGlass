# Looking Glass over Hyper-V shared memory: the PC test

This is an experimental, unofficial test build from Limiar's fork of Looking
Glass, https://github.com/templarsco/LookingGlass, commit @COMMIT@. It is
not an official Looking Glass release, and the Looking Glass project does
not support it.

It checks, on a PC with Hyper-V and a GPU that Hyper-V can partition,
whether a Windows virtual machine that the Host Compute Service (HCS)
creates gets both memory shared with the PC and a partition of the GPU, and
whether the Looking Glass client on the PC then shows the VM's display.

## What it does

`hcs_probe_pc.ps1` runs everything:

1. It downloads Microsoft's evaluation ISO of Windows 11 Enterprise LTSC
   and the IVSHMEM driver that Looking Glass's installer bundles, unless
   they are in the work folder already.
2. `hcs_probe_windows_disk.ps1` makes a new virtual disk with Windows from
   the ISO, and puts on it `lg-hyperv-ivshmem.exe`, the IVSHMEM driver, the
   Looking Glass IDD from `idd`, and the driver packages of the PC's GPU,
   in the guest's `HostDriverStore`, where a guest with a partition of the
   GPU loads them from.
3. `lg-windows-client-hcs-probe.exe` boots the disk in VMs that it creates
   through the HCS, each with a shared memory region and a partition of the
   GPU. Windows sets itself up on its first boot without asking anything.
   In the guest, `lg-hyperv-ivshmem.exe` finds the region, gives the
   IVSHMEM driver a device over it, checks that the PC and the guest see
   each other's writes, and shows whether Direct3D works on the GPU. The
   guest then installs the IDD, which serves its display in the shared
   memory, and `client\looking-glass-client.exe` shows it in a window on
   the PC and saves its first frame.
4. The probe then tries VMs with the HCS's paravisor setting (HclEnabled)
   and the shared memory, tries one that stops again without the shared
   memory, and boots the guest with the paravisor setting if a VM with it
   and the shared memory keeps running.
5. The results, the guest's logs and the Hyper-V events about the probe's
   VMs go to a zip in the work folder.

Nothing else on the PC changes: no other virtual machine, no certificate,
no boot setting and no driver of the PC. The probe's VMs go away when it
ends. The test signs nothing: the IDD's package is signed by the build's
test certificate, which only the guest trusts.

## Running

Unpack the folder to a path without spaces, such as `C:\lg-pc-test`. First
see what a run would do, which needs no elevation and changes nothing:

```
powershell -ExecutionPolicy Bypass -File C:\lg-pc-test\hcs_probe_pc.ps1 -Plan
```

Then run it in an elevated Windows PowerShell, without `-Plan`:

```
powershell -ExecutionPolicy Bypass -File C:\lg-pc-test\hcs_probe_pc.ps1
```

It needs Windows with Hyper-V and its PowerShell module, and about 30 GiB
free on the drive of the work folder, `work` next to the script unless
`-WorkDir` says otherwise. The first run takes a while, mostly the ISO's
download and Windows setting itself up. At the end the script names the
zip to send back.

- `-Plan` says what a run would do, with this PC's GPUs, free space and
  what is downloaded already, and does nothing else.
- `-Gpu NAME` picks the GPU, by the Name that `Get-VMHostPartitionableGpu`
  shows, when Hyper-V can partition several, as on a PC with a GPU in the
  processor and another one on a card. `-NoGpu` tests without one, as CI
  does on runners that have no GPU.
- The IVSHMEM driver goes into the guest, and nothing here knows its hash in
  advance, so the script shows the SHA-256 of what it downloaded and of the
  ISO, and stops unless every driver catalog has a valid signature.
  `-IvshmemSha256` and `-IsoSha256` stop it unless the files are the ones
  with the hashes you give, which is how to pin a run to files that you
  checked before. `-AllowUnverifiedDriver` accepts a driver whose signature
  is not valid.
- `-Iso PATH` uses a Windows ISO that is on the PC already.
- `-Watch` leaves the client showing the guest's display until you close
  its window, or for ten minutes.
- `-Reuse` boots the disk of the last run again instead of making a new
  one.
- `-NoHcl` skips the VMs with the paravisor setting.

Deleting the work folder removes everything the test made.

## What goes in the zip

The log of the run, the probe's report, the guest's logs, the Hyper-V events
about the probe's own VMs, and the first frame that the client composed. The
probe's report says how many VMs the HCS lists on the PC, with the type, the
owner and the state of each, and no name or id of any: the zip may go to
someone else, and these identify a PC's VMs. The script also hides the user
and machine names in the text files that it zips.

The log still has the PC's Windows version, its processor, free space and the
device path of the GPU, and the frame is a picture of the guest's display,
which in this test is a new Windows. Read `pc-test.log` before sending the
zip.

While the probe runs, the PC's other VMs keep running, and share the GPU
with the probe's. Do not start a game on the PC, or in a VM that uses its
GPU, until the test ends.

## What it does not do

The HCS has no documented setting for a paravisor image of your own, such
as an OpenHCL build: its HclEnabled setting loads Windows' own. On GitHub's
Windows Server 2025 runners, the HCS created and started such a VM with the
shared memory region, and its firmware stopped with an error right away;
the same VM without the region kept running.

## License and source code

Looking Glass is licensed under the GNU General Public License, version 2
or later; see `LICENSE.txt`. This build is commit @COMMIT@ of
https://github.com/templarsco/LookingGlass, where its complete source code
is, including every submodule, and `windows-client/README.md` there
describes the probe and the guest's tool.

`licenses` holds the licenses of the code built into the programs: Dear
ImGui, cimgui, ImPlot, cimplot, NanoSVG, the MinGW-w64 runtime and the GCC
runtime libraries. The build contains no proprietary components and no
firmware.
