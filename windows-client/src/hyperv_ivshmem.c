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

/*
 * Gives a Windows guest of Hyper-V an IVSHMEM device over the memory that the
 * PC shares with the VM through the Host Compute Service's SharedMemory
 * device, so that the IVSHMEM driver and the Looking Glass host run unchanged,
 * as they do under QEMU.
 *
 * The IVSHMEM driver never looks at the PCI bus: it takes its device's first
 * memory resource, 256 bytes, as the registers of QEMU's ivshmem-plain, and
 * the next as the shared memory. So this makes a root-enumerated device with
 * the IVSHMEM hardware IDs and a forced configuration of two ranges of the
 * region: a page that the PC keeps zeroed as the registers, which reads as an
 * ivshmem-plain without interrupts that is peer 0, and the shared memory.
 *
 * The HCS puts the region right after the VM's memory, and the PC reads where
 * from the VM's properties. This takes the addresses on its command line, or
 * from the HCS probe over a serial port, which is how the probe checks it.
 */

#include <windows.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>

#define COBJMACROS
#include <initguid.h>
#include <devguid.h>
#include <dxgi.h>
#include <d3d11.h>

#include "ivshmem.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#define PAGE_SIZE      4096
#define GOLDEN         0x9E3779B97F4A7C15ULL
#define REGISTERS_SIZE 256

// the most that x64 can address
#define PHYSICAL_LIMIT (UINT64_C(1) << 52)

// the IDs that Windows gives QEMU's ivshmem-plain on a PCI bus, which the
// IVSHMEM driver's INF matches
static const WCHAR hardwareIds[] =
  L"PCI\\VEN_1AF4&DEV_1110&SUBSYS_11001AF4&REV_01\0"
  L"PCI\\VEN_1AF4&DEV_1110&SUBSYS_11001AF4\0"
  L"PCI\\VEN_1AF4&DEV_1110&CC_050000\0"
  L"PCI\\VEN_1AF4&DEV_1110&CC_0500\0";

static const WCHAR compatibleIds[] =
  L"PCI\\VEN_1AF4&DEV_1110&REV_01\0"
  L"PCI\\VEN_1AF4&DEV_1110\0"
  L"PCI\\VEN_1AF4&CC_050000\0"
  L"PCI\\VEN_1AF4&CC_0500\0";

// where lines go: the console, or the serial port and the log
static HANDLE port;
static FILE * logFile;

static void emit(const char * fmt, ...)
  __attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)));

static void emit(const char * fmt, ...)
{
  char line[1024];
  int  len = 0;
  if (port)
    len = snprintf(line, sizeof(line), "LGSHM ");

  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(line + len, sizeof(line) - len - 1, fmt, ap);
  va_end(ap);
  len = n < 0 ? len : min(len + n, (int)sizeof(line) - 2);
  line[len++] = '\n';

  if (port)
  {
    DWORD written;
    WriteFile(port, line, len, &written, NULL);
  }
  else
    fwrite(line, 1, len, stdout);

  if (logFile)
  {
    fwrite(line, 1, len, logFile);
    fflush(logFile);
  }
}

// a failed step, with the error of the call that failed
static bool failed(const char * what, const char * step, unsigned long error)
{
  emit("%s failed step=%s error=0x%lx", what, step, error);
  return false;
}

// a number that fits in 64 bits. strtoull takes a minus sign and gives the
// negative as a very large number, which a size must not be
static bool parseNumber(const char ** text, uint64_t * value)
{
  while (isspace((unsigned char)**text))
    ++*text;

  if (**text == '-')
    return false;

  char * end;
  errno = 0;
  *value = strtoull(*text, &end, 0);
  if (end == *text || errno == ERANGE || (*end && *end != ' '))
    return false;
  *text = end;
  return true;
}

static bool parseWide(const WCHAR * text, uint64_t * value)
{
  while (iswspace(*text))
    ++text;

  if (*text == L'-')
    return false;

  WCHAR * end;
  errno = 0;
  *value = _wcstoui64(text, &end, 0);
  return end != text && !*end && errno != ERANGE;
}

/* The memory ranges in a resource list that Windows keeps under
 * HARDWARE\RESOURCEMAP, such as the RAM it uses. The list is a
 * CM_RESOURCE_LIST, which only the driver kit headers define: a count of full
 * descriptors, each an interface type, a bus number, a version, a revision
 * and a count of partial descriptors of 20 bytes on x64. */
struct Range
{
  uint64_t start, length;
};

#define MAX_RANGES 64

static size_t parseResourceList(const BYTE * data, size_t size,
    struct Range * ranges, size_t max)
{
  const BYTE * p   = data;
  const BYTE * end = data + size;
  size_t count = 0;
  if (size < 4)
    return 0;

  ULONG lists;
  memcpy(&lists, p, 4);
  p += 4;
  for(ULONG l = 0; l < lists && p + 16 <= end; ++l)
  {
    ULONG partials;
    memcpy(&partials, p + 12, 4);
    p += 16;

    for(ULONG i = 0; i < partials && p + 20 <= end; ++i)
    {
      const UCHAR type = p[0];
      USHORT   flags;
      uint64_t start;
      ULONG    length;
      memcpy(&flags , p + 2 , 2);
      memcpy(&start , p + 4 , 8);
      memcpy(&length, p + 12, 4);
      p += 20;

      // CmResourceTypeDeviceSpecific, whose data follows the descriptor
      if (type == 5)
      {
        ULONG dataSize;
        memcpy(&dataSize, p - 16, 4);
        p += dataSize;
        continue;
      }

      // CmResourceTypeMemory and CmResourceTypeMemoryLarge, whose length is
      // shifted by 8, 16 or 32 bits with CM_RESOURCE_MEMORY_LARGE_40, 48, 64
      if ((type != 3 && type != 7) || count == max)
        continue;
      uint64_t len = length;
      if (flags & 0x200)
        len <<= 8;
      else if (flags & 0x400)
        len <<= 16;
      else if (flags & 0x800)
        len <<= 32;
      ranges[count++] = (struct Range){ start, len };
    }
  }
  return count;
}

static size_t resourceMap(const WCHAR * key, const WCHAR * value,
    struct Range * ranges, size_t max)
{
  HKEY hkey;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hkey) !=
      ERROR_SUCCESS)
    return 0;

  DWORD type, size = 0;
  BYTE * data = NULL;
  if (RegQueryValueExW(hkey, value, NULL, &type, NULL, &size) ==
        ERROR_SUCCESS && type == REG_RESOURCE_LIST && size >= 4)
  {
    data = malloc(size);
    if (data && RegQueryValueExW(hkey, value, NULL, &type, data, &size) !=
        ERROR_SUCCESS)
    {
      free(data);
      data = NULL;
    }
  }
  RegCloseKey(hkey);
  if (!data)
    return 0;

  const size_t count = parseResourceList(data, size, ranges, max);
  free(data);
  return count;
}

