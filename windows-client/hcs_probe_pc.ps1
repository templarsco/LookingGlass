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

-Plan only says what a run would do, and changes nothing: it downloads
nothing, makes no folder and starts no virtual machine, and it needs no
elevation. Run it first.

Elevated code is run from the folder of this script and reads what is put in
the work folder, so the script stops if another account can change either,
as it can in a folder made in the root of C:. Move the bundle to a folder
that only the administrators can write, or add -LockFolders, which locks the
folders to SYSTEM and the administrators, with read access for you, and which
then need an elevated shell to delete.

-Gpu picks the GPU by the Name that Get-VMHostPartitionableGpu shows, when
there are several, and -NoGpu tests without one, as CI does. -Iso takes a
Windows ISO already on this PC instead of downloading one. -Watch leaves
the client showing the guest's display until you close it, or for ten
minutes. -Reuse boots the disk of the last run again instead of making a
new one.

The IVSHMEM driver goes into the guest, and nothing here knows its hash in
advance, so the script shows the SHA-256 of what it downloaded and of the
ISO, and stops unless the catalog of every driver package has a valid
signature. -IvshmemSha256 and -IsoSha256 stop it unless the files match
the hashes you give, which is how to pin a run to files that you checked
before; -AllowUnverifiedDriver accepts a driver whose signature is not
valid.

The zip has the log of the run and the probe's report. The report names no
virtual machine of this PC, only how many the HCS lists and their type,
owner and state, and the script hides the user and machine names in the
text files that it zips. Read pc-test.log before sending it anywhere: it
has this PC's Windows version, its processor and the device path of the GPU.
#>

