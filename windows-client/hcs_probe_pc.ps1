#
# Looking Glass
# Copyright © 2017-2026 The Looking Glass Authors
# https://looking-glass.io
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc., 59
# Temple Place, Suite 330, Boston, MA 02111-1307 USA
#

<#
.SYNOPSIS
Tests Looking Glass over Hyper-V shared memory on this PC, with a partition
of its GPU, in virtual machines that go away when the test ends.

.DESCRIPTION
Makes a new disk with Windows 11 from Microsoft's evaluation ISO, with the
IVSHMEM driver, the Looking Glass IDD, lg-hyperv-ivshmem and the driver
packages of this PC's GPU in the guest's HostDriverStore. The HCS probe then
boots it in virtual machines that it makes through the Host Compute Service,
with memory shared with this PC and a partition of the GPU: the guest checks
the shared memory and Direct3D on the GPU, the IDD serves the guest's
display in the shared memory, and the Looking Glass client in this folder
shows it on this PC. Then the probe tries virtual machines with the HCS's
paravisor setting, unless -NoHcl.

Everything goes to WorkDir: the downloads, the disk, and a folder of results
with a zip to send back. Nothing else on this PC changes: no other virtual
machine, no certificate, no boot setting and no driver of this PC. The
probe's virtual machines go away when it ends.

Run it in an elevated Windows PowerShell on Windows with Hyper-V, from a
folder whose path has no spaces, such as C:\lg-pc-test:

  powershell -ExecutionPolicy Bypass -File C:\lg-pc-test\hcs_probe_pc.ps1

-Gpu picks the GPU by the Name that Get-VMHostPartitionableGpu shows, when
there are several, and -NoGpu tests without one, as CI does. -Iso takes a
Windows ISO already on this PC instead of downloading one. -Watch leaves the client showing the guest's display until
you close it, or for ten minutes. -Reuse boots the disk of the last run
again instead of making a new one.
#>