#define RESOURCEMAP L"HARDWARE\\RESOURCEMAP\\System Resources\\"

static size_t ramRanges(struct Range * ranges, size_t max)
{
  return resourceMap(RESOURCEMAP L"Physical Memory", L".Translated", ranges,
      max);
}

// where a range ends, which is the top of the address space for one that runs
// past it, as the sum of a start and a very large length would otherwise wrap
// to a small number
static uint64_t rangeEnd(uint64_t start, uint64_t length)
{
  uint64_t end;
  return __builtin_add_overflow(start, length, &end) ? UINT64_MAX : end;
}

static bool overlaps(const struct Range * ranges, size_t count,
    uint64_t start, uint64_t length)
{
  const uint64_t end = rangeEnd(start, length);
  for(size_t i = 0; i < count; ++i)
    if (start < rangeEnd(ranges[i].start, ranges[i].length) &&
        ranges[i].start < end)
      return true;
  return false;
}

// the device this made before, which stays across restarts
struct Device
{
  HDEVINFO        set;
  SP_DEVINFO_DATA data;
};

static void deviceClose(struct Device * d)
{
  if (d->set != INVALID_HANDLE_VALUE && d->set)
    SetupDiDestroyDeviceInfoList(d->set);
  d->set = INVALID_HANDLE_VALUE;
}

// the root-enumerated device whose first hardware ID is id
static bool deviceFindId(struct Device * d, const WCHAR * id)
{
  d->set = SetupDiGetClassDevsW(NULL, L"ROOT", NULL, DIGCF_ALLCLASSES);
  if (d->set == INVALID_HANDLE_VALUE)
    return false;

  for(DWORD i = 0;; ++i)
  {
    d->data = (SP_DEVINFO_DATA){ .cbSize = sizeof(d->data) };
    if (!SetupDiEnumDeviceInfo(d->set, i, &d->data))
      break;

    WCHAR ids[1024] = { 0 };
    DWORD type;
    if (SetupDiGetDeviceRegistryPropertyW(d->set, &d->data, SPDRP_HARDWAREID,
          &type, (BYTE *)ids, sizeof(ids) - 2 * sizeof(WCHAR), NULL) &&
        type == REG_MULTI_SZ && _wcsicmp(ids, id) == 0)
      return true;
  }

  deviceClose(d);
  return false;
}

static bool deviceFind(struct Device * d)
{
  return deviceFindId(d, hardwareIds);
}

// a root-enumerated device of the INF's class, as devcon install makes one
static bool deviceCreate(struct Device * d, const WCHAR * inf)
{
  GUID  classGuid;
  WCHAR className[MAX_CLASS_NAME_LEN];
  if (!SetupDiGetINFClassW(inf, &classGuid, className, MAX_CLASS_NAME_LEN,
        NULL))
    return failed("device", "inf class", GetLastError());

  d->set = SetupDiCreateDeviceInfoList(&classGuid, NULL);
  if (d->set == INVALID_HANDLE_VALUE)
    return failed("device", "info list", GetLastError());

  d->data = (SP_DEVINFO_DATA){ .cbSize = sizeof(d->data) };
  if (!SetupDiCreateDeviceInfoW(d->set, className, &classGuid,
        L"IVSHMEM over Hyper-V shared memory", NULL, DICD_GENERATE_ID,
        &d->data))
    return failed("device", "create", GetLastError());

  if (!SetupDiSetDeviceRegistryPropertyW(d->set, &d->data, SPDRP_HARDWAREID,
        (const BYTE *)hardwareIds, sizeof(hardwareIds)) ||
      !SetupDiSetDeviceRegistryPropertyW(d->set, &d->data,
        SPDRP_COMPATIBLEIDS, (const BYTE *)compatibleIds,
        sizeof(compatibleIds)))
    return failed("device", "ids", GetLastError());

  if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, d->set, &d->data))
    return failed("device", "register", GetLastError());
  return true;
}

static void deviceInstance(struct Device * d, char * text, size_t size)
{
  WCHAR id[MAX_DEVICE_ID_LEN];
  if (CM_Get_Device_IDW(d->data.DevInst, id, MAX_DEVICE_ID_LEN, 0) !=
      CR_SUCCESS)
    wcscpy(id, L"unknown");
  snprintf(text, size, "%ls", id);
}

static bool addMemory(LOG_CONF config, uint64_t start, uint64_t length,
    DWORD flags)
{
  MEM_RESOURCE mem = { 0 };
  mem.MEM_Header.MD_Count      = 0;
  mem.MEM_Header.MD_Type       = MType_Range;
  mem.MEM_Header.MD_Alloc_Base = start;
  mem.MEM_Header.MD_Alloc_End  = start + length - 1;
  mem.MEM_Header.MD_Flags      = flags;

  RES_DES des;
  const CONFIGRET cr = CM_Add_Res_Des(&des, config, ResType_Mem, &mem,
      sizeof(mem), 0);
  if (cr != CR_SUCCESS)
    return failed("config", "memory", cr);
  CM_Free_Res_Des_Handle(des);
  return true;
}

// replaces the device's forced configuration: the registers, then the memory
static bool deviceConfigure(struct Device * d, uint64_t registers,
    uint64_t memory, uint64_t size)
{
  LOG_CONF config;
  while (CM_Get_First_Log_Conf(&config, d->data.DevInst, FORCED_LOG_CONF) ==
      CR_SUCCESS)
  {
    const CONFIGRET cr = CM_Free_Log_Conf(config, 0);
    CM_Free_Log_Conf_Handle(config);
    if (cr != CR_SUCCESS)
      return failed("config", "free", cr);
  }

  const CONFIGRET cr = CM_Add_Empty_Log_Conf(&config, d->data.DevInst,
      LCPRI_FORCECONFIG, FORCED_LOG_CONF | PRIORITY_EQUAL_LAST);
  if (cr != CR_SUCCESS)
    return failed("config", "add", cr);

  // as QEMU has them: BAR0 non-prefetchable, BAR2 prefetchable
  const bool ok =
    addMemory(config, registers, REGISTERS_SIZE, fMD_RAM | fMD_32) &&
    addMemory(config, memory, size, fMD_RAM | fMD_32 | fMD_PrefetchAllowed);
  CM_Free_Log_Conf_Handle(config);
  return ok;
}

