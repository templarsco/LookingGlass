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
and at every start, with the IVSHMEM driver next to it.
lg-windows-client-hcs-probe --windows-disk boots it. With -Logs, shows the
tool's log, the driver's installation and Windows' device events from the
disk once the guest is off. Run this elevated on Windows with Hyper-V's
PowerShell module.
#>

[CmdletBinding(DefaultParameterSetName = 'Make')]
param(
  [Parameter(Mandatory, ParameterSetName = 'Make')] [string] $Iso,
  [Parameter(Mandatory)] [string] $Disk,
  [Parameter(Mandatory, ParameterSetName = 'Make')] [string] $Tool,
  [Parameter(Mandatory, ParameterSetName = 'Make')] [string] $Inf,
  [Parameter(ParameterSetName = 'Make')] [int] $SizeGB = 40,
  [Parameter(Mandatory, ParameterSetName = 'Logs')] [switch] $Logs
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

function Show-Logs {
  $number = (Mount-VHD -Path $Disk -Passthru | Get-Disk).Number
  try {
    $partition = Get-Partition -DiskNumber $number |
      Sort-Object Size -Descending | Select-Object -First 1
    if (-not $partition.DriveLetter) {
      $partition | Add-PartitionAccessPath -AssignDriveLetter
      $partition = Get-Partition -DiskNumber $number `
        -PartitionNumber $partition.PartitionNumber
    }
    $w = "$($partition.DriveLetter):"

    foreach ($log in "$w\lgprobe\setup.log", "$w\lgprobe\lg-hyperv-ivshmem.log",
        "$w\Windows\Panther\setuperr.log") {
      Write-Host "== $log"
      if (Test-Path $log) {
        Get-Content $log -Tail 200 | Write-Host
      }
    }

    # the driver's installation, the last ones about the IVSHMEM IDs
    $devices = "$w\Windows\INF\setupapi.dev.log"
    Write-Host "== $devices"
    if (Test-Path $devices) {
      Select-String -Path $devices -Pattern 'VEN_1AF4&DEV_1110' `
          -Context 0, 40 | Select-Object -Last 3 |
        Out-String -Width 250 | Write-Host
    }

    $events = "$w\Windows\System32\winevt\Logs"
    foreach ($name in 'Microsoft-Windows-Kernel-PnP%4Configuration',
        'Microsoft-Windows-CodeIntegrity%4Operational', 'System') {
      Write-Host "== $name"
      $path = "$events\$name.evtx"
      if (-not (Test-Path $path)) {
        continue
      }
      Get-WinEvent -Path $path -MaxEvents 80 -ErrorAction SilentlyContinue |
        Where-Object { $name -ne 'System' -or $_.Level -le 3 } |
        Format-List TimeCreated, ProviderName, Id, LevelDisplayName, Message |
        Out-String -Width 250 | Write-Host
    }
  } finally {
    Dismount-VHD -Path $Disk
  }
}

$Disk = [IO.Path]::GetFullPath($Disk)
if ($Logs) {
  if (Test-Path $Disk) {
    Show-Logs
  } else {
    Write-Host "There is no $Disk"
  }
  exit 0
}

$Iso  = (Resolve-Path $Iso).Path
$Tool = (Resolve-Path $Tool).Path
$Inf  = (Resolve-Path $Inf).Path
if (Test-Path $Disk) {
  throw "$Disk exists already"
}

$image = Mount-DiskImage -ImagePath $Iso -PassThru
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
  $number = (Mount-VHD -Path $Disk -Passthru | Get-Disk).Number
  try {
    Initialize-Disk -Number $number -PartitionStyle GPT

    # the EFI system partition, made as a data partition to format it and
    # retyped at the end, Microsoft's reserved partition unless initializing
    # the disk made one, and Windows'
    $system = New-Partition -DiskNumber $number -Size 260MB -AssignDriveLetter
    Format-Volume -Partition $system -FileSystem FAT32 `
      -NewFileSystemLabel System -Confirm:$false | Out-Null
    $reserved = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'
    if (-not (Get-Partition -DiskNumber $number |
        Where-Object GptType -eq $reserved)) {
      New-Partition -DiskNumber $number -Size 16MB -GptType $reserved |
        Out-Null
    }
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
    & bcdboot.exe "$w\Windows" /s $s /f UEFI | Write-Host
    if ($LASTEXITCODE) {
      throw "bcdboot failed with $LASTEXITCODE"
    }

    # the tool and the driver, in C:\lgprobe of the guest
    $dir = "$w\lgprobe"
    New-Item -ItemType Directory "$dir\ivshmem" | Out-Null
    Copy-Item $Tool $dir
    Copy-Item "$(Split-Path $Inf)\*" "$dir\ivshmem" -Recurse
    $exe = 'C:\lgprobe\' + (Split-Path $Tool -Leaf)
    $guestInf = 'C:\lgprobe\ivshmem\' + (Split-Path $Inf -Leaf)

    # the driver's publisher, trusted so that installing it asks nothing
    $trust = $null
    $catalog = Get-ChildItem "$dir\ivshmem" -Filter *.cat |
      Select-Object -First 1
    if ($catalog) {
      $signer = (Get-AuthenticodeSignature $catalog.FullName).SignerCertificate
      if ($signer) {
        [IO.File]::WriteAllBytes("$dir\publisher.cer", $signer.Export('Cert'))
        $trust = 'certutil -addstore TrustedPublisher ' +
          'C:\lgprobe\publisher.cer >> C:\lgprobe\setup.log 2>&1'
      }
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
      "schtasks /create /f /tn $task /ru SYSTEM /rl HIGHEST /sc onstart " +
        "/tr `"$exe serial COM1 $guestInf`" >> C:\lgprobe\setup.log 2>&1"
      "schtasks /run /tn $task >> C:\lgprobe\setup.log 2>&1"
    ) | Where-Object { $_ } |
      Set-Content -Path "$scripts\SetupComplete.cmd" -Encoding ASCII

    # the firmware boots from the EFI system partition
    Set-Partition -DiskNumber $number -PartitionNumber $system.PartitionNumber `
      -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
  } finally {
    Dismount-VHD -Path $Disk
  }
} finally {
  Dismount-DiskImage -ImagePath $Iso | Out-Null
}

Get-Item $Disk | Format-List FullName, Length | Out-String | Write-Host
