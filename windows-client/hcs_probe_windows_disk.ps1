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
Makes a disk with Windows for the HCS probe's Windows guest, or shows what
the guest logged on it.

.DESCRIPTION
Applies Windows from an ISO, such as Microsoft's evaluation ISOs of Windows
Server, whose Server Core it takes, or Windows 11, to a new dynamic VHDX
that boots with UEFI. Windows sets itself up on its first boot without
asking anything, and then runs lg-hyperv-ivshmem on COM1 as SYSTEM, then
and at every start, with the IVSHMEM driver next to it. With -Idd, the
Looking Glass IDD's package goes next to it too, for the probe's --client.
With -GpuPv, the driver packages of that GPU of this PC, by the Name that
Get-VMHostPartitionableGpu shows, go to the guest's HostDriverStore, from
where a guest with a partition of the GPU loads the GPU's driver, for the
probe's --gpu. lg-windows-client-hcs-probe --windows-disk boots the disk,
which it does only for one that has the file that the script writes next to
it, DISK.lgprobe, when it has finished it.
Nothing on this PC changes but the new disk and that file. With -Logs, shows the
tool's log, the driver's installation and Windows' device events from the
disk once the guest is off. With -ListGpuPackages, only shows the driver
packages that -GpuPv would copy. Run this elevated on Windows with
Hyper-V's PowerShell module.
#>

[CmdletBinding(DefaultParameterSetName = 'Make')]
param(
  [Parameter(Mandatory, ParameterSetName = 'Make')] [string] $Iso,
  [Parameter(Mandatory, ParameterSetName = 'Make')]
  [Parameter(Mandatory, ParameterSetName = 'Logs')] [string] $Disk,
  [Parameter(Mandatory, ParameterSetName = 'Make')] [string] $Tool,
  [Parameter(Mandatory, ParameterSetName = 'Make')] [string] $Inf,
  [Parameter(ParameterSetName = 'Make')] [string] $Idd,
  [Parameter(ParameterSetName = 'Make')]
  [Parameter(Mandatory, ParameterSetName = 'Gpu')] [string] $GpuPv,
  [Parameter(ParameterSetName = 'Make')] [int] $SizeGB = 40,
  [Parameter(Mandatory, ParameterSetName = 'Logs')] [switch] $Logs,
  [Parameter(Mandatory, ParameterSetName = 'Gpu')] [switch] $ListGpuPackages
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

# whether a file of the guest's disk is there, and neither it nor a folder
# above it is a link: the guest could make one to a file of this PC, and what
# is shown goes into a log that people send back
function Test-GuestFile([string] $path) {
  $item = Get-Item -LiteralPath $path -Force -ErrorAction Ignore
  if (-not $item) {
    return $false
  }
  while ($item) {
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
      return $false
    }
    $item = if ($item -is [IO.DirectoryInfo]) { $item.Parent } else {
      $item.Directory
    }
  }
  return $true
}