static void deviceResources(struct Device * d, ULONG type, const char * name)
{
  LOG_CONF config;
  if (CM_Get_First_Log_Conf(&config, d->data.DevInst, type) != CR_SUCCESS)
  {
    emit("res %s none", name);
    return;
  }

  RES_DES des = config;
  RESOURCEID id;
  bool first = true;
  for(;;)
  {
    RES_DES next;
    if (CM_Get_Next_Res_Des(&next, des, ResType_All, &id, 0) != CR_SUCCESS)
      break;
    if (!first)
      CM_Free_Res_Des_Handle(des);
    first = false;
    des   = next;

    if (id == ResType_Mem)
    {
      MEM_RESOURCE mem;
      if (CM_Get_Res_Des_Data(des, &mem, sizeof(mem), 0) == CR_SUCCESS)
        emit("res %s mem 0x%" PRIx64 "-0x%" PRIx64 " flags=0x%lx", name,
            (uint64_t)mem.MEM_Header.MD_Alloc_Base,
            (uint64_t)mem.MEM_Header.MD_Alloc_End,
            (unsigned long)mem.MEM_Header.MD_Flags);
    }
    else
      emit("res %s type=0x%lx", name, (unsigned long)id);
  }
  if (!first)
    CM_Free_Res_Des_Handle(des);
  CM_Free_Log_Conf_Handle(config);
}

static bool deviceStarted(struct Device * d, ULONG * status, ULONG * problem)
{
  *status = *problem = 0;
  // CM_PROB_* problems stay put until the device is restarted
  for(int i = 0; i < 60; ++i)
  {
    CM_Get_DevNode_Status(status, problem, d->data.DevInst, 0);
    if ((*status & DN_STARTED) || (*status & DN_HAS_PROBLEM))
      break;
    Sleep(500);
  }
  return (*status & DN_STARTED) && !(*status & DN_HAS_PROBLEM);
}

/* Why the registers and the memory cannot be a device's, or NULL if they can.
 * They must be whole pages, in what x64 can address without running past the
 * top of it, as a size that is a negative number does, and not into each
 * other: that is "args". And they must not be RAM that Windows uses: a device
 * there would hand the IVSHMEM driver's users Windows' own memory. */
static const char * rangesProblem(const struct Range * ram, size_t count,
    uint64_t registers, uint64_t memory, uint64_t size)
{
  const uint64_t registersEnd = rangeEnd(registers, REGISTERS_SIZE);
  const uint64_t memoryEnd    = rangeEnd(memory, size);
  if (!size || size % PAGE_SIZE || memory % PAGE_SIZE ||
      registers % PAGE_SIZE ||
      registersEnd > PHYSICAL_LIMIT || memoryEnd > PHYSICAL_LIMIT ||
      (registers < memoryEnd && memory < registersEnd))
    return "args";

  if (overlaps(ram, count, registers, REGISTERS_SIZE) ||
      overlaps(ram, count, memory, size))
    return "ram";

  return NULL;
}

// checks the ranges against this PC's RAM, and says why not, for what, which
// is a verb, that failed
static bool checkRanges(const char * what, uint64_t registers,
    uint64_t memory, uint64_t size)
{
  struct Range ram[MAX_RANGES];
  const size_t count = ramRanges(ram, MAX_RANGES);
  if (!count)
    return failed(what, "ram", GetLastError());

  const char * problem = rangesProblem(ram, count, registers, memory, size);
  if (!problem)
    return true;

  if (strcmp(problem, "ram") == 0)
    emit("%s failed step=ram error=0x0 the ranges are RAM that Windows uses",
        what);
  else
    emit("%s failed step=args error=0x0", what);
  return false;
}

/* Makes or updates the device, installs the IVSHMEM driver on it and waits
 * for Windows to start it. */
static bool install(uint64_t registers, uint64_t memory, uint64_t size,
    const WCHAR * inf)
{
  if (!checkRanges("install", registers, memory, size))
    return false;

  struct Device d = { .set = INVALID_HANDLE_VALUE };
  bool created = false;
  if (!deviceFind(&d))
  {
    if (!deviceCreate(&d, inf))
    {
      deviceClose(&d);
      return false;
    }
    created = true;
  }

  char instance[MAX_DEVICE_ID_LEN * 3];
  deviceInstance(&d, instance, sizeof(instance));
  emit("device instance=%s created=%d", instance, created);

  if (!deviceConfigure(&d, registers, memory, size))
  {
    deviceClose(&d);
    return false;
  }

  // installs the driver on every device with the ID and restarts them, which
  // applies the new configuration
  BOOL reboot = FALSE;
  if (!UpdateDriverForPlugAndPlayDevicesW(NULL, hardwareIds, inf,
        INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &reboot))
  {
    const DWORD error = GetLastError();
    deviceResources(&d, FORCED_LOG_CONF, "forced");
    deviceClose(&d);
    return failed("install", "driver", error);
  }

  ULONG status, problem;
  const bool started = deviceStarted(&d, &status, &problem);
  emit("device status=0x%lx problem=0x%lx started=%d reboot=%d",
      (unsigned long)status, (unsigned long)problem, started, reboot);
  deviceResources(&d, FORCED_LOG_CONF, "forced");
  deviceResources(&d, ALLOC_LOG_CONF , "allocated");
  deviceClose(&d);

  if (!started)
  {
    emit("install failed step=start error=0x%lx", (unsigned long)problem);
    return false;
  }
  return true;
}

static bool removeDevice(void)
{
  struct Device d = { .set = INVALID_HANDLE_VALUE };
  if (!deviceFind(&d))
  {
    emit("remove ok none");
    return true;
  }

  const bool removed = SetupDiCallClassInstaller(DIF_REMOVE, d.set, &d.data);
  const DWORD error  = GetLastError();
  deviceClose(&d);
  if (!removed)
    return failed("remove", "remove", error);
  emit("remove ok");
  return true;
}

static void status(void)
{
  struct Device d = { .set = INVALID_HANDLE_VALUE };
  if (!deviceFind(&d))
  {
    emit("status none");
    return;
  }

  char instance[MAX_DEVICE_ID_LEN * 3];
  deviceInstance(&d, instance, sizeof(instance));
  ULONG st = 0, problem = 0;
  CM_Get_DevNode_Status(&st, &problem, d.data.DevInst, 0);
  emit("status instance=%s status=0x%lx problem=0x%lx", instance,
      (unsigned long)st, (unsigned long)problem);
  deviceResources(&d, FORCED_LOG_CONF, "forced");
  deviceResources(&d, ALLOC_LOG_CONF , "allocated");
  deviceClose(&d);
}