[CmdletBinding()]
param(
  [string] $WorkDir,
  [string] $Iso,
  [string] $Gpu,
  [switch] $NoGpu,
  [switch] $Watch,
  [switch] $Reuse,
  [switch] $NoHcl,
  [switch] $Plan,
  [string] $IvshmemSha256,
  [string] $IsoSha256,
  [switch] $AllowUnverifiedDriver,
  [switch] $LockFolders
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

# next to the script, which is not a default of the parameter: Windows
# PowerShell 5.1 does not have $PSScriptRoot yet when it sets those, and the
# work folder was \work
if (-not $WorkDir) {
  $WorkDir = Join-Path $PSScriptRoot 'work'
}

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
if (-not $Plan -and -not $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw ('Run this in an elevated Windows PowerShell (Run as ' +
    'administrator); -Plan needs no elevation')
}

# a SHA-256 as the hex text that Get-FileHash returns, or an error
function Test-Sha256([string] $name, [string] $value) {
  if ($value -and $value -notmatch '^[0-9a-fA-F]{64}$') {
    throw "$name must be 64 hexadecimal digits"
  }
}
Test-Sha256 '-IvshmemSha256' $IvshmemSha256
Test-Sha256 '-IsoSha256' $IsoSha256

# shows a file's SHA-256, and stops if it is not the one that was asked for
function Confirm-Sha256([string] $what, [string] $path, [string] $expected) {
  $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
  Write-Host "$what SHA-256: $actual"
  if ($expected -and $actual -ne $expected) {
    throw "$what is not the file that was asked for: its SHA-256 is " +
      "$actual, and not $expected"
  }
}

# the user's and the machine's names, in the text files that go to the zip. A
# transcript starts with both, and a path may have the first
function Hide-Identity([string] $folder) {
  $names = @($env:USERNAME, $env:COMPUTERNAME) |
    Where-Object { $_ -and $_.Length -ge 3 }
  $files = Get-ChildItem -LiteralPath $folder -Recurse -File `
    -Include *.log, *.txt, *.json
  foreach ($file in $files) {
    $text = [IO.File]::ReadAllText($file.FullName)
    $text = $text -replace '(?m)^(Username|RunAs User|Machine):[^\r\n]*',
      '$1: (hidden)'
    foreach ($name in $names) {
      $text = $text -replace
        "(?i)(?<![A-Za-z0-9])$([regex]::Escape($name))(?![A-Za-z0-9])",
        '(hidden)'
    }
    [IO.File]::WriteAllText($file.FullName, $text,
      (New-Object Text.UTF8Encoding($false)))
  }
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
# Elevated code is run from the folder of this script, and reads what is put in
# the work folder, for tens of minutes. A folder that C:\ gave to every user
# lets any of them replace the probe, a script or the ISO, and be run as an
# administrator, so no account but SYSTEM, the Administrators and this user
# may change them. These are the others that may, or may own the folder and so
# say who may
function Get-OtherWriters([string] $folder) {
  $me = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
  $trusted = @('S-1-5-18', 'S-1-5-32-544', $me,
    'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
  $rights = [Security.AccessControl.FileSystemRights]
  $write = $rights::WriteData -bor $rights::AppendData -bor
    $rights::WriteExtendedAttributes -bor $rights::WriteAttributes -bor
    $rights::Delete -bor $rights::DeleteSubdirectoriesAndFiles -bor
    $rights::ChangePermissions -bor $rights::TakeOwnership

  $acl = Get-Acl -LiteralPath $folder
  $others = @()
  foreach ($ace in $acl.Access) {
    # an entry that only what is inside inherits gives nothing on the folder
    if ($ace.AccessControlType -ne 'Allow' -or ($ace.PropagationFlags -band
        [Security.AccessControl.PropagationFlags]::InheritOnly) -or
        -not ($ace.FileSystemRights -band $write)) {
      continue
    }
    $sid = $ace.IdentityReference.Translate(
      [Security.Principal.SecurityIdentifier]).Value
    if ($sid -notin $trusted) {
      $others += $ace.IdentityReference.Value
    }
  }
  $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
  if ($owner -notin $trusted) {
    $others += "the owner of the folder, $owner"
  }
  return @($others | Select-Object -Unique)
}

# SYSTEM and the Administrators change it, and this user reads it
function Lock-Folder([string] $folder) {
  $me = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
  & icacls.exe $folder /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' `
    '*S-1-5-32-544:(OI)(CI)F' "*${me}:(OI)(CI)RX" | Out-Null
  if ($LASTEXITCODE) {
    throw "icacls could not lock $folder (exit $LASTEXITCODE)"
  }
}

$unsafe = @()
foreach ($folder in $PSScriptRoot, $WorkDir) {
  if (Test-Path -LiteralPath $folder) {
    $writers = @(Get-OtherWriters $folder)
    if ($writers) {
      $unsafe += [pscustomobject]@{ Folder = $folder; Writers = $writers }
    }
  }
}
if ($unsafe -and -not $Plan) {
  if ($LockFolders) {
    foreach ($item in $unsafe) {
      Write-Host "Locking $($item.Folder): it can be changed by $(
        $item.Writers -join ', ')"
      Lock-Folder $item.Folder
    }
  } else {
    $lines = $unsafe | ForEach-Object {
      "$($_.Folder) can be changed by $($_.Writers -join ', ')" }
    throw ("This runs as an administrator what is in these folders, which " +
      "other accounts can change, and could replace:`n" +
      ($lines -join "`n") + "`nLock them to SYSTEM and the administrators " +
      'yourself, or run this again with -LockFolders, which does it and gives ' +
      'you read access; a locked folder needs an elevated shell to delete')
  }
}

if (-not (Get-Command Get-VMHostPartitionableGpu -ErrorAction Ignore)) {
  throw "Hyper-V's PowerShell module is missing: turn on Hyper-V with its " +
    'management tools'
}

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

# the drive of the work folder, which may not exist yet
$drive = Get-PSDrive -PSProvider FileSystem |
  Where-Object { $WorkDir.StartsWith($_.Root, 'OrdinalIgnoreCase') } |
  Sort-Object { $_.Root.Length } -Descending | Select-Object -First 1
if (-not $drive) {
  throw "$WorkDir is not on a drive of this PC"
}
if ($drive.Free -lt $needed) {
  throw ('{0} has {1:N0} GiB free, and the test needs {2:N0} GiB' -f
    $drive.Root, ($drive.Free / 1GB), ($needed / 1GB))
}