function Show-Logs {
  # not read only: the guest's last writes are in the file system's log, which
  # only a read-write mount replays. The VHD is attached before the try, but
  # nothing that can fail is between them
  $mounted = Mount-VHD -Path $Disk -Passthru
  try {
    $number = ($mounted | Get-Disk).Number
    $partition = Get-Partition -DiskNumber $number |
      Sort-Object Size -Descending | Select-Object -First 1
    if (-not $partition.DriveLetter) {
      $partition | Add-PartitionAccessPath -AssignDriveLetter
      $partition = Get-Partition -DiskNumber $number `
        -PartitionNumber $partition.PartitionNumber
    }
    $w = "$($partition.DriveLetter):"

    $idd = @(Get-ChildItem "$w\ProgramData\Looking Glass (IDD)" -Filter *.txt `
      -ErrorAction SilentlyContinue | ForEach-Object FullName)
    foreach ($log in @("$w\lgprobe\setup.log",
        "$w\lgprobe\lg-hyperv-ivshmem.log",
        "$w\Windows\Panther\setuperr.log") + $idd) {
      Write-Host "== $log"
      if (Test-GuestFile $log) {
        Get-Content -LiteralPath $log -Tail 200 | Write-Host
      }
    }

    # the drivers' installation: the last entries about the IVSHMEM's and
    # the IDD's devices, and about staging and installing the IDD's packages
    $devices = "$w\Windows\INF\setupapi.dev.log"
    Write-Host "== $devices"
    if (Test-GuestFile $devices) {
      foreach ($id in 'VEN_1AF4&DEV_1110', 'Root\LGIdd', 'Root\LGInput',
          'LGIdd.inf]', 'LGInput.inf]') {
        Select-String -Path $devices -Pattern $id -SimpleMatch `
            -Context 0, 60 | Select-Object -Last 3 |
          Out-String -Width 250 | Write-Host
      }
    }

    # the packages of a GPU's driver that -GpuPv put in the HostDriverStore
    $store = "$w\Windows\System32\HostDriverStore\FileRepository"
    Write-Host "== $store"
    Get-ChildItem -LiteralPath $store -Directory -ErrorAction Ignore |
      ForEach-Object { Write-Host $_.Name }

    # Windows' device, code integrity and graphics kernel events, and the
    # errors of the System and Application logs, such as a user-mode
    # driver's host that crashed
    $events = "$w\Windows\System32\winevt\Logs"
    $names = @('Microsoft-Windows-Kernel-PnP%4Configuration',
      'Microsoft-Windows-CodeIntegrity%4Operational') +
      @(Get-ChildItem "$events\*DxgKrnl*.evtx" -ErrorAction Ignore |
        ForEach-Object BaseName) + @('System', 'Application')
    foreach ($name in $names) {
      Write-Host "== $name"
      $path = "$events\$name.evtx"
      if (-not (Test-GuestFile $path)) {
        continue
      }
      Get-WinEvent -Path $path -MaxEvents 80 -ErrorAction SilentlyContinue |
        Where-Object { $name -notin 'System', 'Application' -or
          $_.Level -le 3 } |
        Format-List TimeCreated, ProviderName, Id, LevelDisplayName, Message |
        Out-String -Width 250 | Write-Host
    }
  } finally {
    Dismount-VHD -Path $Disk
  }
}

# the driver packages that a guest with a partition of the GPU needs: the
# package of the driver's service, and those of its OpenGL libraries, which
# may be another package. Each is a folder of the DriverStore, with every
# file's size and SHA-256
function Get-GpuPackages([string] $interface) {
  # the device interface's path holds the device's instance ID
  if ($interface -notmatch '^\\\\\?\\(.+?)#\{[0-9a-fA-F-]{36}\}(\\.*)?$') {
    throw "$interface is not a device interface, such as the Name that " +
      'Get-VMHostPartitionableGpu shows'
  }
  $instance = $Matches[1] -replace '#', '\'
  $device = Get-PnpDevice -InstanceId $instance
  if ($device.Class -ne 'Display') {
    throw "$instance is not a display adapter"
  }
  Write-Host "GPU: $($device.FriendlyName), $instance"

  $repository = [IO.Path]::GetFullPath(
    "$env:SystemRoot\System32\DriverStore\FileRepository") + '\'

  # the package that a driver's file is in, which must be in the DriverStore
  function Get-Package([string] $path) {
    $path = $path.Trim('"')
    if ($path -match '^\\SystemRoot\\(.*)$') {
      $path = [IO.Path]::Combine($env:SystemRoot, $Matches[1])
    } elseif ($path -match '^\\\?\?\\(.*)$') {
      $path = $Matches[1]
    } elseif ($path -match '^System32\\') {
      $path = [IO.Path]::Combine($env:SystemRoot, $path)
    }
    if (-not [IO.Path]::IsPathRooted($path)) {
      return $null
    }
    $path = [IO.Path]::GetFullPath($path)
    if (-not $path.StartsWith($repository,
        [StringComparison]::OrdinalIgnoreCase)) {
      return $null
    }
    return $path.Substring($repository.Length).Split('\')[0]
  }

  $service = (Get-PnpDeviceProperty -InstanceId $instance `
    -KeyName DEVPKEY_Device_Service).Data
  $image = $null
  if ($service) {
    $image = (Get-ItemProperty -ErrorAction Ignore `
      "HKLM:\SYSTEM\CurrentControlSet\Services\$service").ImagePath
  }
  $main = if ($image) { Get-Package $image }
  if (-not $main) {
    throw "The driver of $instance, service '$service' with image " +
      "'$image', is not in the DriverStore"
  }
  $names = @($main)

  $key = (Get-PnpDeviceProperty -InstanceId $instance `
    -KeyName DEVPKEY_Device_Driver).Data
  $class = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$key"
  foreach ($value in 'OpenGLVendorName', 'OpenGLVendorNameWow') {
    foreach ($file in @($class.$value)) {
      if ($file) {
        $package = Get-Package $file
        Write-Host "$value $file is in $(if ($package) { $package } else {
          'no package of the DriverStore' })"
        if ($package -and $package -notin $names) {
          $names += $package
        }
      }
    }
  }

  foreach ($name in $names) {
    $source = [IO.Path]::Combine($repository, $name)
    $items = @(Get-Item -LiteralPath $source -Force) +
      @(Get-ChildItem -LiteralPath $source -Recurse -Force)
    $links = @($items | Where-Object {
      $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
    if ($links) {
      throw "$source has links: $($links.FullName -join ', ')"
    }
    $files = @($items | Where-Object { -not $_.PSIsContainer } |
      ForEach-Object {
        [pscustomobject]@{
          Path   = $_.FullName.Substring($source.Length + 1)
          Length = $_.Length
          Sha256 = (Get-FileHash -LiteralPath $_.FullName `
            -Algorithm SHA256).Hash
        }
      })

    # the package's version and its catalog's signature, for the log
    $inf = Get-ChildItem -LiteralPath $source -Filter *.inf |
      Select-Object -First 1
    $version = if ($inf) {
      Select-String -LiteralPath $inf.FullName -Pattern '^\s*DriverVer' |
        Select-Object -First 1 | ForEach-Object { $_.Line.Trim() }
    }
    $signatures = Get-ChildItem -LiteralPath $source -Filter *.cat |
      ForEach-Object {
        "$($_.Name) $((Get-AuthenticodeSignature $_.FullName).Status)"
      }
    Write-Host ("Package $name`: $($files.Count) files, " +
      "$([Math]::Round(($files | Measure-Object Length -Sum).Sum / 1MB)) " +
      "MiB, $version, $($signatures -join ', ')")

    [pscustomobject]@{ Name = $name; Source = $source; Files = $files }
  }
}

if ($ListGpuPackages) {
  $packages = @(Get-GpuPackages $GpuPv)
  Write-Host "-GpuPv would copy $($packages.Count) packages"
  exit 0
}

# before anything is mounted: this fails at the first disk call otherwise
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this in an elevated PowerShell, as it mounts disks'
}

# from the shell's folder, which an elevated shell does not start in, and not
# as a pattern, which a path with brackets would be
$Disk = $PSCmdlet.GetUnresolvedProviderPathFromPSPath($Disk)
if ($Logs) {
  if (Test-Path -LiteralPath $Disk) {
    Show-Logs
  } else {
    Write-Host "There is no $Disk"
  }
  exit 0
}

$Iso  = (Resolve-Path -LiteralPath $Iso).Path
$Tool = (Resolve-Path -LiteralPath $Tool).Path
$Inf  = (Resolve-Path -LiteralPath $Inf).Path
if ($Idd) {
  $Idd = (Resolve-Path -LiteralPath $Idd).Path
}
if (Test-Path -LiteralPath $Disk) {
  throw "$Disk exists already"
}

# the guest runs them by a command line that quotes nothing, and the driver's
# folder goes to the guest as it is
foreach ($leaf in (Split-Path $Tool -Leaf), (Split-Path $Inf -Leaf)) {
  if ($leaf -notmatch '^[A-Za-z0-9._-]+$') {
    throw "$leaf has a space or another character that is not a letter, " +
      'a digit, a dot, an underscore or a hyphen'
  }
}
if ([IO.Path]::GetPathRoot($Inf) -eq (Split-Path $Inf)) {
  throw "$Inf is in the root of a drive: put the driver in a folder of its own"
}
$gpu = @()
if ($GpuPv) {
  $gpu = @(Get-GpuPackages $GpuPv)
}

# an ISO that was mounted already is left mounted
$isoWasMounted = (Get-DiskImage -ImagePath $Iso).Attached
$image = Mount-DiskImage -ImagePath $Iso -PassThru
$made  = $false
$done  = $false
try {
  $source = ($image | Get-Volume).DriveLetter + ':'
  $wim = "$source\sources\install.wim"
  if (-not (Test-Path $wim)) {
    $wim = "$source\sources\install.esd"
  }

  # the first image without a desktop, which is Server Core in Windows
  # Server's ISO, or the first edition in a Windows 11 one
  $info = & dism.exe /English /Get-WimInfo "/WimFile:$wim"
  if ($LASTEXITCODE) {
    throw "dism /Get-WimInfo failed with $LASTEXITCODE"
  }
  $info | Write-Host
  $images = @()
  foreach ($line in $info) {
    if ($line -match '^Index : (\d+)') {
      $images += [pscustomobject]@{ Index = [int]$Matches[1]; Name = '' }
    } elseif ($line -match '^Name : (.+)$' -and $images) {
      $images[-1].Name = $Matches[1]
    }
  }
  $chosen = $images | Where-Object Name -notmatch 'Desktop' |
    Select-Object -First 1
  if (-not $chosen) {
    throw "$wim has no image to apply"
  }
  Write-Host "Applying image $($chosen.Index), $($chosen.Name)"

  New-Item -ItemType Directory -Force (Split-Path $Disk) | Out-Null
  New-VHD -Path $Disk -SizeBytes ($SizeGB * 1GB) -Dynamic | Out-Null
  $made = $true
  $mounted = Mount-VHD -Path $Disk -Passthru
  try {
    $number = ($mounted | Get-Disk).Number

    # the number is only the number that the disk has now: a VHD that is
    # detached hands it to another disk, and these calls format what they are
    # given. So each one is for the VHD's disk, or it does not happen
    function Assert-VhdDisk {
      $vhd = Get-VHD -Path $Disk
      if ($vhd.DiskNumber -ne $number) {
        throw "Disk $number is not $Disk"
      }
      $disk = Get-Disk -Number $number
      if ($disk.IsBoot -or $disk.IsSystem) {
        throw "Disk $number is a boot or system disk of this PC, not $Disk"
      }
    }

    Assert-VhdDisk
    Initialize-Disk -Number $number -PartitionStyle GPT

    # the EFI system partition, made as a data partition to format it and
    # retyped at the end, Microsoft's reserved partition unless initializing
    # the disk made one, and Windows'
    Assert-VhdDisk
    $system = New-Partition -DiskNumber $number -Size 260MB -AssignDriveLetter
    Format-Volume -Partition $system -FileSystem FAT32 `
      -NewFileSystemLabel System -Confirm:$false | Out-Null
    $reserved = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'
    Assert-VhdDisk
    if (-not (Get-Partition -DiskNumber $number |
        Where-Object GptType -eq $reserved)) {
      New-Partition -DiskNumber $number -Size 16MB -GptType $reserved |
        Out-Null
    }
    Assert-VhdDisk
    $windows = New-Partition -DiskNumber $number -UseMaximumSize `
      -AssignDriveLetter
    Format-Volume -Partition $windows -FileSystem NTFS `
      -NewFileSystemLabel Windows -Confirm:$false | Out-Null

    $s = (Get-Partition -DiskNumber $number `
      -PartitionNumber $system.PartitionNumber).DriveLetter + ':'
    $w = (Get-Partition -DiskNumber $number `
      -PartitionNumber $windows.PartitionNumber).DriveLetter + ':'

    & dism.exe /English /Apply-Image "/ImageFile:$wim" `
      "/Index:$($chosen.Index)" "/ApplyDir:$w\" | Select-Object -Last 3 |
      Write-Host
    if ($LASTEXITCODE) {
      throw "dism /Apply-Image failed with $LASTEXITCODE"
    }
    # bcdboot may also update the boot entries in the firmware of this PC for
    # a UEFI disk, which this disk is not for: /nofirmwaresync keeps it to the
    # files of the VHD's system partition
    & bcdboot.exe "$w\Windows" /s $s /f UEFI /nofirmwaresync | Write-Host
    if ($LASTEXITCODE) {
      throw "bcdboot failed with $LASTEXITCODE"
    }

    # the tool and the driver, in C:\lgprobe of the guest. The guest runs the
    # tool, and installs the drivers, as SYSTEM, and a folder made in the root
    # of C: lets every user of the guest change what is in it, so only SYSTEM
    # and the Administrators may. This is before anything is put in it, which
    # is how the files get the same
    $dir = "$w\lgprobe"
    New-Item -ItemType Directory $dir | Out-Null
    & icacls.exe $dir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' `
      '*S-1-5-32-544:(OI)(CI)F' | Out-Null
    if ($LASTEXITCODE) {
      throw "icacls failed with $LASTEXITCODE"
    }
    New-Item -ItemType Directory "$dir\ivshmem" | Out-Null
    Copy-Item $Tool $dir
    Copy-Item "$(Split-Path $Inf)\*" "$dir\ivshmem" -Recurse
    $exe = 'C:\lgprobe\' + (Split-Path $Tool -Leaf)
    $guestInf = 'C:\lgprobe\ivshmem\' + (Split-Path $Inf -Leaf)

    # the driver's publisher, trusted so that installing it asks nothing.
    # certutil needs -f for Trusted Publishers, which a new installation
    # does not have yet
    $trust = $null
    $catalog = Get-ChildItem "$dir\ivshmem" -Filter *.cat |
      Select-Object -First 1
    if ($catalog) {
      $signer = (Get-AuthenticodeSignature $catalog.FullName).SignerCertificate
      if ($signer) {
        [IO.File]::WriteAllBytes("$dir\publisher.cer", $signer.Export('Cert'))
        $trust = 'certutil -f -addstore TrustedPublisher ' +
          'C:\lgprobe\publisher.cer >> C:\lgprobe\setup.log 2>&1'
      }
    }

    # the Looking Glass IDD, which the tool installs when the probe asks. CI
    # signs it with the WDK's test certificate, which the guest trusts so
    # that Windows installs its user-mode drivers without asking
    $iddTrust = @()
    if ($Idd) {
      New-Item -ItemType Directory "$dir\idd" | Out-Null
      Copy-Item "$Idd\*" "$dir\idd" -Recurse
      $iddSigners = @(Get-ChildItem "$dir\idd" -Filter *.cat |
        ForEach-Object {
          (Get-AuthenticodeSignature $_.FullName).SignerCertificate
        } | Where-Object { $_ } | Sort-Object Thumbprint -Unique)
      if (-not $iddSigners) {
        throw "$Idd has no signed catalog"
      }
      # each certificate in a file named by its thumbprint
      $iddTrust = foreach ($iddSigner in $iddSigners) {
        $cer = "idd-$($iddSigner.Thumbprint).cer"
        [IO.File]::WriteAllBytes("$dir\$cer", $iddSigner.Export('Cert'))
        Write-Host "The IDD is signed by $($iddSigner.Subject)"
        foreach ($store in 'Root', 'TrustedPublisher') {
          "certutil -f -addstore $store C:\lgprobe\$cer >> " +
            'C:\lgprobe\setup.log 2>&1'
        }
      }
    }

    # the GPU's driver packages, where a guest with a partition of the GPU
    # looks for them; every copy must match the package on this PC
    if ($gpu) {
      $store = "$w\Windows\System32\HostDriverStore\FileRepository"
      foreach ($package in $gpu) {
        $target = [IO.Path]::Combine($store, $package.Name)
        foreach ($file in $package.Files) {
          $to = [IO.Path]::Combine($target, $file.Path)
          New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
          Copy-Item -LiteralPath ([IO.Path]::Combine($package.Source,
            $file.Path)) -Destination $to
        }
        foreach ($file in $package.Files) {
          $to = [IO.Path]::Combine($target, $file.Path)
          if ((Get-Item -LiteralPath $to).Length -ne $file.Length -or
              (Get-FileHash -LiteralPath $to -Algorithm SHA256).Hash -ne
                $file.Sha256) {
            throw "$to does not match $($file.Path) of $($package.Name)"
          }
        }
        Write-Host ("Copied $($package.Name) to the guest's HostDriverStore, " +
          "$($package.Files.Count) files that match")
      }
      $gpu | Select-Object Name, Files | ConvertTo-Json -Depth 4 |
        Set-Content -Path "$dir\gpu-packages.json" -Encoding UTF8
    }

    # Windows sets itself up without asking anything, with passwords that
    # nobody needs; Windows 11 also wants an account of its own, and its
    # online and privacy pages hidden
    $bytes = New-Object byte[] 18
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $password = [Convert]::ToBase64String($bytes) + 'a1!'
    $component = 'processorArchitecture="amd64" ' +
      'publicKeyToken="31bf3856ad364e35" language="neutral" ' +
      'versionScope="nonSxS"'
    $oobe = '<HideEULAPage>true</HideEULAPage>'
    $account = ''
    if ($chosen.Name -notmatch 'Server') {
      $oobe += '<HideLocalAccountScreen>true</HideLocalAccountScreen>' +
        '<HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>' +
        '<HideOnlineAccountScreens>true</HideOnlineAccountScreens>' +
        '<HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>' +
        '<ProtectYourPC>3</ProtectYourPC>'
      $account = '<LocalAccounts><LocalAccount wcm:action="add">' +
        '<Name>lgprobe</Name><Group>Administrators</Group>' +
        "<Password><Value>$password</Value><PlainText>true</PlainText>" +
        '</Password></LocalAccount></LocalAccounts>'
    }
    New-Item -ItemType Directory -Force "$w\Windows\Panther" | Out-Null
    @"
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend"
    xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-International-Core" $component>
      <InputLocale>en-US</InputLocale>
      <SystemLocale>en-US</SystemLocale>
      <UILanguage>en-US</UILanguage>
      <UserLocale>en-US</UserLocale>
    </component>
    <component name="Microsoft-Windows-Shell-Setup" $component>
      <OOBE>$oobe</OOBE>
      <UserAccounts>
        <AdministratorPassword>
          <Value>$password</Value>
          <PlainText>true</PlainText>
        </AdministratorPassword>
        $account
      </UserAccounts>
    </component>
  </settings>
</unattend>
"@ | Set-Content -Path "$w\Windows\Panther\unattend.xml" -Encoding UTF8

    # then it starts the tool as SYSTEM, now and at every start
    $scripts = "$w\Windows\Setup\Scripts"
    New-Item -ItemType Directory -Force $scripts | Out-Null
    $task = 'LookingGlassHcsProbe'
    @(
      '@echo off'
      $trust
      $iddTrust
      "schtasks /create /f /tn $task /ru SYSTEM /rl HIGHEST /sc onstart " +
        "/tr `"$exe serial COM1 $guestInf`" >> C:\lgprobe\setup.log 2>&1"
      "schtasks /run /tn $task >> C:\lgprobe\setup.log 2>&1"
    ) | Where-Object { $_ } |
      Set-Content -Path "$scripts\SetupComplete.cmd" -Encoding ASCII

    # the firmware boots from the EFI system partition. This changes a
    # partition's type, so first make sure that the disk is still the VHD's
    Assert-VhdDisk
    Set-Partition -DiskNumber $number -PartitionNumber $system.PartitionNumber `
      -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
    $done = $true
  } finally {
    Dismount-VHD -Path $Disk
  }
} finally {
  if (-not $isoWasMounted) {
    Dismount-DiskImage -ImagePath $Iso | Out-Null
  }

  # a disk that was not finished is not a guest, and would be booted as one
  # the next time. This made it, and it did not exist before
  if ($made -and -not $done) {
    Remove-Item -LiteralPath $Disk -Force -ErrorAction Ignore
  }
}

# says that this script made the disk and finished it, which the probe asks
# before it boots a disk that its guest writes to
Set-Content -LiteralPath "$Disk.lgprobe" -Encoding ASCII -Value @(
  'made by hcs_probe_windows_disk.ps1',
  "on $([DateTime]::UtcNow.ToString('o'))")

Get-Item -LiteralPath $Disk | Format-List FullName, Length | Out-String |
  Write-Host