// the IVSHMEM driver's mapping of the memory, as the Looking Glass host
// requests it
static HANDLE              ivshmem = INVALID_HANDLE_VALUE;
static volatile uint8_t  * map;
static uint64_t            mapSize;

static void unmapIvshmem(void);

static bool mapIvshmem(uint16_t * peer)
{
  // a mapping from an earlier call is given up, not lost
  unmapIvshmem();

  HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_IVSHMEM, NULL, NULL,
      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE)
    return failed("map", "devices", GetLastError());

  SP_DEVICE_INTERFACE_DATA iface = { .cbSize = sizeof(iface) };
  if (!SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_IVSHMEM, 0,
        &iface))
  {
    const DWORD error = GetLastError();
    SetupDiDestroyDeviceInfoList(set);
    return failed("map", "interface", error);
  }

  DWORD size = 0;
  SetupDiGetDeviceInterfaceDetailW(set, &iface, NULL, 0, &size, NULL);
  SP_DEVICE_INTERFACE_DETAIL_DATA_W * detail = size ? calloc(1, size) : NULL;
  if (!detail)
  {
    SetupDiDestroyDeviceInfoList(set);
    return failed("map", "detail size", GetLastError());
  }
  detail->cbSize = sizeof(*detail);
  if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, NULL,
        NULL))
  {
    const DWORD error = GetLastError();
    free(detail);
    SetupDiDestroyDeviceInfoList(set);
    return failed("map", "detail", error);
  }

  ivshmem = CreateFileW(detail->DevicePath, 0, 0, NULL, OPEN_EXISTING, 0,
      NULL);
  const DWORD error = GetLastError();
  free(detail);
  SetupDiDestroyDeviceInfoList(set);
  if (ivshmem == INVALID_HANDLE_VALUE)
    return failed("map", "open", error);

  IVSHMEM_SIZE total;
  if (!DeviceIoControl(ivshmem, IOCTL_IVSHMEM_REQUEST_SIZE, NULL, 0, &total,
        sizeof(total), NULL, NULL))
  {
    const DWORD sizeError = GetLastError();
    unmapIvshmem();
    return failed("map", "size", sizeError);
  }

  IVSHMEM_MMAP_CONFIG config = { .cacheMode = IVSHMEM_CACHE_WRITECOMBINED };
  IVSHMEM_MMAP mmap = { 0 };
  if (!DeviceIoControl(ivshmem, IOCTL_IVSHMEM_REQUEST_MMAP, &config,
        sizeof(config), &mmap, sizeof(mmap), NULL, NULL))
  {
    const DWORD mmapError = GetLastError();
    unmapIvshmem();
    return failed("map", "mmap", mmapError);
  }

  map     = mmap.ptr;
  mapSize = total;
  *peer   = mmap.peerID;
  return true;
}

static void unmapIvshmem(void)
{
  if (ivshmem == INVALID_HANDLE_VALUE)
    return;
  if (map)
    DeviceIoControl(ivshmem, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0,
        NULL, NULL);
  CloseHandle(ivshmem);
  ivshmem = INVALID_HANDLE_VALUE;
  map     = NULL;
  mapSize = 0;
}

// the RAM that Windows uses, and what its loader kept aside
static void memory(void)
{
  struct Range ranges[MAX_RANGES];
  size_t count = ramRanges(ranges, MAX_RANGES);
  uint64_t total = 0;
  for(size_t i = 0; i < count; ++i)
  {
    emit("ram 0x%" PRIx64 "-0x%" PRIx64, ranges[i].start,
        ranges[i].start + ranges[i].length - 1);
    total += ranges[i].length;
  }
  emit("ram total=0x%" PRIx64 " ranges=%u", total, (unsigned)count);

  count = resourceMap(RESOURCEMAP L"Loader Reserved", L".Raw", ranges,
      MAX_RANGES);
  for(size_t i = 0; i < count; ++i)
    emit("reserved 0x%" PRIx64 "-0x%" PRIx64, ranges[i].start,
        ranges[i].start + ranges[i].length - 1);
}

/* The SharedMemory region, which the guest finds without being told: the HCS
 * maps it right after the VM's memory, and Hyper-V's firmware reports it as
 * reserved memory, which Windows keeps as Loader Reserved. So it is the
 * reserved range that starts where the highest range of RAM ends. */
static bool findRegion(uint64_t * start, uint64_t * length)
{
  struct Range ranges[MAX_RANGES];
  size_t count = ramRanges(ranges, MAX_RANGES);
  uint64_t top = 0;
  for(size_t i = 0; i < count; ++i)
    top = max(top, ranges[i].start + ranges[i].length);
  if (!top)
    return false;

  count = resourceMap(RESOURCEMAP L"Loader Reserved", L".Raw", ranges,
      MAX_RANGES);
  for(size_t i = 0; i < count; ++i)
    if (ranges[i].start == top && ranges[i].length >= 2 * PAGE_SIZE &&
        ranges[i].length % PAGE_SIZE == 0)
    {
      *start  = ranges[i].start;
      *length = ranges[i].length;
      return true;
    }
  return false;
}

// the HCS probe's commands, as its Linux guest answers them
static void cmdInfo(void)
{
  typedef LONG (WINAPI * RtlGetVersion)(OSVERSIONINFOW *);
  RtlGetVersion getVersion = (RtlGetVersion)(void (*)(void))GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  OSVERSIONINFOW info = { .dwOSVersionInfoSize = sizeof(info) };
  if (getVersion && getVersion(&info) == 0)
    emit("kernel Windows %lu.%lu.%lu", info.dwMajorVersion,
        info.dwMinorVersion, info.dwBuildNumber);

  memory();
  status();
  emit("info end");
}

// find: where the guest finds the region by itself
static void cmdFind(void)
{
  uint64_t start, length;
  if (findRegion(&start, &length))
    emit("find ok start=0x%" PRIx64 " length=0x%" PRIx64, start, length);
  else
    emit("find failed step=none error=0x0");
}

