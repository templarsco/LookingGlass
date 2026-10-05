/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "common/ivshmem.h"
#include "common/option.h"
#include "common/vector.h"
#include "common/windebug.h"

#include <windows.h>
#include "ivshmem.h"

#include <aclapi.h>
#include <limits.h>
#include <sddl.h>
#include <setupapi.h>
#include <io.h>

struct IVSHMEMInfo
{
  // the guest's IVSHMEM driver, or a named section on the PC running the VM
  bool   section;
  HANDLE handle;
};

void ivshmemOptionsInit(void)
{
  static struct Option options[] = {
    {
      .module         = "os",
      .name           = "shmDevice",
      .description    = "The IVSHMEM device to use",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = 0
    },
    {0}
  };

  option_register(options);
}

struct IVSHMEMData
{
  SP_DEVINFO_DATA devInfoData;
  DWORD64         busAddr;
};

static int ivshmemComparator(const void * a_, const void * b_)
{
  const struct IVSHMEMData * a = a_;
  const struct IVSHMEMData * b = b_;

  if (a->busAddr < b->busAddr)
    return -1;

  if (a->busAddr > b->busAddr)
    return 1;

  return 0;
}

bool ivshmemInit(struct IVSHMEM * dev)
{
  DEBUG_ASSERT(dev && !dev->opaque);

  HANDLE                           handle;
  HDEVINFO                         devInfoSet;
  PSP_DEVICE_INTERFACE_DETAIL_DATA infData = NULL;
  SP_DEVINFO_DATA                  devInfoData = {0};
  SP_DEVICE_INTERFACE_DATA         devInterfaceData = {0};
  Vector                           devices;

  devInfoSet = SetupDiGetClassDevs(&GUID_DEVINTERFACE_IVSHMEM, NULL, NULL,
      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  devInfoData.cbSize      = sizeof(SP_DEVINFO_DATA);
  devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  if (!vector_create(&devices, sizeof(struct IVSHMEMData), 1))
  {
    DEBUG_ERROR("Failed to allocate memory");
    return false;
  }

  for (int i = 0; SetupDiEnumDeviceInfo(devInfoSet, i, &devInfoData); ++i)
  {
    struct IVSHMEMData * device = vector_push(&devices, NULL);

    DWORD bus, addr;
    if (!SetupDiGetDeviceRegistryProperty(devInfoSet, &devInfoData,
          SPDRP_BUSNUMBER, NULL, (void*) &bus, sizeof(bus), NULL))
    {
      DEBUG_WINERROR("Failed to SetupDiGetDeviceRegistryProperty",
          GetLastError());
      bus = 0xFFFF;
    }

    if (!SetupDiGetDeviceRegistryProperty(devInfoSet, &devInfoData,
          SPDRP_ADDRESS, NULL, (void*) &addr, sizeof(addr), NULL))
    {
      DEBUG_WINERROR("Failed to SetupDiGetDeviceRegistryProperty",
          GetLastError());
      addr = 0xFFFFFFFF;
    }

    device->busAddr = (((DWORD64) bus) << 32) | addr;
    memcpy(&device->devInfoData, &devInfoData, sizeof(SP_DEVINFO_DATA));
  }

  if (GetLastError() != ERROR_NO_MORE_ITEMS)
  {
    vector_destroy(&devices);
    DEBUG_WINERROR("SetupDiEnumDeviceInfo failed", GetLastError());
    return false;
  }

  if (vector_size(&devices) == 0)
  {
    vector_destroy(&devices);
    DEBUG_ERROR("Failed to find any IVSHMEM devices, unable to continue");
    DEBUG_ERROR("Did you remember to add the device to your VM "
        "and is the driver installed?");
    return false;
  }

  const int shmDevice = option_get_int("os", "shmDevice");
  qsort(vector_data(&devices), vector_size(&devices), sizeof(struct IVSHMEMData),
      ivshmemComparator);

  struct IVSHMEMData * device;
  vector_forEachRefIdx(i, device, &devices)
  {
    DWORD bus  = device->busAddr >> 32;
    DWORD addr = device->busAddr & 0xFFFFFFFF;
    DEBUG_INFO(
      "IVSHMEM %" PRIuPTR "%c on bus 0x%lx, device 0x%lx, function 0x%lx",
      i,
      i == shmDevice ? '*' : ' ',
      bus,
      addr >> 16,
      addr & 0xFFFF);
  }

  if (shmDevice >= vector_size(&devices))
  {
    vector_destroy(&devices);
    DEBUG_ERROR("os:shmDevice %d does not exist", shmDevice);
    return false;
  }

  device = vector_ptrTo(&devices, shmDevice);
  memcpy(&devInfoData, &device->devInfoData, sizeof(SP_DEVINFO_DATA));
  vector_destroy(&devices);

  if (SetupDiEnumDeviceInterfaces(devInfoSet, &devInfoData,
        &GUID_DEVINTERFACE_IVSHMEM, 0, &devInterfaceData) == FALSE)
  {
    DEBUG_WINERROR("SetupDiEnumDeviceInterfaces failed", GetLastError());
    return false;
  }

  DWORD reqSize = 0;
  SetupDiGetDeviceInterfaceDetail(devInfoSet, &devInterfaceData,
      NULL, 0, &reqSize, NULL);
  if (!reqSize)
  {
    DEBUG_WINERROR("SetupDiGetDeviceInterfaceDetail", GetLastError());
    return false;
  }

  infData         = calloc(1, reqSize);
  infData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
  if (!SetupDiGetDeviceInterfaceDetail(devInfoSet, &devInterfaceData, infData,
        reqSize, NULL, NULL))
  {
    free(infData);
    DEBUG_WINERROR("SetupDiGetDeviceInterfaceDetail", GetLastError());
    return false;
  }

  handle = CreateFile(infData->DevicePath, 0, 0, NULL, OPEN_EXISTING, 0, 0);
  if (handle == INVALID_HANDLE_VALUE)
  {
    SetupDiDestroyDeviceInfoList(devInfoSet);
    DEBUG_WINERROR("CreateFile returned INVALID_HANDLE_VALUE", GetLastError());
    return false;
  }

  free(infData);
  SetupDiDestroyDeviceInfoList(devInfoSet);

  struct IVSHMEMInfo * info = malloc(sizeof(*info));

  info->section = false;
  info->handle  = handle;
  dev->opaque   = info;
  dev->size     = 0;
  dev->mem      = NULL;

  return true;
}

bool ivshmemOpen(struct IVSHMEM * dev)
{
  DEBUG_ASSERT(dev && dev->opaque && !dev->mem);

  struct IVSHMEMInfo * info = (struct IVSHMEMInfo *)dev->opaque;

  IVSHMEM_SIZE size;
  if (!DeviceIoControl(info->handle, IOCTL_IVSHMEM_REQUEST_SIZE, NULL, 0, &size,
        sizeof(IVSHMEM_SIZE), NULL, NULL))
  {
    DEBUG_WINERROR("DeviceIoControl Failed", GetLastError());
    return false;
  }

  IVSHMEM_MMAP_CONFIG config = { .cacheMode = IVSHMEM_CACHE_WRITECOMBINED };
  IVSHMEM_MMAP map = { 0 };
  if (!DeviceIoControl(
    info->handle,
    IOCTL_IVSHMEM_REQUEST_MMAP,
    &config, sizeof(IVSHMEM_MMAP_CONFIG),
    &map   , sizeof(IVSHMEM_MMAP),
    NULL, NULL))
  {
    DEBUG_WINERROR("DeviceIoControl Failed", GetLastError());
    return false;
  }

  dev->size   = (unsigned int)size;
  dev->mem    = map.ptr;
  return true;
}

static void * tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS type)
{
  DWORD size = 0;
  GetTokenInformation(token, type, NULL, 0, &size);

  void * info = size ? malloc(size) : NULL;
  if (!info || !GetTokenInformation(token, type, info, size, &size))
  {
    DEBUG_WINERROR("GetTokenInformation failed", GetLastError());
    free(info);
    return NULL;
  }
  return info;
}