if ($Plan) {
  function Get-State([string] $path) {
    if (Test-Path -LiteralPath $path) { 'already in the work folder' }
    else { 'to download' }
  }
  $downloads  = Join-Path $WorkDir 'downloads'
  $isoPath    = Join-Path $downloads 'windows11-enterprise-ltsc-eval.iso'
  $driverPath = Join-Path $downloads 'ivshmem.tar.gz'

  $gpuText = if ($chosen) { $chosen.Name } else { 'none (-NoGpu)' }
  $isoText = if ($Iso) { "$Iso, yours" }
    elseif (-not $fresh) {
      'not needed: the disk of the last run is booted again' }
    else { "$isoUrl, about 5 GB, $(Get-State $isoPath)" }
  $isoPin = if ($IsoSha256) { 'pinned' } else {
    'not pinned, its SHA-256 is shown' }
  $driverPin = if ($IvshmemSha256) { 'pinned' } else {
    'not pinned, its SHA-256 is shown' }
  $signature = if ($AllowUnverifiedDriver) {
    'a driver whose catalog is not validly signed is accepted'
  } else {
    'a driver whose catalog is not validly signed stops the run'
  }

  $diskText = if ($fresh) {
    "a new 40 GB dynamic disk, $disk, that is mounted to put Windows from " +
      'the ISO on it, with the IVSHMEM driver, the Looking Glass IDD and ' +
      'lg-hyperv-ivshmem'
  } else {
    "the disk $disk, which is booted again"
  }
  if ($fresh -and $chosen) {
    $diskText += ", and the driver packages of the GPU in the guest's " +
      'HostDriverStore'
  }
  $vmText = 'virtual machines that the Host Compute Service makes for the ' +
    'probe, each with 128 MiB of memory shared with this PC'
  if ($chosen) { $vmText += ' and a partition of the GPU' }
  if (-not $NoHcl) { $vmText += ', and then ones with the paravisor setting' }
  $clientText = if ($Watch) { 'show the guest until you close it' } else {
    'save the first frame of the guest' }

  Write-Host '== What a run would do. Nothing was changed.'
  $folderText = 'Work folder      : {0}, on {1}, which has {2:N0} GiB free; ' +
    'the test needs {3:N0}'
  Write-Host ($folderText -f $WorkDir, $drive.Root, ($drive.Free / 1GB),
    ($needed / 1GB))
  Write-Host "GPU to partition : $gpuText"
  Write-Host "Windows ISO      : $isoText ($isoPin)"
  Write-Host ("IVSHMEM driver   : $driverUrl, $(Get-State $driverPath) " +
    "($driverPin)")
  Write-Host "Driver signature : $signature"
  Write-Host ''
  Write-Host 'In the work folder, with an elevated PowerShell, it would make:'
  Write-Host "  - $diskText"
  Write-Host "  - $vmText; they go away when the probe ends"
  Write-Host ("  - a window of the Looking Glass client on this PC, to " +
    $clientText)
  Write-Host "  - a zip of the log and the probe's report"
  Write-Host ''
  if ($unsafe) {
    Write-Host ''
    Write-Host ('== Elevated code is run from these folders, and other ' +
      'accounts can change them:')
    foreach ($item in $unsafe) {
      Write-Host "  $($item.Folder): $($item.Writers -join ', ')"
    }
    Write-Host ('A run stops until they are moved somewhere that only the ' +
      'administrators can write, or -LockFolders locks them to SYSTEM and ' +
      'the administrators, with read access for you.')
  }
  Write-Host ''
  Write-Host ('Nothing else on this PC changes: no other virtual machine, no ' +
    'certificate, no boot setting and no driver of this PC. The disk is a ' +
    'new file that is mounted for a while, and Windows gives its volumes ' +
    'drive letters until it is dismounted. Other virtual machines keep ' +
    "running, and share the GPU with the probe's.")
  exit 0
}

# the bundle came from the internet; what it copies into the guest should
# not carry that mark there
Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -File |
  Where-Object FullName -notlike "$WorkDir\*" | Unblock-File