[CmdletBinding()]
param(
  [string] $WorkDir = "$PSScriptRoot\work",
  [string] $Iso,
  [string] $Gpu,
  [switch] $NoGpu,
  [switch] $Watch,
  [switch] $Reuse,
  [switch] $NoHcl
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

# Microsoft's evaluation ISO of Windows 11 Enterprise LTSC, 64-bit, en-US,
# with which CI checks the same guest
$isoUrl = 'https://go.microsoft.com/fwlink/?linkid=2289029&clcid=0x409' +
  '&culture=en-us&country=us'
# the IVSHMEM driver that Looking Glass's installer bundles
$driverUrl = 'https://dl.quantum2.xyz/ivshmem.tar.gz'

# runs a program, whose messages on its error output, such as curl's
# progress, are not errors of this script when its output is redirected;
# returns its exit code
function Invoke-Native([scriptblock] $command) {
  $ErrorActionPreference = 'Continue'
  & $command | Out-Host
  return $LASTEXITCODE
}

$principal = New-Object Security.Principal.WindowsPrincipal(
  [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this in an elevated Windows PowerShell (Run as administrator)'
}

# the probe takes the client's command line as one argument, which Windows
# PowerShell cannot pass with quotes inside
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
foreach ($path in $PSScriptRoot, $WorkDir) {
  if ($path -match '[\s"]') {
    throw "$path has a space or a quote in it: move this folder to a " +
      'path without any, such as C:\lg-pc-test'
  }
}
foreach ($file in 'lg-windows-client-hcs-probe.exe', 'lg-hyperv-ivshmem.exe',
    'client\looking-glass-client.exe', 'idd\LGIddInstall.exe',
    'hcs_probe_windows_disk.ps1') {
  if (-not (Test-Path -LiteralPath "$PSScriptRoot\$file")) {
    throw "$file is missing from $PSScriptRoot`: unpack the whole bundle"
  }
}
if (-not (Get-Command Get-VMHostPartitionableGpu -ErrorAction Ignore)) {
  throw "Hyper-V's PowerShell module is missing: turn on Hyper-V with its " +
    'management tools'
}

# the bundle came from the internet; what it copies into the guest should
# not carry that mark there
Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -File |
  Where-Object FullName -notlike "$WorkDir\*" | Unblock-File

# the GPU to partition, which must be named if there are several
$gpus   = @()
$chosen = $null
if ($NoGpu) {
  Write-Host 'Testing without a partition of a GPU'
} else {
  $gpus = @(Get-VMHostPartitionableGpu)
  if (-not $gpus) {
    throw 'Hyper-V finds no GPU on this PC that it can partition ' +
      '(Get-VMHostPartitionableGpu shows none); -NoGpu tests without one'
  }
  if ($Gpu) {
    $chosen = $gpus | Where-Object Name -eq $Gpu | Select-Object -First 1
    if (-not $chosen) {
      throw "$Gpu is not one of the GPUs that Get-VMHostPartitionableGpu " +
        'shows'
    }
  } elseif ($gpus.Count -eq 1) {
    $chosen = $gpus[0]
  } else {
    $gpus | Format-List Name | Out-String | Write-Host
    throw 'Hyper-V can partition several GPUs on this PC: pass the one to ' +
      'test with -Gpu NAME'
  }
}

$disk = "$WorkDir\disk\windows.vhdx"
$fresh = -not ($Reuse -and (Test-Path -LiteralPath $disk))
$needed = if ($fresh) { 30GB } else { 5GB }
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$drive = (Get-Item -LiteralPath $WorkDir).PSDrive
if ($drive.Free -lt $needed) {
  throw ('{0} has {1:N0} GiB free, and the test needs {2:N0} GiB' -f
    $drive.Root, ($drive.Free / 1GB), ($needed / 1GB))
}

$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$results = "$WorkDir\results-$stamp"
New-Item -ItemType Directory -Force $results | Out-Null
$began = Get-Date
Start-Transcript -LiteralPath "$results\pc-test.log" | Out-Null
$probeExit = $null
$failure   = $null
try {
  Write-Host "== This PC"
  Get-CimInstance Win32_OperatingSystem |
    Format-List Caption, Version, BuildNumber, OSArchitecture |
    Out-String | Write-Host
  Get-CimInstance Win32_Processor | Format-List Name | Out-String |
    Write-Host
  $virtualization = Get-ItemProperty -ErrorAction Ignore `
    'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization'
  Write-Host ('AllowFirmwareLoadFromFile: {0}' -f
    $virtualization.AllowFirmwareLoadFromFile)
  Write-Host "== The GPUs that Hyper-V can partition"
  $gpus | Select-Object * -ExcludeProperty ComputerName | Format-List |
    Out-String -Width 250 | Write-Host
  if ($chosen) {
    Write-Host "Testing with $($chosen.Name)"
  }

  $downloads = "$WorkDir\downloads"
  New-Item -ItemType Directory -Force $downloads | Out-Null
  function Get-Download([string] $url, [string] $path) {
    if (Test-Path -LiteralPath $path) {
      Write-Host "Using $path"
      return
    }
    Write-Host "Downloading $url"
    $code = Invoke-Native {
      & "$env:SystemRoot\System32\curl.exe" -L --fail --retry 4 `
        --retry-all-errors --show-error -C - -o "$path.part" $url
    }
    if ($code) {
      throw "curl.exe could not download $url (exit $code)"
    }
    Move-Item -LiteralPath "$path.part" -Destination $path
  }

  # the IVSHMEM driver, with the INF for the newest Windows in the archive,
  # as CI picks it
  $driverDir = "$WorkDir\ivshmem-driver"
  if (-not (Test-Path -LiteralPath $driverDir)) {
    Get-Download $driverUrl "$downloads\ivshmem.tar.gz"
    New-Item -ItemType Directory "$driverDir.part" -Force | Out-Null
    $code = Invoke-Native {
      & "$env:SystemRoot\System32\tar.exe" -xzf `
        "$downloads\ivshmem.tar.gz" -C "$driverDir.part"
    }
    if ($code) {
      throw "tar.exe could not unpack the IVSHMEM driver (exit $code)"
    }
    Move-Item -LiteralPath "$driverDir.part" -Destination $driverDir
  }
  Get-ChildItem -LiteralPath $driverDir -Recurse -File -Include *.cat, *.sys |
    Get-AuthenticodeSignature | Format-List Path, Status,
      @{ n = 'Signer'; e = { $_.SignerCertificate.Subject } } |
    Out-String -Width 250 | Write-Host
  $infs = @(Get-ChildItem -LiteralPath $driverDir -Recurse -Filter *.inf)
  $inf = $null
  foreach ($os in 'w11', 'win11', '2k25', '2k22', 'w10', 'win10', '') {
    $inf = $infs | Where-Object FullName -match "$os.*(amd64|x64)" |
      Select-Object -First 1
    if ($inf) {
      break
    }
  }
  if (-not $inf) {
    $inf = $infs | Select-Object -First 1
  }
  if (-not $inf) {
    throw "The IVSHMEM driver in $driverDir has no INF"
  }
  Write-Host "IVSHMEM driver: $($inf.FullName)"

  # a new disk, unless -Reuse and there is one
  if ($fresh) {
    if (-not $Iso) {
      $Iso = "$downloads\windows11-enterprise-ltsc-eval.iso"
      Get-Download $isoUrl $Iso
    }
    $Iso = [IO.Path]::GetFullPath($Iso)
    Get-Item -LiteralPath $Iso | Format-List FullName, Length |
      Out-String | Write-Host
    if (Test-Path -LiteralPath $disk) {
      Dismount-VHD -Path $disk -ErrorAction Ignore
      Remove-Item -LiteralPath $disk
    }
    Write-Host "== Making $disk"
    $gpuPackages = @{}
    if ($chosen) {
      $gpuPackages.GpuPv = $chosen.Name
    }
    & "$PSScriptRoot\hcs_probe_windows_disk.ps1" -Iso $Iso -Disk $disk `
      -Tool "$PSScriptRoot\lg-hyperv-ivshmem.exe" -Inf $inf.FullName `
      -Idd "$PSScriptRoot\idd" @gpuPackages
  } else {
    Write-Host "== Booting the disk of the last run again, $disk"
  }

  # the client reads the guest's display from the shared memory: its first
  # frame to a capture, or until it is closed with -Watch
  $client = "$PSScriptRoot\client\looking-glass-client.exe " +
    'app:transport=lgmp lgmp:shmDevice={section} app:renderer=OpenGL ' +
    'win:size=1280x720 win:quickSplash=yes win:alerts=no ' +
    'opengl:amdPinnedMem=no'
  if (-not $Watch) {
    $client += " test:captureFile=$results\guest.lgcapture " +
      'test:captureFrame=1 test:captureDelay=1'
  }
  $probeArgs = @('--windows-disk', $disk, '--size-mib', '128', '--client',
    $client, '--out', "$results\probe")
  if ($chosen) {
    $probeArgs += '--gpu', $chosen.Name
  }
  if (-not $NoHcl) {
    $probeArgs += '--hcl', 'auto'
  }

  Write-Host "== The probe"
  Write-Host ("Windows sets itself up on the first boot, which takes a " +
    "while; the client's window opens when the guest's display arrives")
  $probeExit = Invoke-Native {
    & "$PSScriptRoot\lg-windows-client-hcs-probe.exe" @probeArgs
  }
  Write-Host "The probe exited with $probeExit"
} catch {
  # the results so far still go to the zip
  $failure = $_
  Write-Host "The test stopped: $_"
} finally {
  # what the guest logged, from its disk
  if (Test-Path -LiteralPath $disk) {
    try {
      & "$PSScriptRoot\hcs_probe_windows_disk.ps1" -Logs -Disk $disk *>&1 |
        Out-File -LiteralPath "$results\guest-logs.txt" -Width 250
    } catch {
      "Could not read the guest's logs: $_" |
        Out-File -LiteralPath "$results\guest-logs.txt" -Append
    }
  }

  # the Hyper-V events about the probe's virtual machines only
  $report = "$results\probe\report.json"
  $ids = @()
  if (Test-Path -LiteralPath $report) {
    $ids = @([regex]::Matches((Get-Content -Raw -LiteralPath $report),
        '"vm_id":"([0-9a-fA-F-]{36})"') | ForEach-Object {
      $_.Groups[1].Value } | Sort-Object -Unique)
  }
  $events = foreach ($log in Get-WinEvent -ListLog *Hyper-V*, *Hcs* `
      -ErrorAction Ignore | Where-Object RecordCount -gt 0) {
    Get-WinEvent -ErrorAction Ignore -FilterHashtable @{
        LogName = $log.LogName; StartTime = $began } |
      Where-Object {
        $message = "$($_.Message)"
        $message -match 'lg-hcs-probe' -or @($ids | Where-Object {
          $message -match $_ }).Count
      } | ForEach-Object {
        '{0} {1} {2} {3}: {4}' -f $_.TimeCreated.ToString('HH:mm:ss.fff'),
          $log.LogName, $_.Id, $_.LevelDisplayName,
          ("$($_.Message)" -replace '\s+', ' ')
      }
  }
  $events | Sort-Object | Out-File -LiteralPath "$results\hyperv-events.txt" `
    -Width 1000

  Stop-Transcript | Out-Null
}

# what worked, from the probe's report
$summary = @()
try {
  $data = Get-Content -Raw -LiteralPath "$results\probe\report.json" |
    ConvertFrom-Json
  foreach ($case in $data.cases) {
    if ($case.case -eq 'windows') {
      $summary += "Windows guest with the shared memory: " +
        $(if ($case.passed) { 'passed' } else { 'failed' })
      foreach ($start in $case.starts) {
        $keys = $start.PSObject.Properties.Name
        $line = '  start:'
        foreach ($key in 'found_region', 'gpu', 'gpu_in_guest', 'idd',
            'client_exit') {
          if ($keys -contains $key) {
            $line += " $key=$($start.$key)"
          }
        }
        $summary += $line
      }
    } elseif ($case.case -eq 'hcl') {
      $summary += "Paravisor setting (HclEnabled) with shared memory: " +
        $(if ($case.passed) { 'a VM ran' } else { 'no VM ran' }) +
        ", the Windows guest found the region: $($case.windows_found_region)"
      foreach ($attempt in $case.attempts) {
        $summary += ('  {0}, {1}, {2}: started={3} kept_running={4}' -f
          $attempt.isolation_type, $attempt.guest_state_type,
          $(if ($attempt.region) { 'shared memory' } else { 'no memory' }),
          $attempt.started, $attempt.kept_running)
      }
    }
  }
} catch {
  $summary += "Could not read the probe's report: $_"
}
if ($failure) {
  $summary += "The test stopped: $failure"
}
$summary += "The probe exited with $probeExit"
$summary | Set-Content -LiteralPath "$results\summary.txt"
$summary | Write-Host

$zip = "$WorkDir\lg-pc-test-$stamp.zip"
Compress-Archive -Path "$results\*" -DestinationPath $zip -Force
Write-Host ''
Write-Host "Send this file back: $zip"
Write-Host ("The disk stays in $WorkDir\disk for -Reuse; delete $WorkDir " +
  'to remove everything the test made')
if ($failure) {
  exit 1
}
exit $probeExit