/* This user and logon session, the system and administrators, who can open
 * any object anyway, and Hyper-V's per-VM accounts, as the VM maps the
 * section too. Anyone else could see the guest's screen and send it input. */
static bool sidTrusted(PSID sid, const TOKEN_USER * user,
    const TOKEN_GROUPS * groups)
{
  if (EqualSid(sid, user->User.Sid))
    return true;

  for (DWORD i = 0; i < groups->GroupCount; ++i)
    if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) ==
          SE_GROUP_LOGON_ID &&
        EqualSid(sid, groups->Groups[i].Sid))
      return true;

  if (IsWellKnownSid(sid, WinLocalSystemSid) ||
      IsWellKnownSid(sid, WinBuiltinAdministratorsSid) ||
      IsWellKnownSid(sid, WinCreatorOwnerRightsSid))
    return true;

  // NT VIRTUAL MACHINE, S-1-5-83-...
  static const SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
  return
    memcmp(GetSidIdentifierAuthority(sid), &ntAuthority,
        sizeof(ntAuthority)) == 0 &&
    *GetSidSubAuthorityCount(sid) >= 1 &&
    *GetSidSubAuthority(sid, 0) == SECURITY_VIRTUALSERVER_ID_BASE_RID;
}

static void sectionUntrusted(const char * name, const char * what, PSID sid)
{
  char * sidString = NULL;
  if (sid)
    ConvertSidToStringSidA(sid, &sidString);
  DEBUG_ERROR("The shared memory section %s %s %s; only this user and the VM "
      "may have access", name, what, sidString ? sidString : "another account");
  LocalFree(sidString);
}