// map ivshmem REGISTERS MEMORY SIZE: the device over the region, mapped
// through the IVSHMEM driver
static void cmdMapIvshmem(const char * args, const WCHAR * inf)
{
  uint64_t registers, memory, size;
  if (!parseNumber(&args, &registers) || !parseNumber(&args, &memory) ||
      !parseNumber(&args, &size))
  {
    emit("map failed step=args error=0x0");
    return;
  }

  if (!inf)
  {
    emit("map failed step=inf error=0x0");
    return;
  }

  if (!install(registers, memory, size, inf))
  {
    emit("map failed step=install error=0x0");
    return;
  }

  uint16_t peer;
  if (!mapIvshmem(&peer))
    return;
  emit("map ok peer=0x%x size=0x%" PRIx64 " cache=writecombined", peer,
      mapSize);
}

static volatile uint64_t * pageWord(uint64_t page, unsigned word)
{
  return (volatile uint64_t *)(map + page * PAGE_SIZE) + word;
}

static void cmdVerify(const char * args)
{
  uint64_t magic, nonce, pages;
  if (!map || !parseNumber(&args, &magic) || !parseNumber(&args, &nonce) ||
      !parseNumber(&args, &pages) || !pages || pages > mapSize / PAGE_SIZE)
  {
    emit("verify failed step=args");
    return;
  }

  uint64_t bad = 0, first = 0, got0 = 0, got1 = 0;
  for(uint64_t i = 0; i < pages; ++i)
  {
    const uint64_t v0 = *pageWord(i, 0);
    const uint64_t v1 = *pageWord(i, 1);
    if (v0 == (magic ^ i) && v1 == (nonce ^ (i * GOLDEN)))
      continue;

    if (!bad++)
    {
      first = i;
      got0  = v0;
      got1  = v1;
    }
  }

  if (!bad)
    emit("verify ok pages=0x%" PRIx64, pages);
  else
    emit("verify bad count=0x%" PRIx64 " first=0x%" PRIx64 " got0=0x%" PRIx64
        " got1=0x%" PRIx64, bad, first, got0, got1);
}

static bool pageArg(const char ** args, uint64_t * page)
{
  return map && parseNumber(args, page) && *page < mapSize / PAGE_SIZE;
}

static void cmdWrite(const char * args)
{
  uint64_t page, value;
  if (!pageArg(&args, &page) || !parseNumber(&args, &value))
  {
    emit("write failed step=args");
    return;
  }

  // the mapping is write-combined, and the fence empties its buffers
  *pageWord(page, 2) = value;
  __asm__ volatile ("mfence" ::: "memory");
  emit("write ok page=0x%" PRIx64, page);
}

static void cmdRead(const char * args)
{
  uint64_t page;
  if (!pageArg(&args, &page))
  {
    emit("read failed step=args");
    return;
  }

  __asm__ volatile ("mfence" ::: "memory");
  emit("read ok page=0x%" PRIx64 " value=0x%" PRIx64, page,
      *pageWord(page, 2));
}

// emits what a program wrote, a line at a time
static void emitOutput(const char * prefix, const char * data, size_t len)
{
  // the IDD's installer writes UTF-16
  static char text[65536];
  size_t n = len;
  if (len >= 2 && len % 2 == 0 && memchr(data, '\0', len))
  {
    const int got = WideCharToMultiByte(CP_UTF8, 0, (const WCHAR *)data,
        (int)(len / 2), text, sizeof(text) - 1, NULL, NULL);
    n = got > 0 ? (size_t)got : 0;
  }
  else
    memcpy(text, data, n = min(len, sizeof(text) - 1));
  text[n] = '\0';

  unsigned lines = 0;
  for(char * line = strtok(text, "\r\n"); line && lines < 200;
      line = strtok(NULL, "\r\n"), ++lines)
    emit("%s %s", prefix, line);
}

#define IDD_HARDWARE_ID L"Root\\LGIdd"
#define IDD_TIMEOUT_MS  300000

/* idd: installs the Looking Glass IDD from the idd folder next to this
 * program, as its installer does. The IDD maps the memory through the
 * IVSHMEM driver, as the Looking Glass host does, so this lets go of its own
 * mapping first. */
static void cmdIdd(void)
{
  unmapIvshmem();

  WCHAR dir[MAX_PATH];
  const DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
  WCHAR * slash = n && n < MAX_PATH ? wcsrchr(dir, L'\\') : NULL;
  if (!slash || (size_t)(slash - dir) + 8 >= MAX_PATH)
  {
    emit("idd failed step=path error=0x0");
    return;
  }
  wcscpy(slash + 1, L"idd");

  WCHAR command[MAX_PATH + 64];
  _snwprintf(command, MAX_PATH + 64, L"\"%ls\\LGIddInstall.exe\" install",
      dir);
  command[MAX_PATH + 63] = L'\0';

  SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
  HANDLE readEnd, writeEnd;
  if (!CreatePipe(&readEnd, &writeEnd, &sa, 0))
  {
    failed("idd", "pipe", GetLastError());
    return;
  }
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si =
  {
    .cb         = sizeof(si),
    .dwFlags    = STARTF_USESTDHANDLES,
    .hStdOutput = writeEnd,
    .hStdError  = writeEnd
  };
  PROCESS_INFORMATION pi;
  const BOOL started = CreateProcessW(NULL, command, NULL, NULL, TRUE,
      CREATE_NO_WINDOW, NULL, dir, &si, &pi);
  const DWORD error = GetLastError();
  CloseHandle(writeEnd);
  if (!started)
  {
    CloseHandle(readEnd);
    failed("idd", "start", error);
    return;
  }

  // reads until the installer exits, or gives up on it
  static char out[65536];
  size_t len    = 0;
  bool   exited = false;
  const ULONGLONG deadline = GetTickCount64() + IDD_TIMEOUT_MS;
  for(;;)
  {
    exited = WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0;
    DWORD avail = 0;
    while (PeekNamedPipe(readEnd, NULL, 0, NULL, &avail, NULL) && avail)
    {
      char  scratch[4096];
      DWORD got = 0;
      if (!ReadFile(readEnd, scratch, sizeof(scratch), &got, NULL) || !got)
        break;
      const size_t keep = min((size_t)got, sizeof(out) - len);
      memcpy(out + len, scratch, keep);
      len += keep;
    }
    if (exited || GetTickCount64() > deadline)
      break;
  }
  CloseHandle(readEnd);

  DWORD code = 0;
  if (!exited)
    TerminateProcess(pi.hProcess, 1);
  else
    GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  emitOutput("installer", out, len);

  // the display device that the installer made, which Windows starts
  struct Device d = { .set = INVALID_HANDLE_VALUE };
  if (deviceFindId(&d, IDD_HARDWARE_ID))
  {
    char instance[MAX_DEVICE_ID_LEN * 3];
    deviceInstance(&d, instance, sizeof(instance));
    ULONG status, problem;
    const bool running = deviceStarted(&d, &status, &problem);
    emit("display instance=%s status=0x%lx problem=0x%lx started=%d",
        instance, (unsigned long)status, (unsigned long)problem, running);
    deviceClose(&d);
  }
  else
    emit("display none");

  if (!exited)
    emit("idd failed step=timeout error=0x0");
  else if (code)
    emit("idd failed step=install error=0x%lx", code);
  else
    emit("idd ok");
}