$workExisted = Test-Path -LiteralPath $WorkDir
New-Item -ItemType Directory -Force $WorkDir | Out-Null
if (-not $workExisted -and @(Get-OtherWriters $WorkDir)) {
  # a folder that this made takes its parent's permissions
  Lock-Folder $WorkDir
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
    Confirm-Sha256 'The IVSHMEM driver archive' "$downloads\ivshmem.tar.gz" `
      $IvshmemSha256
    New-Item -ItemType Directory "$driverDir.part" -Force | Out-Null
    $code = Invoke-Native {
      & "$env:SystemRoot\System32\tar.exe" -xzf `
        "$downloads\ivshmem.tar.gz" -C "$driverDir.part"
    }
    if ($code) {
      throw "tar.exe could not unpack the IVSHMEM driver (exit $code)"
    }
    Move-Item -LiteralPath "$driverDir.part" -Destination $driverDir
  } elseif ($IvshmemSha256) {
    # the folder came from an earlier run, so the archive has to be there to
    # show that it is the one that was asked for
    if (-not (Test-Path -LiteralPath "$downloads\ivshmem.tar.gz")) {
      throw "$driverDir is from an earlier run, and $downloads\ivshmem.tar.gz " +
        'is not here to check against -IvshmemSha256: delete the folder'
    }
    Confirm-Sha256 'The IVSHMEM driver archive' "$downloads\ivshmem.tar.gz" `
      $IvshmemSha256
  }
  $signatures = @(Get-ChildItem -LiteralPath $driverDir -Recurse -File `
      -Include *.cat, *.sys | Get-AuthenticodeSignature)
  $signatures | Format-List Path, Status,
      @{ n = 'Signer'; e = { $_.SignerCertificate.Subject } } |
    Out-String -Width 250 | Write-Host

  # a driver's signature is its catalog's, and the guest is going to load
  # this one: the catalog has to be valid, and there has to be one
  $catalogs = @($signatures | Where-Object { $_.Path -like '*.cat' })
  $invalid  = @($catalogs | Where-Object Status -ne 'Valid')
  if ((-not $catalogs -or $invalid) -and -not $AllowUnverifiedDriver) {
    $what = if ($invalid) {
      "$($invalid.Count) of its $($catalogs.Count) catalogs are not validly " +
        'signed'
    } else { 'it has no catalog' }
    throw ("The IVSHMEM driver in $driverDir is not validly signed ($what). " +
      'Check where it came from, or run again with -AllowUnverifiedDriver')
  }
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
    Confirm-Sha256 'The Windows ISO' $Iso $IsoSha256
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
  if ($Watch) {
    # a window that is left open is how this ends, and not a failure
    $probeArgs += '--watch'
  }
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
        foreach ($key in 'guest_ready', 'found_region', 'gpu', 'gpu_in_guest',
            'idd', 'client_exit') {
          if ($keys -contains $key) {
            $line += " $key=$($start.$key)"
          }
        }
        $summary += $line
        # the end of the guest's list of its display adapters
        if ($keys -contains 'adapters' -and @($start.adapters).Count) {
          $summary += "    $(@($start.adapters)[-1])"
        }
      }
    } elseif ($case.case -eq 'hcl') {
      $summary += "Paravisor setting (HclEnabled) with shared memory: " +
        $(if ($case.passed) { 'a VM ran' } else { 'no VM ran' }) +
        ", the Windows guest found the region: $($case.windows_found_region)"
      foreach ($attempt in $case.attempts) {
        $summary += ('  {0}, {1}, {2}: started={3} kept_running={4}' -f
          $(if ($attempt.isolation_type) { $attempt.isolation_type } else {
            'isolation type left to the HCS' }),
          $(if ($attempt.guest_state_type) { $attempt.guest_state_type }
            else { 'no guest state file' }),
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

Hide-Identity $results

$zip = "$WorkDir\lg-pc-test-$stamp.zip"
Compress-Archive -Path "$results\*" -DestinationPath $zip -Force
Write-Host ''
Write-Host ('The user and machine names are hidden in the text files of the ' +
  'zip, which has no name or id of this PC''s virtual machines. Read ' +
  "$results\pc-test.log before sending it: it has this PC's Windows " +
  'version, its processor and the device path of the GPU.')
Write-Host "Send this file back: $zip"
Write-Host ("The disk stays in $WorkDir\disk for -Reuse; delete $WorkDir " +
  'to remove everything the test made')
if ($failure) {
  exit 1
}
exit $probeExit