static bool sectionIsPrivate(HANDLE handle, const char * name)
{
  bool                 result = false;
  HANDLE               token  = NULL;
  TOKEN_USER         * user   = NULL;
  TOKEN_GROUPS       * groups = NULL;
  PSID                 owner  = NULL;
  PACL                 dacl   = NULL;
  PACL                 sacl   = NULL;
  PSECURITY_DESCRIPTOR sd     = NULL;

  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
  {
    DEBUG_WINERROR("OpenProcessToken failed", GetLastError());
    goto out;
  }

  if (!(user   = tokenInfo(token, TokenUser  )) ||
      !(groups = tokenInfo(token, TokenGroups)))
    goto out;

  // the integrity label is in the SACL, which READ_CONTROL reads as far as the
  // label goes. Where that is not supported, such as under Wine, the owner and
  // the DACL are all that there is to check
  DWORD error = GetSecurityInfo(handle, SE_KERNEL_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
      LABEL_SECURITY_INFORMATION, &owner, NULL, &dacl, &sacl, &sd);
  if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED)
  {
    DEBUG_WARN("The integrity label of the shared memory section %s cannot "
        "be read here, so it is not checked", name);
    sacl = NULL;
    error = GetSecurityInfo(handle, SE_KERNEL_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, NULL, &dacl, NULL, &sd);
  }
  if (error != ERROR_SUCCESS)
  {
    DEBUG_WINERROR("Failed to read who can open the shared memory section",
        error);
    goto out;
  }

  if (!dacl)
  {
    DEBUG_ERROR("The shared memory section %s has no DACL, so every account "
        "can open it", name);
    goto out;
  }

  // the owner can always change the DACL
  if (!owner || !sidTrusted(owner, user, groups))
  {
    sectionUntrusted(name, "is owned by", owner);
    goto out;
  }

  const ACCESS_MASK access =
    SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE |
    SECTION_EXTEND_SIZE | WRITE_DAC | WRITE_OWNER |
    GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL;

  for (DWORD i = 0; i < dacl->AceCount; ++i)
  {
    ACE_HEADER * header;
    if (!GetAce(dacl, i, (void **)&header))
    {
      DEBUG_WINERROR("GetAce failed", GetLastError());
      goto out;
    }

    if (header->AceFlags & INHERIT_ONLY_ACE)
      continue;

    switch (header->AceType)
    {
      // the callback variant has the same layout
      case ACCESS_ALLOWED_ACE_TYPE:
      case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
        break;

      case ACCESS_ALLOWED_COMPOUND_ACE_TYPE:
      case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
      case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
        DEBUG_ERROR("The shared memory section %s has an access entry of an "
            "unsupported type (%u)", name, header->AceType);
        goto out;

      // denied, audit and label entries grant nothing
      default:
        continue;
    }

    const ACCESS_ALLOWED_ACE * ace = (const ACCESS_ALLOWED_ACE *)header;
    PSID sid = (PSID)&ace->SidStart;
    if ((ace->Mask & access) && !sidTrusted(sid, user, groups))
    {
      sectionUntrusted(name, "can be opened by", sid);
      goto out;
    }
  }

  // A process of low integrity, which a sandbox gives to what it runs, can make
  // a section with a name before the user's own process does. It is owned by
  // the user and only the user can open it, so the checks above pass, and the
  // client would then trust it with the user's keystrokes. What a process of
  // the user's usual integrity makes has no label, or one of medium or above
  for (DWORD i = 0; sacl && i < sacl->AceCount; ++i)
  {
    ACE_HEADER * header;
    if (!GetAce(sacl, i, (void **)&header))
    {
      DEBUG_WINERROR("GetAce failed", GetLastError());
      goto out;
    }

    if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
      continue;

    const SYSTEM_MANDATORY_LABEL_ACE * label =
      (const SYSTEM_MANDATORY_LABEL_ACE *)header;
    PSID sid = (PSID)&label->SidStart;
    const DWORD count = *GetSidSubAuthorityCount(sid);
    if (count && *GetSidSubAuthority(sid, count - 1) <
        SECURITY_MANDATORY_MEDIUM_RID)
    {
      DEBUG_ERROR("The shared memory section %s has an integrity label below "
          "medium, so a process of lower integrity than the user's usual ones "
          "made it, as a sandboxed program is", name);
      goto out;
    }
  }

  result = true;