/* iddlog: the end of the Looking Glass IDD's log, which tells how it got on
 * with the memory and with its display */
static void cmdIddLog(void)
{
  WCHAR path[MAX_PATH];
  const DWORD n = ExpandEnvironmentStringsW(
      L"%ProgramData%\\Looking Glass (IDD)\\looking-glass-idd.txt", path,
      MAX_PATH);
  if (!n || n > MAX_PATH)
  {
    emit("iddlog failed step=path error=0x0");
    return;
  }

  HANDLE file = CreateFileW(path, GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE)
  {
    failed("iddlog", "open", GetLastError());
    return;
  }

  // the last lines, as many as the probe keeps
  static char text[12288];
  LARGE_INTEGER size, at = { 0 };
  DWORD got = 0;
  if (GetFileSizeEx(file, &size) && size.QuadPart > (LONGLONG)sizeof(text))
    at.QuadPart = size.QuadPart - (LONGLONG)sizeof(text);
  const bool read = SetFilePointerEx(file, at, NULL, FILE_BEGIN) &&
    ReadFile(file, text, sizeof(text), &got, NULL);
  const DWORD error = GetLastError();
  CloseHandle(file);
  if (!read)
  {
    failed("iddlog", "read", error);
    return;
  }

  emitOutput("log", text, got);
  emit("iddlog ok");
}

/* adapters: the display adapters, as Windows' devices and as DXGI adapters,
 * and whether Direct3D 11 makes a device on each. With a partition of the
 * PC's GPU, one of them is the GPU, and Direct3D makes a device on it only
 * if the GPU's driver from the PC's DriverStore is in the HostDriverStore */
static void cmdAdapters(void)
{
  unsigned devices = 0;
  HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, NULL, NULL,
      DIGCF_PRESENT);
  if (set != INVALID_HANDLE_VALUE)
  {
    SP_DEVINFO_DATA data = { .cbSize = sizeof(data) };
    for(DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i, ++devices)
    {
      WCHAR id[MAX_DEVICE_ID_LEN];
      WCHAR name[256]   = L"";
      WCHAR service[64] = L"";
      if (CM_Get_Device_IDW(data.DevInst, id, MAX_DEVICE_ID_LEN, 0) !=
          CR_SUCCESS)
        wcscpy(id, L"unknown");
      SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_DEVICEDESC, NULL,
          (BYTE *)name, sizeof(name) - sizeof(WCHAR), NULL);
      SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_SERVICE, NULL,
          (BYTE *)service, sizeof(service) - sizeof(WCHAR), NULL);

      ULONG status = 0, problem = 0;
      CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0);
      emit("adapter device instance=%ls status=0x%lx problem=0x%lx "
          "service=%ls name=%ls", id, (unsigned long)status,
          (unsigned long)problem, service[0] ? service : L"none", name);
    }
    SetupDiDestroyDeviceInfoList(set);
  }

  typedef HRESULT (WINAPI * CreateFactory)(REFIID, void **);
  typedef HRESULT (WINAPI * CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE,
      HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
      D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

  HMODULE dxgi  = LoadLibraryW(L"dxgi.dll");
  HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
  CreateFactory createFactory = dxgi ?
    (CreateFactory)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
  CreateDevice createDevice = d3d11 ?
    (CreateDevice)GetProcAddress(d3d11, "D3D11CreateDevice") : NULL;

  IDXGIFactory1 * factory = NULL;
  const HRESULT hr = createFactory && createDevice ?
    createFactory(&IID_IDXGIFactory1, (void **)&factory) :
    HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  if (FAILED(hr))
    emit("adapters failed step=dxgi error=0x%lx devices=%u",
        (unsigned long)hr, devices);
  else
  {
    unsigned adapters = 0, gpus = 0;
    IDXGIAdapter1 * adapter;
    for(UINT i = 0; SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, i,
          &adapter)); ++i, ++adapters)
    {
      DXGI_ADAPTER_DESC1 desc = { 0 };
      IDXGIAdapter1_GetDesc1(adapter, &desc);

      ID3D11Device *    device = NULL;
      D3D_FEATURE_LEVEL level  = 0;
      const HRESULT made = createDevice((IDXGIAdapter *)adapter,
          D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
          &device, &level, NULL);
      if (device)
        ID3D11Device_Release(device);
      IDXGIAdapter1_Release(adapter);

      // Microsoft's own adapters, such as the Basic Render Driver, are
      // vendor 0x1414
      const bool gpu = SUCCEEDED(made) && desc.VendorId != 0x1414 &&
        !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
      gpus += gpu;
      emit("adapter dxgi index=%u vendor=0x%04x device=0x%04x "
          "memory=%" PRIu64 " flags=0x%x d3d11=0x%lx level=0x%x gpu=%d "
          "name=%ls", i, desc.VendorId, desc.DeviceId,
          (uint64_t)desc.DedicatedVideoMemory, desc.Flags,
          (unsigned long)made, (unsigned)level, gpu, desc.Description);
    }
    IDXGIFactory1_Release(factory);
    emit("adapters ok devices=%u dxgi=%u gpus=%u", devices, adapters, gpus);
  }

  if (d3d11)
    FreeLibrary(d3d11);
  if (dxgi)
    FreeLibrary(dxgi);
}

static void powerOff(void)
{
  HANDLE token;
  if (OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
  {
    TOKEN_PRIVILEGES privileges = { .PrivilegeCount = 1 };
    if (LookupPrivilegeValueW(NULL, L"SeShutdownPrivilege",
          &privileges.Privileges[0].Luid))
    {
      privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
      AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL);
    }
    CloseHandle(token);
  }

  if (!InitiateSystemShutdownExW(NULL, NULL, 0, TRUE, FALSE,
        SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
    emit("off failed step=shutdown error=0x%lx", GetLastError());
}

static bool command(const char * line, const char * name, const char ** args)
{
  const size_t len = strlen(name);
  if (strncmp(line, name, len) != 0 || (line[len] && line[len] != ' '))
    return false;
  *args = line + len;
  return true;
}

static void handle(const char * line, const WCHAR * inf)
{
  if (logFile)
  {
    fprintf(logFile, "> %s\n", line);
    fflush(logFile);
  }

  const char * args;
  if (command(line, "info", &args))
    cmdInfo();
  else if (command(line, "map ivshmem", &args))
    cmdMapIvshmem(args, inf);
  else if (command(line, "verify", &args))
    cmdVerify(args);
  else if (command(line, "write", &args))
    cmdWrite(args);
  else if (command(line, "read", &args))
    cmdRead(args);
  else if (command(line, "find", &args))
    cmdFind();
  else if (command(line, "idd", &args))
    cmdIdd();
  else if (command(line, "iddlog", &args))
    cmdIddLog();
  else if (command(line, "adapters", &args))
    cmdAdapters();
  else if (command(line, "off", &args))
  {
    emit("off");
    powerOff();
  }
  else if (line[0])
    emit("error unknown command");
}

// answers the HCS probe on a serial port until Windows shuts down
static int serve(const WCHAR * name, const WCHAR * inf)
{
  WCHAR path[64];
  _snwprintf(path, 64, L"\\\\.\\%ls", name);
  path[63] = L'\0';

  // the port appears once Windows has started its driver
  HANDLE h = INVALID_HANDLE_VALUE;
  for(int i = 0; i < 1200; ++i)
  {
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE)
      break;
    Sleep(500);
  }
  if (h == INVALID_HANDLE_VALUE)
  {
    emit("cannot open %ls: error 0x%lx", path, GetLastError());
    return 1;
  }

  DCB dcb = { .DCBlength = sizeof(dcb) };
  GetCommState(h, &dcb);
  dcb.BaudRate     = CBR_115200;
  dcb.ByteSize     = 8;
  dcb.Parity       = NOPARITY;
  dcb.StopBits     = ONESTOPBIT;
  dcb.fBinary      = TRUE;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fOutX        = FALSE;
  dcb.fInX         = FALSE;
  SetCommState(h, &dcb);

  // a read returns what has come, or after half a second without anything
  COMMTIMEOUTS timeouts =
  {
    .ReadIntervalTimeout         = MAXDWORD,
    .ReadTotalTimeoutMultiplier  = MAXDWORD,
    .ReadTotalTimeoutConstant    = 500,
    .WriteTotalTimeoutConstant   = 5000
  };
  SetCommTimeouts(h, &timeouts);
  port = h;

  static char buf[4096];
  size_t   len       = 0;
  uint64_t greetings = 0;
  bool     heard     = false;
  for(;;)
  {
    // the host may open its end after the guest wrote, which loses the
    // output, so the greeting repeats until a command comes
    if (!heard)
      emit("ready version=0x1 greeting=0x%" PRIx64 " os=windows",
          ++greetings);

    DWORD got = 0;
    if (!ReadFile(h, buf + len, sizeof(buf) - 1 - len, &got, NULL))
    {
      Sleep(500);
      continue;
    }
    if (!got)
      continue;

    heard = true;
    len  += got;

    size_t start = 0;
    for(size_t i = 0; i < len; ++i)
    {
      if (buf[i] != '\n' && buf[i] != '\r')
        continue;
      buf[i] = '\0';
      handle(buf + start, inf);
      start = i + 1;
    }

    // keeps the incomplete line; a line that fills the buffer is dropped
    len -= start;
    memmove(buf, buf + start, len);
    if (len == sizeof(buf) - 1)
      len = 0;
  }
}

/* Checks, without a device or an administrator, what keeps the device off the
 * RAM that Windows uses: the numbers that are taken, and the ranges that are
 * refused, on a made-up RAM so that it is the same on every PC. The ones that
 * wrap around the address space were taken for ranges that did not touch RAM. */
static int selfTest(void)
{
  // what a guest with 2 GiB below 4 GiB and 128 MiB above would list
  static const struct Range ram[] =
  {
    { 0x0000000000001000ULL, 0x000000007FFFF000ULL },
    { 0x0000000100000000ULL, 0x0000000008000000ULL },
  };
  const size_t count = sizeof(ram) / sizeof(ram[0]);

  static const struct
  {
    const char * name;
    uint64_t     registers;
    uint64_t     memory;
    uint64_t     size;
    const char * problem;  // NULL if they can be a device's
  }
  ranges[] =
  {
    { "the region after the RAM",
      0x10FFFF000ULL, 0x108000000ULL, 0x7FFF000ULL, NULL },
    { "memory in the RAM",
      0x10FFFF000ULL, 0x100000000ULL, 0x1000000ULL, "ram" },
    { "registers in the RAM",
      0x100000000ULL, 0x108000000ULL, 0x1000000ULL, "ram" },
    { "memory that ends past the top, and wraps to 0",
      0x10FFFF000ULL, 0x1000ULL, 0xFFFFFFFFFFFFF000ULL, "args" },
    { "memory that ends past the top, and wraps to 0 from higher up",
      0x10FFFF000ULL, 0x100000000ULL, 0xFFFFFFFF00000000ULL, "args" },
    { "memory that ends past the top, and wraps to a small number",
      0x10FFFF000ULL, 0x200000000ULL, 0xFFFFFFFFFFFFE000ULL, "args" },
    { "registers at the top",
      0xFFFFFFFFFFFFF000ULL, 0x108000000ULL, 0x1000ULL, "args" },
    { "memory past what x64 can address",
      0x10000000001000ULL, 0x10000000000000ULL, 0x1000ULL, "args" },
    { "memory that ends past what x64 can address, without wrapping",
      0x10FFFF000ULL, 0x200000000ULL, 0x10000000000000ULL, "args" },
    { "no memory",
      0x10FFFF000ULL, 0x108000000ULL, 0, "args" },
    { "memory that is not whole pages",
      0x10FFFF000ULL, 0x108000000ULL, 0x7FFF001ULL, "args" },
    { "registers that are not on a page",
      0x10FFFF010ULL, 0x108000000ULL, 0x7FFF000ULL, "args" },
    { "registers in the memory",
      0x108001000ULL, 0x108000000ULL, 0x7FFF000ULL, "args" },
  };

  int failures = 0;
  for(size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i)
  {
    const char * problem = rangesProblem(ram, count, ranges[i].registers,
        ranges[i].memory, ranges[i].size);
    const bool same = problem && ranges[i].problem ?
      strcmp(problem, ranges[i].problem) == 0 : problem == ranges[i].problem;
    if (!same)
    {
      emit("selftest failed ranges=\"%s\" got=%s wanted=%s", ranges[i].name,
          problem ? problem : "accepted",
          ranges[i].problem ? ranges[i].problem : "accepted");
      ++failures;
    }
  }

  static const struct
  {
    const char * text;
    bool         valid;
    uint64_t     value;
  }
  numbers[] =
  {
    { "4096"                 , true , 4096 },
    { "0x1000"               , true , 0x1000 },
    { "  0x20"               , true , 0x20 },
    { "0xFFFFFFFFFFFFFFFF"   , true , UINT64_MAX },
    { "18446744073709551615" , true , UINT64_MAX },
    { "-4096"                , false, 0 },
    { "  -4096"              , false, 0 },
    { "\t-4096"              , false, 0 },
    { "0x10000000000000000"  , false, 0 },
    { "18446744073709551616" , false, 0 },
    { ""                     , false, 0 },
    { "0x"                   , false, 0 },
    { "12x"                  , false, 0 },
  };

  for(size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i)
  {
    const char * text = numbers[i].text;
    uint64_t value = 0;
    const bool valid = parseNumber(&text, &value);
    if (valid != numbers[i].valid || (valid && value != numbers[i].value))
    {
      emit("selftest failed number=\"%s\" valid=%d", numbers[i].text, valid);
      ++failures;
    }

    // the same number, as a command line gives it
    WCHAR wide[64] = { 0 };
    for(size_t c = 0; numbers[i].text[c] && c + 1 < 64; ++c)
      wide[c] = (unsigned char)numbers[i].text[c];
    value = 0;
    const bool wideValid = parseWide(wide, &value);
    if (wideValid != numbers[i].valid ||
        (wideValid && value != numbers[i].value))
    {
      emit("selftest failed number=\"%s\" valid=%d as an argument",
          numbers[i].text, wideValid);
      ++failures;
    }
  }

  if (failures)
    return 1;

  emit("selftest ok");
  return 0;
}

static void usage(void)
{
  printf(
    "Usage: lg-hyperv-ivshmem COMMAND\n"
    "\n"
    "Gives this Windows guest of Hyper-V an IVSHMEM device over memory that\n"
    "the PC shares with it through the Host Compute Service, and installs the\n"
    "IVSHMEM driver on it, for the Looking Glass host. Run it as an\n"
    "administrator.\n"
    "\n"
    "  install INF\n"
    "      makes or updates the device over the region that find finds,\n"
    "      whose last page is the registers, and INF is the IVSHMEM\n"
    "      driver's INF\n"
    "  install REGISTERS MEMORY SIZE INF\n"
    "      the same with the ranges given: REGISTERS is the guest physical\n"
    "      address of a page of the region that the PC keeps zeroed, MEMORY\n"
    "      and SIZE the shared memory\n"
    "  check REGISTERS MEMORY SIZE\n"
    "      says whether install would take these ranges, which must be whole\n"
    "      pages that x64 can address and not RAM that Windows uses, and\n"
    "      changes nothing\n"
    "  selftest\n"
    "      checks the numbers and ranges that install takes and refuses, on\n"
    "      made-up RAM, and changes nothing\n"
    "  find\n"
    "      shows the region: the memory that Windows keeps reserved where\n"
    "      its RAM ends, where the HCS puts it\n"
    "  remove\n"
    "      removes the device\n"
    "  status\n"
    "      shows the device and its resources\n"
    "  memory\n"
    "      shows the RAM that Windows uses, which the device stays out of\n"
    "  adapters\n"
    "      shows the display adapters, and whether Direct3D 11 works on\n"
    "      each, such as a partition of the PC's GPU\n"
    "  serial PORT [INF]\n"
    "      answers the HCS probe (lg-windows-client-hcs-probe) on a serial\n"
    "      port such as COM1\n");
}

int wmain(int argc, WCHAR ** argv)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2)
  {
    usage();
    return 2;
  }

  const WCHAR * verb = argv[1];
  if ((_wcsicmp(verb, L"install") == 0 && argc == 6) ||
      (_wcsicmp(verb, L"check") == 0 && argc == 5))
  {
    uint64_t values[3];
    for(int i = 0; i < 3; ++i)
      if (!parseWide(argv[2 + i], &values[i]))
      {
        usage();
        return 2;
      }

    if (_wcsicmp(verb, L"install") == 0)
      return install(values[0], values[1], values[2], argv[5]) ? 0 : 1;

    if (!checkRanges("check", values[0], values[1], values[2]))
      return 1;
    emit("check ok");
    return 0;
  }

  // the region that the guest finds, whose last page is the registers
  if (_wcsicmp(verb, L"install") == 0 && argc == 3)
  {
    uint64_t start, length;
    if (!findRegion(&start, &length))
    {
      emit("install failed step=find error=0x0 no reserved memory starts "
          "where the RAM ends");
      return 1;
    }
    emit("region 0x%" PRIx64 "-0x%" PRIx64, start, start + length - 1);
    return install(start + length - PAGE_SIZE, start, length - PAGE_SIZE,
        argv[2]) ? 0 : 1;
  }

  if (_wcsicmp(verb, L"find") == 0 && argc == 2)
  {
    uint64_t start, length;
    if (!findRegion(&start, &length))
    {
      emit("region none");
      return 1;
    }
    emit("region 0x%" PRIx64 "-0x%" PRIx64, start, start + length - 1);
    return 0;
  }

  if (_wcsicmp(verb, L"remove") == 0 && argc == 2)
    return removeDevice() ? 0 : 1;

  if (_wcsicmp(verb, L"selftest") == 0 && argc == 2)
    return selfTest();

  if (_wcsicmp(verb, L"status") == 0 && argc == 2)
  {
    status();
    return 0;
  }

  if (_wcsicmp(verb, L"memory") == 0 && argc == 2)
  {
    memory();
    return 0;
  }

  if (_wcsicmp(verb, L"adapters") == 0 && argc == 2)
  {
    cmdAdapters();
    return 0;
  }

  if (_wcsicmp(verb, L"serial") == 0 && (argc == 3 || argc == 4))
  {
    // a log next to the program, for when nothing reaches the port
    WCHAR logPath[MAX_PATH];
    const DWORD n = GetModuleFileNameW(NULL, logPath, MAX_PATH);
    if (n && n < MAX_PATH - 8)
    {
      WCHAR * dot = wcsrchr(logPath, L'.');
      if (dot)
        wcscpy(dot, L".log");
      logFile = _wfopen(logPath, L"ab");
    }
    return serve(argv[2], argc == 4 ? argv[3] : NULL);
  }

  usage();
  return 2;
}