out:
  LocalFree(sd);
  free(groups);
  free(user);
  if (token)
    CloseHandle(token);
  return result;
}

bool ivshmemOpenDev(struct IVSHMEM * dev, const char * shmDevice)
{
  DEBUG_ASSERT(dev);

  dev->opaque = NULL;
  dev->mem    = NULL;
  dev->size   = 0;

  DEBUG_INFO("Shared memory    : %s", shmDevice);

  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      shmDevice, -1, NULL, 0);
  wchar_t * name = length > 0 ? malloc(length * sizeof(*name)) : NULL;
  if (!name || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        shmDevice, -1, name, length))
  {
    DEBUG_ERROR("Invalid shared memory section name: %s", shmDevice);
    free(name);
    return false;
  }

  // READ_CONTROL lets us check who else can open the section
  HANDLE handle = OpenFileMappingW(
      FILE_MAP_READ | FILE_MAP_WRITE | READ_CONTROL, FALSE, name);
  free(name);
  if (!handle)
  {
    DEBUG_WINERROR("Failed to open the shared memory section",
        GetLastError());
    return false;
  }

  if (!sectionIsPrivate(handle, shmDevice))
  {
    CloseHandle(handle);
    return false;
  }

  uint8_t * mem = MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE,
      0, 0, 0);
  if (!mem)
  {
    DEBUG_WINERROR("Failed to map the shared memory section", GetLastError());
    CloseHandle(handle);
    return false;
  }

  // the view covers the whole section, but a section that commits memory in
  // parts reports one region per part, and reserved pages cannot be read
  size_t size      = 0;
  bool   committed = true;
  MEMORY_BASIC_INFORMATION region;
  while (VirtualQuery(mem + size, &region, sizeof(region)) == sizeof(region) &&
      region.AllocationBase == mem)
  {
    committed &= region.State == MEM_COMMIT;
    size      += region.RegionSize;
  }

  if (!committed)
  {
    DEBUG_ERROR("The shared memory section %s is not fully committed",
        shmDevice);
    UnmapViewOfFile(mem);
    CloseHandle(handle);
    return false;
  }

  if (!size || size > UINT_MAX)
  {
    DEBUG_ERROR("Unsupported shared memory section size: %" PRIuPTR, size);
    UnmapViewOfFile(mem);
    CloseHandle(handle);
    return false;
  }

  struct IVSHMEMInfo * info = malloc(sizeof(*info));
  if (!info)
  {
    DEBUG_ERROR("Failed to allocate memory");
    UnmapViewOfFile(mem);
    CloseHandle(handle);
    return false;
  }

  info->section = true;
  info->handle  = handle;

  dev->opaque = info;
  dev->mem    = mem;
  dev->size   = (unsigned int)size;
  return true;
}

void ivshmemClose(struct IVSHMEM * dev)
{
  DEBUG_ASSERT(dev);

  if (!dev->opaque)
    return;

  struct IVSHMEMInfo * info = (struct IVSHMEMInfo *)dev->opaque;

  if (info->section)
  {
    UnmapViewOfFile(dev->mem);
    CloseHandle(info->handle);
    free(info);
    dev->opaque = NULL;
    dev->size   = 0;
    dev->mem    = NULL;
    return;
  }

  DEBUG_ASSERT(dev->mem);

  if (!DeviceIoControl(info->handle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL,
        0, NULL, NULL))
    DEBUG_WINERROR("DeviceIoControl failed", GetLastError());

  dev->size = 0;
  dev->mem  = NULL;
}

void ivshmemFree(struct IVSHMEM * dev)
{
  DEBUG_ASSERT(dev);

  // a section is freed by ivshmemClose, which leaves nothing here
  if (!dev->opaque)
    return;

  DEBUG_ASSERT(!dev->mem);
  struct IVSHMEMInfo * info = (struct IVSHMEMInfo *)dev->opaque;

  free(info);
  dev->opaque = NULL;
}

bool ivshmemHasDMA(struct IVSHMEM * dev)
{
  return false;
}

int ivshmemGetDMABuf(struct IVSHMEM * dev, uint64_t offset, uint64_t size)
{
  DEBUG_ERROR("DMA buffers are only available with the Linux KVMFR module");
  return -1;
}
