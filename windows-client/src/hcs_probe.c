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
 * Checks, on a Windows PC with Hyper-V, two ways for the host to share memory
 * with a VM like IVSHMEM does under QEMU:
 *
 *   hdv  an IVSHMEM PCI device (1af4:1110) that this process emulates through
 *        the HCS device emulation API, with BAR2 backed by a section
 *   shm  a HCS SharedMemory region, which maps a named section at a guest
 *        physical address
 *
 * Each case boots a disposable Linux VM through the HCS, as WSL does, whose
 * init (hcs_probe_guest.c) checks the memory from the guest side and answers
 * on the serial port. The VMs boot copies of the kernel and the initrd in the
 * probe's output folder and end when it exits, so the probe leaves nothing on
 * the host but that folder.
 *
 * With --vm it instead checks a VM that something else created, such as
 * Hyper-V Manager: whether the HCS opens it and lets this PC add a
 * SharedMemory region to it, which the probe removes again.
 *
 * With --windows-disk it boots a disposable Windows guest from a disk whose
 * lg-hyperv-ivshmem answers on the serial port (hyperv_ivshmem.c): the tool
 * gives the unchanged IVSHMEM driver a device over a SharedMemory region,
 * and the checks go through the driver's mapping, as the Looking Glass
 * host's would.
 */

#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <sddl.h>

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LG_HCS_PROBE_GUEST
#error LG_HCS_PROBE_GUEST must name the built guest init
#endif

// the guest init, a static Linux executable
__asm__(
  ".section .rdata,\"dr\"\n"
  ".balign 16\n"
  ".globl guestInit\n"
  "guestInit:\n"
  ".incbin \"" LG_HCS_PROBE_GUEST "\"\n"
  ".globl guestInitEnd\n"
  "guestInitEnd:\n"
  ".text\n"
);
extern const char guestInit[], guestInitEnd[];

#define PAGE          4096
#define PROBE_MAGIC   0x4c47534850524f42ULL
#define GOLDEN        0x9E3779B97F4A7C15ULL

#define IVSHMEM_VENDOR    0x1af4
#define IVSHMEM_DEVICE    0x1110
#define IVSHMEM_SUBSYSTEM 0x1100
#define IVSHMEM_REGS_SIZE 256

// identifies this emulator to the HCS
static const GUID emulatorClass =
  { 0x62cdfa57, 0x7648, 0x44f3,
    { 0xb1, 0x64, 0x98, 0xe2, 0xda, 0x99, 0x1e, 0xc5 } };

// computecore.h, loaded at run time for a clear error where it is missing
typedef HANDLE HCS_OPERATION;
typedef HANDLE HCS_SYSTEM;
typedef void (CALLBACK * HCS_OPERATION_COMPLETION)(HCS_OPERATION, void *);

typedef struct
{
  DWORD         Type;
  PCWSTR        EventData;
  HCS_OPERATION Operation;
}
HCS_EVENT;

#define HcsEventSystemExited          0x00000001
#define HcsEventSystemCrashInitiated  0x00000002
#define HcsEventSystemCrashReport     0x00000003
#define HcsEventGuestConnectionClosed 0x00000006
#define HcsEventServiceDisconnect     0x02000000

typedef void (CALLBACK * HCS_EVENT_CALLBACK)(HCS_EVENT *, void *);

// vmdevicehost.h, which MinGW does not ship
typedef void * HDV_HOST;
typedef void * HDV_DEVICE;

typedef enum
{
  HdvDeviceHostFlagNone                  = 0,
  HdvDeviceHostFlagInitializeComSecurity = 1
}
HDV_DEVICE_HOST_FLAGS;

typedef enum
{
  HdvDeviceTypeUndefined = 0,
  HdvDeviceTypePCI       = 1
}
HDV_DEVICE_TYPE;

typedef enum
{
  HDV_PCI_BAR0 = 0,
  HDV_PCI_BAR1,
  HDV_PCI_BAR2,
  HDV_PCI_BAR3,
  HDV_PCI_BAR4,
  HDV_PCI_BAR5
}
HDV_PCI_BAR_SELECTOR;

typedef enum
{
  HdvMmioMappingFlagNone       = 0,
  HdvMmioMappingFlagWriteable  = 1,
  HdvMmioMappingFlagExecutable = 2
}
HDV_MMIO_MAPPING_FLAGS;

typedef enum
{
  HdvPciDeviceInterfaceVersionInvalid = 0,
  HdvPciDeviceInterfaceVersion1       = 1
}
HDV_PCI_INTERFACE_VERSION;

typedef struct
{
  UINT16 VendorID;
  UINT16 DeviceID;
  UINT8  RevisionID;
  UINT8  ProgIf;
  UINT8  SubClass;
  UINT8  BaseClass;
  UINT16 SubVendorID;
  UINT16 SubSystemID;
}
HDV_PCI_PNP_ID;

typedef struct
{
  HDV_PCI_INTERFACE_VERSION Version;
  HRESULT (CALLBACK * Initialize)(void * context);
  void    (CALLBACK * Teardown)(void * context);
  HRESULT (CALLBACK * SetConfiguration)(void * context, UINT32 count,
      const PCWSTR * values);
  HRESULT (CALLBACK * GetDetails)(void * context, HDV_PCI_PNP_ID * pnpId,
      UINT32 probedBarsCount, UINT32 * probedBars);
  HRESULT (CALLBACK * Start)(void * context);
  void    (CALLBACK * Stop)(void * context);
  HRESULT (CALLBACK * ReadConfigSpace)(void * context, UINT32 offset,
      UINT32 * value);
  HRESULT (CALLBACK * WriteConfigSpace)(void * context, UINT32 offset,
      UINT32 value);
  HRESULT (CALLBACK * ReadInterceptedMemory)(void * context,
      HDV_PCI_BAR_SELECTOR bar, UINT64 offset, UINT64 length, BYTE * value);
  HRESULT (CALLBACK * WriteInterceptedMemory)(void * context,
      HDV_PCI_BAR_SELECTOR bar, UINT64 offset, UINT64 length,
      const BYTE * value);
}
HDV_PCI_DEVICE_INTERFACE;

static struct
{
  HCS_OPERATION (WINAPI * createOperation)(const void *,
      HCS_OPERATION_COMPLETION);
  void    (WINAPI * closeOperation)(HCS_OPERATION);
  HRESULT (WINAPI * cancelOperation)(HCS_OPERATION);
  HRESULT (WINAPI * waitForOperationResult)(HCS_OPERATION, DWORD, PWSTR *);
  HRESULT (WINAPI * createComputeSystem)(PCWSTR, PCWSTR, HCS_OPERATION,
      const SECURITY_DESCRIPTOR *, HCS_SYSTEM *);
  HRESULT (WINAPI * startComputeSystem)(HCS_SYSTEM, HCS_OPERATION, PCWSTR);
  HRESULT (WINAPI * terminateComputeSystem)(HCS_SYSTEM, HCS_OPERATION,
      PCWSTR);
  HRESULT (WINAPI * getComputeSystemProperties)(HCS_SYSTEM, HCS_OPERATION,
      PCWSTR);
  void    (WINAPI * closeComputeSystem)(HCS_SYSTEM);
  HRESULT (WINAPI * enumerateComputeSystems)(PCWSTR, HCS_OPERATION);
  HRESULT (WINAPI * grantVmAccess)(PCWSTR, PCWSTR);
  HRESULT (WINAPI * revokeVmAccess)(PCWSTR, PCWSTR);
  HRESULT (WINAPI * getServiceProperties)(PCWSTR, PWSTR *);
  HRESULT (WINAPI * setComputeSystemCallback)(HCS_SYSTEM, DWORD, const void *,
      HCS_EVENT_CALLBACK);

  // only needed to check an existing VM
  HRESULT (WINAPI * openComputeSystem)(PCWSTR, DWORD, HCS_SYSTEM *);
  HRESULT (WINAPI * modifyComputeSystem)(HCS_SYSTEM, HCS_OPERATION, PCWSTR,
      HANDLE);

  // device emulation, only needed by the hdv case
  HRESULT (WINAPI * initializeDeviceHost)(HCS_SYSTEM, HDV_HOST *);
  HRESULT (WINAPI * initializeDeviceHostEx)(HCS_SYSTEM, HDV_DEVICE_HOST_FLAGS,
      HDV_HOST *);
  HRESULT (WINAPI * teardownDeviceHost)(HDV_HOST);
  HRESULT (WINAPI * createDeviceInstance)(HDV_HOST, HDV_DEVICE_TYPE,
      const GUID *, const GUID *, const void *, void *, HDV_DEVICE *);
  HRESULT (WINAPI * createSectionBackedMmioRange)(HDV_DEVICE,
      HDV_PCI_BAR_SELECTOR, UINT64, UINT64, HDV_MMIO_MAPPING_FLAGS, HANDLE,
      UINT64);
  HRESULT (WINAPI * destroySectionBackedMmioRange)(HDV_DEVICE,
      HDV_PCI_BAR_SELECTOR, UINT64);
}
api;

static volatile LONG aborted;

static void fail(const char * what)
{
  fprintf(stderr, "%s\n", what);
  exit(2);
}

// growable text, always terminated
struct Str
{
  char * data;
  size_t len, cap;
};

static void strAdd(struct Str * s, const char * text, size_t len)
{
  if (s->len + len + 1 > s->cap)
  {
    size_t cap = s->cap ? s->cap : 256;
    while (cap < s->len + len + 1)
      cap *= 2;
    char * data = realloc(s->data, cap);
    if (!data)
      fail("Out of memory");
    s->data = data;
    s->cap  = cap;
  }
  memcpy(s->data + s->len, text, len);
  s->len += len;
  s->data[s->len] = '\0';
}

#define strLiteral(s, text) strAdd(s, text, sizeof(text) - 1)

static void strPrintf(struct Str * s, const char * fmt, ...)
  __attribute__((format(__MINGW_PRINTF_FORMAT, 2, 3)));

static void strPrintf(struct Str * s, const char * fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  const int len = _vscprintf(fmt, ap);
  va_end(ap);
  if (len < 0)
    return;

  char * text = malloc(len + 1);
  if (!text)
    fail("Out of memory");
  va_start(ap, fmt);
  vsnprintf(text, len + 1, fmt, ap);
  va_end(ap);
  strAdd(s, text, len);
  free(text);
}

static void strJsonString(struct Str * s, const char * text)
{
  strAdd(s, "\"", 1);
  for(const unsigned char * p = (const unsigned char *)text; *p; ++p)
  {
    if (*p == '"' || *p == '\\')
    {
      const char escaped[2] = { '\\', *p };
      strAdd(s, escaped, 2);
    }
    else if (*p < 0x20)
      strPrintf(s, "\\u%04x", *p);
    else
      strAdd(s, (const char *)p, 1);
  }
  strAdd(s, "\"", 1);
}

// the report, written as it goes; members are separated automatically
struct Json
{
  struct Str s;
  int        depth;
  bool       first[16];
};

static void jsonMember(struct Json * j, const char * key)
{
  if (!j->first[j->depth])
    strAdd(&j->s, ",", 1);
  j->first[j->depth] = false;
  if (key)
  {
    strJsonString(&j->s, key);
    strAdd(&j->s, ":", 1);
  }
}

static void jsonOpen(struct Json * j, const char * key, char bracket)
{
  if (j->depth || j->s.len)
    jsonMember(j, key);
  strAdd(&j->s, &bracket, 1);
  j->first[++j->depth] = true;
}

static void jsonClose(struct Json * j, char bracket)
{
  strAdd(&j->s, &bracket, 1);
  --j->depth;
}

static void jsonString(struct Json * j, const char * key, const char * value)
{
  jsonMember(j, key);
  if (value)
    strJsonString(&j->s, value);
  else
    strAdd(&j->s, "null", 4);
}

static void jsonBool(struct Json * j, const char * key, bool value)
{
  jsonMember(j, key);
  strAdd(&j->s, value ? "true" : "false", value ? 4 : 5);
}

static void jsonNumber(struct Json * j, const char * key, uint64_t value)
{
  jsonMember(j, key);
  strPrintf(&j->s, "%" PRIu64, value);
}

static void jsonResult(struct Json * j, const char * key, HRESULT hr)
{
  char text[16];
  snprintf(text, sizeof(text), "0x%08lx", (unsigned long)hr);
  jsonString(j, key, text);
}

// a document that is JSON already, such as one the HCS returned
static void jsonRaw(struct Json * j, const char * key, const char * value)
{
  jsonMember(j, key);
  if (value && *value)
    strAdd(&j->s, value, strlen(value));
  else
    strAdd(&j->s, "null", 4);
}

static WCHAR * widen(const char * text)
{
  const int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
  WCHAR * wide = malloc(sizeof(WCHAR) * (len > 0 ? len : 1));
  if (!wide)
    fail("Out of memory");
  if (len <= 0 || !MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, len))
    wide[0] = L'\0';
  return wide;
}

static char * narrow(const WCHAR * text)
{
  const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL,
      NULL);
  char * utf8 = malloc(len > 0 ? len : 1);
  if (!utf8)
    fail("Out of memory");
  if (len <= 0 ||
      !WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, len, NULL, NULL))
    utf8[0] = '\0';
  return utf8;
}

// the value of the first "name" member in a JSON document
static const char * jsonFind(const char * doc, const char * name)
{
  if (!doc)
    return NULL;

  char key[64];
  snprintf(key, sizeof(key), "\"%s\"", name);

  // a string equal to the name is a member only when a colon follows it
  for(const char * p = doc; (p = strstr(p, key));)
  {
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      ++p;
    if (*p != ':')
      continue;

    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      ++p;
    return p;
  }
  return NULL;
}

static bool jsonGetString(const char * doc, const char * name, char * out,
    size_t size)
{
  const char * p = jsonFind(doc, name);
  if (!p || *p++ != '"')
    return false;

  size_t len = 0;
  for(; *p && *p != '"' && len < size - 1; ++p)
  {
    if (*p == '\\' && p[1])
      ++p;
    out[len++] = *p;
  }
  out[len] = '\0';
  return *p == '"';
}

static bool jsonGetUInt64(const char * doc, const char * name,
    uint64_t * value)
{
  const char * p = jsonFind(doc, name);
  if (!p || *p < '0' || *p > '9')
    return false;
  *value = strtoull(p, NULL, 10);
  return true;
}

static bool jsonIsTrue(const char * doc, const char * name)
{
  const char * p = jsonFind(doc, name);
  return p && strncmp(p, "true", 4) == 0;
}

static uint64_t random64(void)
{
  uint64_t value = 0;
  if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)&value, sizeof(value),
          BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
    fail("BCryptGenRandom failed");
  return value;
}

static void guidText(const GUID * guid, char * text, size_t size)
{
  snprintf(text, size,
      "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      (unsigned)guid->Data1, guid->Data2, guid->Data3,
      guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
      guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
}

static void newGuid(GUID * guid, char * text, size_t size)
{
  if (FAILED(CoCreateGuid(guid)))
    fail("CoCreateGuid failed");
  guidText(guid, text, size);
}

// waits for the HCS call made on op, closes op and returns the call's result
// with its result document in UTF-8
static HRESULT hcsFinish(HCS_OPERATION op, HRESULT hr, DWORD timeoutMs,
    char ** doc)
{
  PWSTR result = NULL;
  if (SUCCEEDED(hr))
  {
    hr = api.waitForOperationResult(op, timeoutMs, &result);
    if (FAILED(hr))
      api.cancelOperation(op);
  }

  if (doc)
    *doc = result ? narrow(result) : NULL;
  if (result)
    LocalFree(result);
  api.closeOperation(op);
  return hr;
}

static HCS_OPERATION hcsOperation(void)
{
  HCS_OPERATION op = api.createOperation(NULL, NULL);
  if (!op)
    fail("HcsCreateOperation failed");
  return op;
}

static HRESULT hcsCreate(const char * id, const char * config,
    HCS_SYSTEM * system, char ** doc)
{
  WCHAR * wid     = widen(id);
  WCHAR * wconfig = widen(config);
  HCS_OPERATION op = hcsOperation();

  *system = NULL;
  HRESULT hr = api.createComputeSystem(wid, wconfig, op, NULL, system);
  hr = hcsFinish(op, hr, 60000, doc);
  if (FAILED(hr) && *system)
  {
    api.closeComputeSystem(*system);
    *system = NULL;
  }

  free(wid);
  free(wconfig);
  return hr;
}

typedef HRESULT (WINAPI * HcsSystemCall)(HCS_SYSTEM, HCS_OPERATION, PCWSTR);

static HRESULT hcsCall(HCS_SYSTEM system, HcsSystemCall call,
    const char * argument, DWORD timeoutMs, char ** doc)
{
  WCHAR * warg = argument ? widen(argument) : NULL;
  HCS_OPERATION op = hcsOperation();
  HRESULT hr = hcsFinish(op, call(system, op, warg), timeoutMs, doc);
  free(warg);
  return hr;
}

static HRESULT hcsModify(HCS_SYSTEM system, const char * request, char ** doc)
{
  WCHAR * wrequest = widen(request);
  HCS_OPERATION op = hcsOperation();
  HRESULT hr = hcsFinish(op, api.modifyComputeSystem(system, op, wrequest,
        NULL), 30000, doc);
  free(wrequest);
  return hr;
}

// every compute system the HCS knows
static HRESULT hcsEnumerate(char ** doc)
{
  HCS_OPERATION op = hcsOperation();
  return hcsFinish(op, api.enumerateComputeSystems(NULL, op), 10000, doc);
}

#define RESOLVE(module, field, name) \
  (api.field = (__typeof__(api.field))(void (*)(void)) \
     GetProcAddress(module, name))

static bool loadHcs(void)
{
  HMODULE core = LoadLibraryExW(L"computecore.dll", NULL,
      LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!core)
  {
    printf("computecore.dll is missing. The Host Compute Service comes with "
        "the Hyper-V and Virtual Machine Platform features.\n");
    return false;
  }

  if (!RESOLVE(core, createOperation           , "HcsCreateOperation"       ) ||
      !RESOLVE(core, closeOperation            , "HcsCloseOperation"        ) ||
      !RESOLVE(core, cancelOperation           , "HcsCancelOperation"       ) ||
      !RESOLVE(core, waitForOperationResult    , "HcsWaitForOperationResult") ||
      !RESOLVE(core, createComputeSystem       , "HcsCreateComputeSystem"   ) ||
      !RESOLVE(core, startComputeSystem        , "HcsStartComputeSystem"    ) ||
      !RESOLVE(core, terminateComputeSystem    , "HcsTerminateComputeSystem") ||
      !RESOLVE(core, getComputeSystemProperties,
        "HcsGetComputeSystemProperties") ||
      !RESOLVE(core, closeComputeSystem        , "HcsCloseComputeSystem"    ) ||
      !RESOLVE(core, enumerateComputeSystems   ,
        "HcsEnumerateComputeSystems") ||
      !RESOLVE(core, grantVmAccess             , "HcsGrantVmAccess"         ) ||
      !RESOLVE(core, revokeVmAccess            , "HcsRevokeVmAccess"        ))
  {
    printf("computecore.dll lacks a function this probe needs\n");
    return false;
  }
  RESOLVE(core, getServiceProperties, "HcsGetServiceProperties");
  RESOLVE(core, setComputeSystemCallback, "HcsSetComputeSystemCallback");
  RESOLVE(core, openComputeSystem, "HcsOpenComputeSystem");
  RESOLVE(core, modifyComputeSystem, "HcsModifyComputeSystem");

  HMODULE hdv = LoadLibraryExW(L"vmdevicehost.dll", NULL,
      LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (hdv)
  {
    RESOLVE(hdv, initializeDeviceHost  , "HdvInitializeDeviceHost"  );
    RESOLVE(hdv, initializeDeviceHostEx, "HdvInitializeDeviceHostEx");
    RESOLVE(hdv, teardownDeviceHost    , "HdvTeardownDeviceHost"    );
    RESOLVE(hdv, createDeviceInstance  , "HdvCreateDeviceInstance"  );
    RESOLVE(hdv, createSectionBackedMmioRange,
        "HdvCreateSectionBackedMmioRange");
    RESOLVE(hdv, destroySectionBackedMmioRange,
        "HdvDestroySectionBackedMmioRange");
  }
  return true;
}

static bool hdvAvailable(void)
{
  return (api.initializeDeviceHost || api.initializeDeviceHostEx) &&
    api.teardownDeviceHost && api.createDeviceInstance &&
    api.createSectionBackedMmioRange && api.destroySectionBackedMmioRange;
}

static void * tokenInfo(TOKEN_INFORMATION_CLASS type)
{
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return NULL;

  DWORD size = 0;
  GetTokenInformation(token, type, NULL, 0, &size);
  void * info = size ? malloc(size) : NULL;
  if (info && !GetTokenInformation(token, type, info, size, &size))
  {
    free(info);
    info = NULL;
  }
  CloseHandle(token);
  return info;
}

static bool isElevated(void)
{
  TOKEN_ELEVATION * elevation = tokenInfo(TokenElevation);
  const bool elevated = elevation && elevation->TokenIsElevated;
  free(elevation);
  return elevated;
}

struct Section
{
  HANDLE    handle;
  uint8_t * view;
  uint64_t  size;
  char      name[96];
};

// SYSTEM, the administrators, this user and the VM worker processes, which
// run as members of NT VIRTUAL MACHINE\Virtual Machines
static bool sectionCreate(struct Section * s, const char * name,
    uint64_t size)
{
  memset(s, 0, sizeof(*s));
  s->size = size;
  snprintf(s->name, sizeof(s->name), "%s", name ? name : "");

  TOKEN_USER * user = tokenInfo(TokenUser);
  char * userSid = NULL;
  if (!user || !ConvertSidToStringSidA(user->User.Sid, &userSid))
  {
    free(user);
    printf("Cannot read the user's SID: error %lu\n", GetLastError());
    return false;
  }

  char sddl[256];
  snprintf(sddl, sizeof(sddl),
      "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%s)(A;;GA;;;S-1-5-83-0)", userSid);
  LocalFree(userSid);
  free(user);

  SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl,
        SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL))
  {
    printf("Cannot build the section's security: error %lu\n",
        GetLastError());
    return false;
  }

  s->handle = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa,
      PAGE_READWRITE | SEC_COMMIT, (DWORD)(size >> 32), (DWORD)size,
      name);
  const DWORD error = GetLastError();
  LocalFree(sa.lpSecurityDescriptor);

  if (!s->handle || error == ERROR_ALREADY_EXISTS)
  {
    printf("Cannot create the %s section: error %lu\n",
        name ? name : "unnamed", error);
    if (s->handle)
      CloseHandle(s->handle);
    s->handle = NULL;
    return false;
  }

  s->view = MapViewOfFile(s->handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!s->view)
  {
    printf("Cannot map the section: error %lu\n", GetLastError());
    CloseHandle(s->handle);
    s->handle = NULL;
    return false;
  }
  return true;
}

static void sectionClose(struct Section * s)
{
  if (s->view)
    UnmapViewOfFile(s->view);
  if (s->handle)
    CloseHandle(s->handle);
  memset(s, 0, sizeof(*s));
}

static volatile uint64_t * sectionWord(struct Section * s, uint64_t page,
    unsigned word)
{
  return (volatile uint64_t *)(s->view + page * PAGE) + word;
}

// the pattern the guest checks: two words at the start of every page
static void sectionFill(struct Section * s, uint64_t nonce)
{
  for(uint64_t i = 0; i < s->size / PAGE; ++i)
  {
    *sectionWord(s, i, 0) = PROBE_MAGIC ^ i;
    *sectionWord(s, i, 1) = nonce ^ (i * GOLDEN);
    *sectionWord(s, i, 2) = 0;
  }
  MemoryBarrier();
}

static bool writeFile(const char * path, const void * data, size_t size)
{
  WCHAR * wpath = widen(path);
  HANDLE file = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, NULL);
  free(wpath);
  if (file == INVALID_HANDLE_VALUE)
    return false;

  DWORD written = 0;
  const bool ok = WriteFile(file, data, (DWORD)size, &written, NULL) &&
    written == size;
  CloseHandle(file);
  return ok;
}

static bool fileExists(const char * path)
{
  WCHAR * wpath = widen(path);
  const DWORD attr = GetFileAttributesW(wpath);
  free(wpath);
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static char * fullPath(const char * path)
{
  WCHAR * wpath = widen(path);
  WCHAR   full[MAX_PATH * 2];
  const DWORD len = GetFullPathNameW(wpath, MAX_PATH * 2, full, NULL);
  free(wpath);
  if (!len || len >= MAX_PATH * 2)
    return NULL;
  return narrow(full);
}

static void cpioEntry(struct Str * out, uint32_t ino, const char * name,
    uint32_t mode, uint32_t rdevMajor, uint32_t rdevMinor, const void * data,
    size_t size)
{
  static const char zeros[4] = { 0 };
  const size_t nameSize = strlen(name) + 1;
  strPrintf(out, "070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X"
      "%08X", ino, mode, 0, 0, 1, 0, (uint32_t)size, 0, 0, rdevMajor,
      rdevMinor, (uint32_t)nameSize, 0);
  strAdd(out, name, nameSize);
  strAdd(out, zeros, -out->len & 3);
  if (size)
  {
    strAdd(out, data, size);
    strAdd(out, zeros, -out->len & 3);
  }
}

// an initramfs with the guest init and nothing else
static bool writeInitrd(const char * path)
{
  struct Str cpio = { 0 };
  cpioEntry(&cpio, 1, "dev"        , 0040755, 0, 0, NULL, 0);
  cpioEntry(&cpio, 2, "dev/console", 0020600, 5, 1, NULL, 0);
  cpioEntry(&cpio, 3, "proc"       , 0040555, 0, 0, NULL, 0);
  cpioEntry(&cpio, 4, "sys"        , 0040555, 0, 0, NULL, 0);
  cpioEntry(&cpio, 5, "init"       , 0100755, 0, 0, guestInit,
      guestInitEnd - guestInit);
  cpioEntry(&cpio, 6, "TRAILER!!!" , 0, 0, 0, NULL, 0);

  const bool ok = writeFile(path, cpio.data, cpio.len);
  free(cpio.data);
  return ok;
}

struct Options
{
  char     kernelSource[MAX_PATH * 3];
  char     kernel[MAX_PATH * 3];
  char     initrd[MAX_PATH * 3];
  char     out[MAX_PATH * 3];
  uint64_t size;
  DWORD    bootTimeoutMs;
  bool     hdv, shm;

  // an existing VM to check instead
  char     vm[40];

  // a disk with Windows to boot instead
  char     windowsDisk[MAX_PATH * 3];

  // the newest 2.x configuration schema the HCS supports
  unsigned schemaMinor;
};

// the configuration schema of Limiar's HCS probe VMs, 2.2
#define SCHEMA_MINOR 2

// the memory of the probe's VMs, and of its Windows guest, which is what
// Windows 11 asks for
#define VM_MEMORY_MB      512
#define WINDOWS_MEMORY_MB 4096

// the Microsoft Windows template of Hyper-V Manager's generation 2 VMs
#define WINDOWS_SECURE_BOOT "1734c6e8-3154-4dda-ba5f-a874cc483422"

// room that prepare() leaves after the output folder's path for file names
#define OUT_NAME_MAX 64

// the path of a file in the output folder
static void outPath(const struct Options * options, const char * name,
    char * path, size_t size)
{
  const int len = snprintf(path, size, "%s\\%s", options->out, name);
  if (len < 0 || (size_t)len >= size)
    fail("The output folder's path is too long");
}

// one disposable VM and the guest on its serial port
struct Vm
{
  char        id[40];
  char        pipe[96];
  HCS_SYSTEM  system;
  HANDLE      serial;
  FILE      * log;
  char        pending[16384];
  size_t      pendingLen;
  struct Str  transcript;
  int         replies;
  bool        ready, panicked, disconnected, stopped;

  // a Windows guest restarts while it sets itself up, which closes the
  // serial port until the VM opens it again
  bool        reopenSerial;
  int         serialReopened;
  char        exitType[32];
  DWORD       lastPoll;
  const struct Options * options;

  // what the HCS said about the VM, such as why it stopped
  char        * events[8];
  volatile LONG eventCount;
};

static void vmPollState(struct Vm * vm)
{
  char * doc = NULL;
  if (SUCCEEDED(hcsCall(vm->system, api.getComputeSystemProperties, "{}",
          5000, &doc)))
  {
    char state[32] = "";
    jsonGetString(doc, "State", state, sizeof(state));
    if (strcmp(state, "Stopped") == 0 || jsonIsTrue(doc, "Stopped"))
    {
      vm->stopped = true;
      jsonGetString(doc, "ExitType", vm->exitType, sizeof(vm->exitType));
    }
  }
  free(doc);
  vm->lastPoll = GetTickCount();
}

// opens the VM's serial port, which the HCS makes as the VM starts
static bool vmOpenSerial(struct Vm * vm, DWORD * error)
{
  WCHAR * wpipe = widen(vm->pipe);
  const HANDLE serial = CreateFileW(wpipe, GENERIC_READ | GENERIC_WRITE, 0,
      NULL, OPEN_EXISTING, 0, NULL);
  *error = GetLastError();
  free(wpipe);
  if (serial == INVALID_HANDLE_VALUE)
    return false;

  vm->serial = serial;
  return true;
}

// the serial port closed; false unless the VM may open it again
static bool vmSerialClosed(struct Vm * vm)
{
  if (!vm->reopenSerial)
  {
    vm->disconnected = true;
    return false;
  }

  CloseHandle(vm->serial);
  vm->serial = NULL;
  return true;
}

// reads what the serial port has; false once it is gone
static bool vmPump(struct Vm * vm)
{
  if (vm->disconnected)
    return false;

  if (!vm->serial)
  {
    DWORD error;
    if (!vm->reopenSerial || !vmOpenSerial(vm, &error))
      return vm->reopenSerial;
    ++vm->serialReopened;
  }

  DWORD available = 0;
  if (!PeekNamedPipe(vm->serial, NULL, 0, NULL, &available, NULL))
    return vmSerialClosed(vm);

  while (available)
  {
    char  buf[4096];
    DWORD got = 0;
    if (!ReadFile(vm->serial, buf, min(available, (DWORD)sizeof(buf)), &got,
          NULL) || !got)
      return vmSerialClosed(vm);
    available -= got;

    if (vm->log)
    {
      fwrite(buf, 1, got, vm->log);
      fflush(vm->log);
    }

    // a line longer than the buffer is dropped, the log keeps it
    if (vm->pendingLen + got > sizeof(vm->pending))
      vm->pendingLen = 0;
    memcpy(vm->pending + vm->pendingLen, buf,
        min((size_t)got, sizeof(vm->pending)));
    vm->pendingLen += min((size_t)got, sizeof(vm->pending));
  }
  return true;
}

// the next reply from the guest, without its "LGSHM " prefix
static bool vmNextReply(struct Vm * vm, char * reply, size_t size)
{
  for(;;)
  {
    char * end = memchr(vm->pending, '\n', vm->pendingLen);
    if (!end)
      return false;

    *end = '\0';
    if (end > vm->pending && end[-1] == '\r')
      end[-1] = '\0';

    if (strstr(vm->pending, "Kernel panic"))
      vm->panicked = true;

    const char * at = strstr(vm->pending, "LGSHM ");
    if (at)
    {
      // the report is UTF-8, and a garbled line must not break it
      snprintf(reply, size, "%s", at + 6);
      for(char * p = reply; *p; ++p)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e)
          *p = '?';

      if (vm->replies++ < 400)
      {
        if (vm->transcript.len)
          strAdd(&vm->transcript, "\n", 1);
        strAdd(&vm->transcript, reply, strlen(reply));
      }
    }

    const size_t used = end + 1 - vm->pending;
    memmove(vm->pending, end + 1, vm->pendingLen - used);
    vm->pendingLen -= used;
    if (at)
      return true;
  }
}

// waits for a reply that starts with prefix
static bool vmWait(struct Vm * vm, const char * prefix, DWORD timeoutMs,
    char * reply, size_t size)
{
  const DWORD  start = GetTickCount();
  const char * why   = NULL;
  for(;;)
  {
    const bool connected = vmPump(vm);
    while (vmNextReply(vm, reply, size))
      if (strncmp(reply, prefix, strlen(prefix)) == 0)
        return true;

    // what arrived before the end was read above
    if (why)
    {
      snprintf(reply, size, "%s", why);
      return false;
    }

    why = aborted ? "aborted" :
      vm->panicked ? "guest kernel panic" :
      vm->stopped ? "VM stopped" :
      !connected ? "serial port closed" :
      GetTickCount() - start > timeoutMs ? "timed out" : NULL;

    if (!why && GetTickCount() - vm->lastPoll > 500)
      vmPollState(vm);
    if (!why)
      Sleep(5);
  }
}

// sends a command; true when the reply is the prefix alone or followed by ok
static bool vmCommand(struct Vm * vm, const char * command,
    const char * prefix, DWORD timeoutMs, char * reply, size_t size)
{
  char  line[256];
  DWORD written;
  const int len = snprintf(line, sizeof(line), "%s\n", command);
  if (!vm->serial || !WriteFile(vm->serial, line, len, &written, NULL) ||
      written != (DWORD)len)
  {
    snprintf(reply, size, "cannot write to the serial port");
    return false;
  }

  if (!vmWait(vm, prefix, timeoutMs, reply, size))
    return false;

  const char * rest = reply + strlen(prefix);
  return !*rest || strncmp(rest, " ok", 3) == 0;
}

static void vmConfig(struct Vm * vm, struct Str * config, unsigned minor,
    const char * devices)
{
  const bool windows = vm->options->windowsDisk[0];
  struct Str s = { 0 };
  strPrintf(&s, "{\"SchemaVersion\":{\"Major\":2,\"Minor\":%u},", minor);
  strLiteral(&s, "\"Owner\":\"LookingGlass.HcsProbe\",");
  strLiteral(&s, "\"ShouldTerminateOnLastHandleClosed\":true,");
  if (windows)
  {
    // Windows restarts as it sets itself up, and boots from its disk with
    // Secure Boot, as in Hyper-V Manager's generation 2 VMs
    strLiteral(&s, "\"VirtualMachine\":{\"Chipset\":{\"Uefi\":{"
        "\"ApplySecureBootTemplate\":\"Apply\",\"SecureBootTemplateId\":\""
        WINDOWS_SECURE_BOOT "\"}},");
  }
  else
  {
    strLiteral(&s, "\"VirtualMachine\":{\"StopOnReset\":true,");
    strLiteral(&s, "\"Chipset\":{\"LinuxKernelDirect\":{"
        "\"KernelFilePath\":");
    strJsonString(&s, vm->options->kernel);
    strLiteral(&s, ",\"InitRdPath\":");
    strJsonString(&s, vm->options->initrd);
    strLiteral(&s, ",\"KernelCmdLine\":");
    strJsonString(&s, "console=ttyS0,115200 8250_core.nr_uarts=1 "
        "8250_core.skip_txen_test=1 panic=-1 rdinit=/init");
    strLiteral(&s, "}},");
  }
  // the memory is backed by the worker's virtual memory: with physically
  // backed memory and a SharedMemory region, the Dynamic Memory Controller
  // does not initialize (0x80070032) and the HCS does not create the VM.
  // With this memory and a region, the VM fails to reset as a Windows
  // guest restarts (the controller's post reset fails, 0x8007054F) and
  // stops, so runWindows starts it again
  strLiteral(&s, "\"ComputeTopology\":{");
  strPrintf(&s, "\"Memory\":{\"SizeInMB\":%d,\"AllowOvercommit\":true},",
      windows ? WINDOWS_MEMORY_MB : VM_MEMORY_MB);
  strLiteral(&s, "\"Processor\":{\"Count\":2}},");
  strLiteral(&s, "\"Devices\":{\"ComPorts\":{\"0\":{\"NamedPipe\":");
  strJsonString(&s, vm->pipe);
  strLiteral(&s, "}}");
  if (windows)
  {
    strLiteral(&s, ",\"Scsi\":{\"0\":{\"Attachments\":{\"0\":{"
        "\"Type\":\"VirtualDisk\",\"Path\":");
    strJsonString(&s, vm->options->windowsDisk);
    strLiteral(&s, "}}}},\"VideoMonitor\":{},\"Keyboard\":{},"
        "\"Mouse\":{}");
  }
  if (devices)
  {
    strLiteral(&s, ",");
    strAdd(&s, devices, strlen(devices));
  }
  strLiteral(&s, "}}}");
  *config = s;
}

static void vmInit(struct Vm * vm, const struct Options * options)
{
  memset(vm, 0, sizeof(*vm));
  vm->options = options;

  GUID guid;
  newGuid(&guid, vm->id, sizeof(vm->id));
  snprintf(vm->pipe, sizeof(vm->pipe), "\\\\.\\pipe\\lg-hcs-probe-%s",
      vm->id);
}

// lets the VM worker read what it boots: the probe's own copies of the
// kernel and the initrd, or the Windows guest's disk
static void vmAccess(struct Vm * vm, bool grant, struct Json * report)
{
  struct File
  {
    const char * path, * granted, * revoked;
  };

  const struct File bootFiles[] =
  {
    { vm->options->kernel, "grant_kernel", "revoke_kernel" },
    { vm->options->initrd, "grant_initrd", "revoke_initrd" }
  };
  const struct File diskFiles[] =
  {
    { vm->options->windowsDisk, "grant_disk", "revoke_disk" }
  };

  const bool windows = vm->options->windowsDisk[0];
  const struct File * files = windows ? diskFiles : bootFiles;
  const size_t count = windows ? 1 : 2;

  WCHAR * id = widen(vm->id);
  for(size_t i = 0; i < count; ++i)
  {
    WCHAR * path = widen(files[i].path);
    const HRESULT hr = grant ?
      api.grantVmAccess(id, path) : api.revokeVmAccess(id, path);
    jsonResult(report, grant ? files[i].granted : files[i].revoked, hr);
    free(path);
  }
  free(id);
}

// the HCS's events about a VM, kept for the report; this runs on the HCS's
// threads until the VM is closed
static void CALLBACK vmEvent(HCS_EVENT * event, void * context)
{
  struct Vm * vm = context;
  const LONG slot = InterlockedIncrement(&vm->eventCount) - 1;
  if (slot >= (LONG)(sizeof(vm->events) / sizeof(vm->events[0])))
    return;

  const char * name =
    event->Type == HcsEventSystemExited          ? "system exited"     :
    event->Type == HcsEventSystemCrashInitiated  ? "crash initiated"   :
    event->Type == HcsEventSystemCrashReport     ? "crash report"      :
    event->Type == HcsEventGuestConnectionClosed ? "guest connection closed" :
    event->Type == HcsEventServiceDisconnect     ? "service disconnect" :
    "other";

  char * data = event->EventData ? narrow(event->EventData) : NULL;
  struct Str text = { 0 };
  strPrintf(&text, "%s (0x%08lx): %s", name, (unsigned long)event->Type,
      data ? data : "");
  free(data);
  vm->events[slot] = text.data;
}

static bool vmCreate(struct Vm * vm, const char * config,
    struct Json * report, const char * logPath)
{
  jsonString(report, "vm_id", vm->id);
  jsonRaw(report, "config", config);

  char * doc = NULL;
  const HRESULT hr = hcsCreate(vm->id, config, &vm->system, &doc);
  jsonResult(report, "create", hr);
  jsonString(report, "create_result", doc);
  if (FAILED(hr))
  {
    printf("  The HCS refused the VM: 0x%08lx %s\n", (unsigned long)hr,
        doc ? doc : "");
    free(doc);
    return false;
  }
  free(doc);

  if (api.setComputeSystemCallback)
    jsonResult(report, "event_callback", api.setComputeSystemCallback(
          vm->system, 0, vm, vmEvent));
  vmAccess(vm, true, report);

  WCHAR * wlog = widen(logPath);
  vm->log = _wfopen(wlog, L"wb");
  free(wlog);
  jsonString(report, "serial_log", logPath);
  return true;
}

static bool vmStart(struct Vm * vm, struct Json * report)
{
  char * doc = NULL;
  const HRESULT hr = hcsCall(vm->system, api.startComputeSystem, NULL, 60000,
      &doc);
  jsonResult(report, "start", hr);
  jsonString(report, "start_result", doc);
  if (FAILED(hr))
  {
    printf("  The VM did not start: 0x%08lx %s\n", (unsigned long)hr,
        doc ? doc : "");
    free(doc);
    return false;
  }
  free(doc);

  // the HCS creates the pipe as the VM starts; output before this is lost
  const DWORD start = GetTickCount();
  for(;;)
  {
    DWORD error;
    if (vmOpenSerial(vm, &error))
      break;

    if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) ||
        GetTickCount() - start > 10000)
    {
      printf("  Cannot open the VM's serial port: error %lu\n", error);
      jsonString(report, "error", "cannot open the serial port");
      return false;
    }
    Sleep(20);
  }
  return true;
}

// stops the VM, through the guest when it is up
static void vmStop(struct Vm * vm, struct Json * report)
{
  char reply[256];
  bool off = false;
  if (vm->ready && !vm->stopped && !aborted &&
      vmCommand(vm, "off", "off", 10000, reply, sizeof(reply)))
  {
    off = true;
    const DWORD start = GetTickCount();
    while (!vm->stopped && !aborted && GetTickCount() - start < 20000)
    {
      vmPump(vm);
      vmPollState(vm);
      if (!vm->stopped)
        Sleep(200);
    }
  }

  vmPump(vm);
  while (vmNextReply(vm, reply, sizeof(reply)));

  // a VM can stop by itself, such as when its worker fails
  if (!vm->stopped)
    vmPollState(vm);

  jsonBool(report, "stopped_by_guest"  ,  off && vm->stopped);
  jsonBool(report, "stopped_on_its_own", !off && vm->stopped);
  jsonString(report, "exit_type", vm->exitType[0] ? vm->exitType : NULL);
  if (!vm->stopped)
    jsonResult(report, "terminate", hcsCall(vm->system,
          api.terminateComputeSystem, NULL, 10000, NULL));
}

// closes the VM and checks the HCS no longer has it
static void vmDestroy(struct Vm * vm, struct Json * report)
{
  if (vm->system)
  {
    if (api.setComputeSystemCallback)
      api.setComputeSystemCallback(vm->system, 0, NULL, NULL);
    api.closeComputeSystem(vm->system);
    vm->system = NULL;
    vmAccess(vm, false, report);
  }

  // no event arrives once the VM is closed
  const LONG events = min(vm->eventCount,
      (LONG)(sizeof(vm->events) / sizeof(vm->events[0])));
  jsonOpen(report, "hcs_events", '[');
  for(LONG i = 0; i < events; ++i)
  {
    jsonString(report, NULL, vm->events[i]);
    free(vm->events[i]);
    vm->events[i] = NULL;
  }
  jsonClose(report, ']');

  if (vm->serial)
    CloseHandle(vm->serial);
  vm->serial = NULL;

  if (vm->log)
    fclose(vm->log);
  vm->log = NULL;

  jsonString(report, "guest", vm->transcript.len ? vm->transcript.data :
      NULL);
  free(vm->transcript.data);
  vm->transcript = (struct Str){ 0 };

  // a closed VM can take a moment to leave the list
  HRESULT hr;
  bool    listed;
  for(int i = 0;; ++i)
  {
    char * doc = NULL;
    hr     = hcsEnumerate(&doc);
    listed = SUCCEEDED(hr) && doc && strstr(doc, vm->id);
    free(doc);
    if (!listed || i == 49)
      break;
    Sleep(200);
  }

  jsonResult(report, "cleanup_enumerate", hr);
  jsonBool(report, "still_listed", listed);
  jsonBool(report, "cleanup_verified", SUCCEEDED(hr) && !listed);
  if (listed)
    printf("  The HCS still lists VM %s\n", vm->id);
  else if (FAILED(hr))
    printf("  Cannot check that the HCS dropped VM %s: 0x%08lx\n", vm->id,
        (unsigned long)hr);
}

// the checks both cases run once the guest has mapped the memory
static bool guestChecks(struct Vm * vm, struct Section * section,
    uint64_t nonce, struct Json * report)
{
  char command[160], reply[512];
  const uint64_t pages = section->size / PAGE;

  snprintf(command, sizeof(command), "verify 0x%" PRIx64 " 0x%" PRIx64
      " 0x%" PRIx64, PROBE_MAGIC, nonce, pages);
  const bool verified = vmCommand(vm, command, "verify", 120000, reply,
      sizeof(reply));
  jsonString(report, "verify", reply);
  printf("  %s: %s\n", verified ?
      "The guest reads the host's pattern on every page" :
      "The guest does not read the host's pattern", reply);
  if (!verified)
    return false;

  // guest to host
  const uint64_t page  = pages / 2 + 1;
  const uint64_t value = random64() | 1;
  snprintf(command, sizeof(command), "write 0x%" PRIx64 " 0x%" PRIx64, page,
      value);
  bool guestWrite = vmCommand(vm, command, "write", 10000, reply,
      sizeof(reply));
  MemoryBarrier();
  const uint64_t seen = *sectionWord(section, page, 2);
  guestWrite = guestWrite && seen == value;
  jsonBool(report, "guest_write_seen_by_host", guestWrite);
  printf("  %s (wrote 0x%016" PRIx64 ", host read 0x%016" PRIx64 ")\n",
      guestWrite ? "The host sees what the guest wrote" :
      "The host does not see what the guest wrote", value, seen);

  // host to guest
  const uint64_t last  = pages - 1;
  const uint64_t value2 = random64() | 1;
  *sectionWord(section, last, 2) = value2;
  MemoryBarrier();
  snprintf(command, sizeof(command), "read 0x%" PRIx64, last);
  bool hostWrite = vmCommand(vm, command, "read", 10000, reply,
      sizeof(reply));
  uint64_t got = 0;
  const char * at = strstr(reply, "value=");
  hostWrite = hostWrite && at && (got = strtoull(at + 6, NULL, 16)) == value2;
  jsonBool(report, "host_write_seen_by_guest", hostWrite);
  printf("  %s (wrote 0x%016" PRIx64 ", guest read 0x%016" PRIx64 ")\n",
      hostWrite ? "The guest sees what the host wrote" :
      "The guest does not see what the host wrote", value2, got);

  return guestWrite && hostWrite;
}

static bool guestReadyWithin(struct Vm * vm, struct Json * report,
    DWORD timeoutMs)
{
  char reply[512];
  const bool ready = vmWait(vm, "ready", timeoutMs, reply, sizeof(reply));
  jsonBool(report, "guest_ready", ready);
  if (!ready)
  {
    printf("  The guest did not come up: %s\n", reply);
    return false;
  }

  vm->ready = true;
  vmCommand(vm, "info", "info end", 20000, reply, sizeof(reply));
  return true;
}

static bool guestReady(struct Vm * vm, struct Json * report)
{
  return guestReadyWithin(vm, report, vm->options->bootTimeoutMs);
}

// the emulated IVSHMEM device, as QEMU's ivshmem-plain: BAR0 holds the
// registers, which this probe leaves unused, and BAR2 the shared memory
#define IVSHMEM_LOG_MAX  96
#define IVSHMEM_LOG_LINE 48

struct Ivshmem
{
  CRITICAL_SECTION lock;
  uint32_t         config[64];
  struct Section * section;

  // what happened to the device, in order, for the report
  DWORD    created;
  char     log[IVSHMEM_LOG_MAX][IVSHMEM_LOG_LINE];
  unsigned logLen, logDropped;

  volatile LONG initialize, teardown, setConfiguration, getDetails, start,
                stop, configReads, configWrites, bar0Reads, bar0Writes,
                bar2Reads, bar2Writes, otherAccesses, command;
};

static void ivshmemLog(struct Ivshmem * d, const char * fmt, ...)
  __attribute__((format(__MINGW_PRINTF_FORMAT, 2, 3)));

// a line of the device's log, after the milliseconds since it was set up
static void ivshmemLog(struct Ivshmem * d, const char * fmt, ...)
{
  EnterCriticalSection(&d->lock);
  if (d->logLen == IVSHMEM_LOG_MAX)
    ++d->logDropped;
  else
  {
    char * line = d->log[d->logLen++];
    const int len = snprintf(line, IVSHMEM_LOG_LINE, "+%lu ",
        (unsigned long)(GetTickCount() - d->created));
    if (len > 0 && len < IVSHMEM_LOG_LINE)
    {
      va_list ap;
      va_start(ap, fmt);
      vsnprintf(line + len, IVSHMEM_LOG_LINE - len, fmt, ap);
      va_end(ap);
    }
  }
  LeaveCriticalSection(&d->lock);
}

static uint32_t barMask(const struct Ivshmem * d, unsigned bar)
{
  switch (bar)
  {
    case 0: return ~(uint32_t)(IVSHMEM_REGS_SIZE - 1);
    case 2: return (uint32_t)~(d->section->size - 1) & ~0xfU;
    case 3: return (uint32_t)(~(d->section->size - 1) >> 32);
    default: return 0;
  }
}

// BAR2 is 64-bit prefetchable memory, the others 32-bit memory
static uint32_t barType(unsigned bar)
{
  return bar == 2 ? 0xc : 0;
}

static HRESULT CALLBACK ivshmemInitialize(void * context)
{
  struct Ivshmem * d = context;
  InterlockedIncrement(&d->initialize);
  ivshmemLog(d, "initialize");
  return S_OK;
}

static void CALLBACK ivshmemTeardown(void * context)
{
  struct Ivshmem * d = context;
  InterlockedIncrement(&d->teardown);
  ivshmemLog(d, "teardown");
}

static HRESULT CALLBACK ivshmemSetConfiguration(void * context, UINT32 count,
    const PCWSTR * values)
{
  struct Ivshmem * d = context;
  InterlockedIncrement(&d->setConfiguration);
  ivshmemLog(d, "set configuration, %u values", (unsigned)count);
  return S_OK;
}

static HRESULT CALLBACK ivshmemGetDetails(void * context,
    HDV_PCI_PNP_ID * pnpId, UINT32 probedBarsCount, UINT32 * probedBars)
{
  struct Ivshmem * d = context;
  InterlockedIncrement(&d->getDetails);
  ivshmemLog(d, "get details, %u BARs", (unsigned)probedBarsCount);

  *pnpId = (HDV_PCI_PNP_ID)
  {
    .VendorID    = IVSHMEM_VENDOR,
    .DeviceID    = IVSHMEM_DEVICE,
    .RevisionID  = 1,
    .ProgIf      = 0,
    .SubClass    = 0x00,
    .BaseClass   = 0x05,
    .SubVendorID = IVSHMEM_VENDOR,
    .SubSystemID = IVSHMEM_SUBSYSTEM
  };

  // what each BAR reads back after the guest writes all ones to it
  for(UINT32 i = 0; i < probedBarsCount; ++i)
    probedBars[i] = i < 6 ? barMask(d, i) | barType(i) : 0;
  return S_OK;
}

static HRESULT CALLBACK ivshmemStart(void * context)
{
  struct Ivshmem * d = context;
  InterlockedIncrement(&d->start);
  ivshmemLog(d, "start");
  return S_OK;
}

static void CALLBACK ivshmemStop(void * context)
{
  struct Ivshmem * d = context;
  InterlockedIncrement(&d->stop);
  ivshmemLog(d, "stop");
}

static HRESULT CALLBACK ivshmemReadConfig(void * context, UINT32 offset,
    UINT32 * value)
{
  struct Ivshmem * d = context;
  InterlockedIncrement(&d->configReads);

  EnterCriticalSection(&d->lock);
  *value = offset < sizeof(d->config) ? d->config[offset / 4] : 0;
  LeaveCriticalSection(&d->lock);
  ivshmemLog(d, "config read  %03x: %08x", (unsigned)offset,
      (unsigned)*value);
  return S_OK;
}

static HRESULT CALLBACK ivshmemWriteConfig(void * context, UINT32 offset,
    UINT32 value)
{
  struct Ivshmem * d = context;
  InterlockedIncrement(&d->configWrites);
  ivshmemLog(d, "config write %03x: %08x", (unsigned)offset,
      (unsigned)value);
  if (offset >= sizeof(d->config))
    return S_OK;

  EnterCriticalSection(&d->lock);
  uint32_t * reg = &d->config[offset / 4];
  switch (offset & ~3U)
  {
    // the status half has only write-one-to-clear bits, all clear
    case 0x04:
      *reg = value & 0x0547;
      InterlockedExchange(&d->command, *reg);
      break;

    case 0x10: case 0x14: case 0x18: case 0x1c: case 0x20: case 0x24:
    {
      const unsigned bar = ((offset & ~3U) - 0x10) / 4;
      *reg = (value & barMask(d, bar)) | barType(bar);
      break;
    }

    case 0x3c:
      *reg = (*reg & ~0xffU) | (value & 0xff);
      break;
  }
  LeaveCriticalSection(&d->lock);
  return S_OK;
}

// accesses the HCS forwards here instead of mapping; BAR2 is served from the
// section so that its contents stay right, and counted to tell the two apart
static HRESULT CALLBACK ivshmemReadMemory(void * context,
    HDV_PCI_BAR_SELECTOR bar, UINT64 offset, UINT64 length, BYTE * value)
{
  struct Ivshmem * d = context;
  LONG count;
  if (bar == HDV_PCI_BAR2 && offset <= d->section->size &&
      length <= d->section->size - offset)
  {
    count = InterlockedIncrement(&d->bar2Reads);
    memcpy(value, d->section->view + offset, length);
  }
  else
  {
    count = InterlockedIncrement(bar == HDV_PCI_BAR0 ? &d->bar0Reads :
        &d->otherAccesses);
    memset(value, 0, length);
  }

  if (count <= 4)
    ivshmemLog(d, "read  BAR%d +%" PRIx64 ", %" PRIu64 " bytes", (int)bar,
        (uint64_t)offset, (uint64_t)length);
  return S_OK;
}

static HRESULT CALLBACK ivshmemWriteMemory(void * context,
    HDV_PCI_BAR_SELECTOR bar, UINT64 offset, UINT64 length,
    const BYTE * value)
{
  struct Ivshmem * d = context;
  LONG count;
  if (bar == HDV_PCI_BAR2 && offset <= d->section->size &&
      length <= d->section->size - offset)
  {
    count = InterlockedIncrement(&d->bar2Writes);
    memcpy(d->section->view + offset, value, length);
  }
  else
    count = InterlockedIncrement(bar == HDV_PCI_BAR0 ? &d->bar0Writes :
        &d->otherAccesses);

  if (count <= 4)
    ivshmemLog(d, "write BAR%d +%" PRIx64 ", %" PRIu64 " bytes", (int)bar,
        (uint64_t)offset, (uint64_t)length);
  return S_OK;
}

static const HDV_PCI_DEVICE_INTERFACE ivshmemInterface =
{
  .Version                = HdvPciDeviceInterfaceVersion1,
  .Initialize             = ivshmemInitialize,
  .Teardown               = ivshmemTeardown,
  .SetConfiguration       = ivshmemSetConfiguration,
  .GetDetails             = ivshmemGetDetails,
  .Start                  = ivshmemStart,
  .Stop                   = ivshmemStop,
  .ReadConfigSpace        = ivshmemReadConfig,
  .WriteConfigSpace       = ivshmemWriteConfig,
  .ReadInterceptedMemory  = ivshmemReadMemory,
  .WriteInterceptedMemory = ivshmemWriteMemory
};

static void ivshmemInit(struct Ivshmem * d, struct Section * section)
{
  memset(d, 0, sizeof(*d));
  InitializeCriticalSection(&d->lock);
  d->section    = section;
  d->created    = GetTickCount();
  d->config[0]  = IVSHMEM_DEVICE << 16 | IVSHMEM_VENDOR;
  d->config[2]  = 0x05 << 24 | 0x00 << 16 | 0 << 8 | 1; // RAM, revision 1
  d->config[6]  = barType(2);
  d->config[11] = IVSHMEM_SUBSYSTEM << 16 | IVSHMEM_VENDOR;
}

static void ivshmemReport(struct Ivshmem * d, struct Json * report)
{
  jsonOpen(report, "device_calls", '{');
  jsonNumber(report, "initialize"       , d->initialize      );
  jsonNumber(report, "set_configuration", d->setConfiguration);
  jsonNumber(report, "get_details"      , d->getDetails      );
  jsonNumber(report, "start"            , d->start           );
  jsonNumber(report, "stop"             , d->stop            );
  jsonNumber(report, "teardown"         , d->teardown        );
  jsonNumber(report, "config_reads"     , d->configReads     );
  jsonNumber(report, "config_writes"    , d->configWrites    );
  jsonNumber(report, "command"          , d->command         );
  jsonNumber(report, "bar0_reads"       , d->bar0Reads       );
  jsonNumber(report, "bar0_writes"      , d->bar0Writes      );
  jsonNumber(report, "bar2_intercepted_reads" , d->bar2Reads );
  jsonNumber(report, "bar2_intercepted_writes", d->bar2Writes);
  jsonNumber(report, "other_accesses"   , d->otherAccesses   );
  jsonClose(report, '}');

  // where the guest placed the BARs, as far as the device was told
  EnterCriticalSection(&d->lock);
  jsonNumber(report, "bar0_address", d->config[4] & ~0xfU);
  jsonNumber(report, "bar2_address",
      (uint64_t)d->config[7] << 32 | (d->config[6] & ~0xfU));
  jsonOpen(report, "device_log", '[');
  for(unsigned i = 0; i < d->logLen; ++i)
    jsonString(report, NULL, d->log[i]);
  jsonClose(report, ']');
  jsonNumber(report, "device_log_dropped", d->logDropped);
  LeaveCriticalSection(&d->lock);
}

// how an attempt with one VM ended
enum AttemptResult
{
  ATTEMPT_REFUSED, // the HCS did not take the VM's configuration
  ATTEMPT_FAILED,
  ATTEMPT_PASSED
};

// whether a peek reply holds the host's pattern for a page
static bool peekShows(const char * reply, uint64_t page, uint64_t nonce)
{
  const char * w0 = strstr(reply, "w0=");
  const char * w1 = strstr(reply, "w1=");
  return w0 && w1 &&
    strtoull(w0 + 3, NULL, 16) == (PROBE_MAGIC ^ page) &&
    strtoull(w1 + 3, NULL, 16) == (nonce ^ (page * GOLDEN));
}

// the guest reads the start of a BAR of the IVSHMEM device where it placed
// it; true when that is page 0 of the host's pattern
static bool guestPeekBar(struct Vm * vm, unsigned bar, uint64_t nonce,
    const char * key, struct Json * report)
{
  char command[96], reply[512];
  snprintf(command, sizeof(command), "peek pci 0x%x 0x%x 0x%x 0x0",
      IVSHMEM_VENDOR, IVSHMEM_DEVICE, bar);
  const bool shows = vmCommand(vm, command, "peek", 20000, reply,
      sizeof(reply)) && peekShows(reply, 0, nonce);

  char patternKey[64];
  snprintf(patternKey, sizeof(patternKey), "%s_pattern", key);
  jsonString(report, key, reply);
  jsonBool(report, patternKey, shows);
  printf("  %s: %s%s\n", command, reply, shows ? ", the host's pattern" :
      "");
  return shows;
}

// a register of the IVSHMEM device's configuration space, through the
// guest, after writing *write to it if given
static bool guestConfig(struct Vm * vm, unsigned offset, unsigned size,
    const uint64_t * write, uint64_t * value, const char * key,
    struct Json * report)
{
  char valueText[24] = "", command[128], reply[512];
  if (write)
    snprintf(valueText, sizeof(valueText), " 0x%" PRIx64, *write);
  snprintf(command, sizeof(command), "config 0x%x 0x%x 0x%x 0x%x%s",
      IVSHMEM_VENDOR, IVSHMEM_DEVICE, offset, size, valueText);

  const bool ok = vmCommand(vm, command, "config", 20000, reply,
      sizeof(reply));
  jsonString(report, key, reply);
  printf("  %s: %s\n", command, reply);

  const char * at = strstr(reply, "value=");
  if (!ok || !at)
    return false;
  if (value)
    *value = strtoull(at + 6, NULL, 16);
  return true;
}

// a device host for the VM, with the COM security that lets the VM worker
// call back, which a process sets only once
static HRESULT hdvHost(HCS_SYSTEM system, HDV_HOST * host)
{
  static bool comSecurity = false;

  HRESULT hr;
  if (api.initializeDeviceHostEx)
  {
    hr = api.initializeDeviceHostEx(system, comSecurity ?
        HdvDeviceHostFlagNone : HdvDeviceHostFlagInitializeComSecurity, host);

    // maybe set already by a failed attempt
    if (hr == RPC_E_TOO_LATE && !comSecurity)
      hr = api.initializeDeviceHostEx(system, HdvDeviceHostFlagNone, host);
    comSecurity = comSecurity || SUCCEEDED(hr);
  }
  else
    hr = api.initializeDeviceHost(system, host);

  if (FAILED(hr))
    *host = NULL;
  return hr;
}

static HRESULT hdvMap(HDV_DEVICE handle, struct Section * section)
{
  return api.createSectionBackedMmioRange(handle, HDV_PCI_BAR2, 0,
      section->size / PAGE, HdvMmioMappingFlagWriteable, section->handle, 0);
}

// backs BAR2 by the section once the guest is up and has placed the BAR,
// reading page 0 through the guest after each step that could make the
// mapping take effect
static void hdvMapLate(struct Vm * vm, struct Ivshmem * device,
    HDV_DEVICE handle, struct Section * section, uint64_t nonce,
    struct Json * report, bool * mapped)
{
  // the device's handlers serve both BARs until then
  LONG before = device->bar2Reads;
  guestPeekBar(vm, 2, nonce, "bar2_before_mapping", report);
  jsonNumber(report, "bar2_before_mapping_intercepts",
      device->bar2Reads - before);
  before = device->bar0Reads;
  guestPeekBar(vm, 0, nonce, "bar0", report);
  jsonNumber(report, "bar0_intercepts", device->bar0Reads - before);

  const HRESULT hr = hdvMap(handle, section);
  jsonResult(report, "bar2_mapping_late", hr);
  ivshmemLog(device, "BAR2 mapping: %08lx", (unsigned long)hr);
  printf("  Backing BAR2 by the section once the guest is up: 0x%08lx\n",
      (unsigned long)hr);
  *mapped = SUCCEEDED(hr);
  if (!*mapped || aborted ||
      guestPeekBar(vm, 2, nonce, "bar2_after_mapping", report))
    return;

  // memory decoding off and on again, which may make the host map the BAR
  uint64_t command, off;
  if (!guestConfig(vm, 0x4, 2, NULL, &command, "command", report))
    return;
  off = command & ~(uint64_t)2;
  ivshmemLog(device, "decoding off and on");
  if (!guestConfig(vm, 0x4, 2, &off, NULL, "decode_off", report) ||
      !guestConfig(vm, 0x4, 2, &command, NULL, "decode_on", report) ||
      guestPeekBar(vm, 2, nonce, "bar2_after_decode", report))
    return;

  // the BAR's address written again while decoding is off
  uint64_t low, high;
  ivshmemLog(device, "BAR2 written again");
  if (guestConfig(vm, 0x18, 4, NULL, &low , "bar2_low" , report) &&
      guestConfig(vm, 0x1c, 4, NULL, &high, "bar2_high", report) &&
      guestConfig(vm, 0x4 , 2, &off    , NULL, "rebar_decode_off", report) &&
      guestConfig(vm, 0x18, 4, &low    , NULL, "rebar_low"       , report) &&
      guestConfig(vm, 0x1c, 4, &high   , NULL, "rebar_high"      , report) &&
      guestConfig(vm, 0x4 , 2, &command, NULL, "rebar_decode_on" , report))
    guestPeekBar(vm, 2, nonce, "bar2_after_rebar", report);
}

// when the probe backs BAR2 by the section: as soon as the device host
// takes it after the VM starts, before the guest places the BAR, or once
// the guest is up, checking each step
enum HdvMapping
{
  HDV_MAP_EARLY,
  HDV_MAP_LATE
};

static enum AttemptResult hdvAttempt(const struct Options * options,
    enum HdvMapping mapping, unsigned schemaMinor, struct Section * section,
    struct Json * report)
{
  struct Vm vm;
  vmInit(&vm, options);

  GUID instance;
  char instanceText[40], classText[40];
  newGuid(&instance, instanceText, sizeof(instanceText));
  guidText(&emulatorClass, classText, sizeof(classText));

  // the HCS offers the guest only a device its configuration declares
  struct Str devices = { 0 };
  strPrintf(&devices, "\"FlexibleIov\":{\"%s\":{\"EmulatorId\":\"%s\","
      "\"HostingModel\":\"External\"}}", instanceText, classText);

  char variant[32];
  snprintf(variant, sizeof(variant), "%s-2.%u",
      mapping == HDV_MAP_EARLY ? "early" : "late", schemaMinor);

  jsonOpen(report, NULL, '{');
  jsonString(report, "variant", variant);
  jsonString(report, "device_instance", instanceText);
  printf("[hdv] VM %s with the emulated IVSHMEM device, BAR2 backed by the "
      "section %s, schema 2.%u\n", vm.id, mapping == HDV_MAP_EARLY ?
      "as soon as the VM runs" : "once the guest is up", schemaMinor);

  const uint64_t nonce = random64();
  sectionFill(section, nonce);

  struct Ivshmem device;
  ivshmemInit(&device, section);

  struct Str config = { 0 };
  vmConfig(&vm, &config, schemaMinor, devices.data);
  free(devices.data);

  char logName[OUT_NAME_MAX], logPath[MAX_PATH * 3];
  snprintf(logName, sizeof(logName), "hdv-%s.serial.log", variant);
  outPath(options, logName, logPath, sizeof(logPath));

  HDV_HOST   host   = NULL;
  HDV_DEVICE handle = NULL;
  bool mapped = false, direct = false;
  enum AttemptResult result = ATTEMPT_REFUSED;
  char reply[512];

  if (!vmCreate(&vm, config.data, report, logPath))
    goto done;
  result = ATTEMPT_FAILED;

  HRESULT hr = hdvHost(vm.system, &host);
  jsonResult(report, "device_host", hr);
  if (FAILED(hr))
  {
    printf("  HdvInitializeDeviceHost failed: 0x%08lx\n", (unsigned long)hr);
    goto done;
  }

  hr = api.createDeviceInstance(host, HdvDeviceTypePCI, &emulatorClass,
      &instance, &ivshmemInterface, &device, &handle);
  jsonResult(report, "device", hr);
  if (FAILED(hr))
  {
    printf("  HdvCreateDeviceInstance failed: 0x%08lx\n", (unsigned long)hr);
    handle = NULL;
    goto done;
  }

  // Windows 10.0.26100 refuses this until the VM runs
  hr = hdvMap(handle, section);
  jsonResult(report, "bar2_mapping", hr);
  ivshmemLog(&device, "BAR2 mapping before the start: %08lx",
      (unsigned long)hr);
  mapped = SUCCEEDED(hr);

  if (!vmStart(&vm, report))
    goto done;
  ivshmemLog(&device, "VM started");

  if (!mapped && mapping == HDV_MAP_EARLY)
  {
    const DWORD start = GetTickCount();
    while ((hr = hdvMap(handle, section)) ==
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE) && !aborted &&
        GetTickCount() - start < 20000)
      Sleep(1);
    mapped = SUCCEEDED(hr);
    jsonResult(report, "bar2_mapping_early", hr);
    jsonNumber(report, "bar2_mapping_ms", GetTickCount() - start);
    ivshmemLog(&device, "BAR2 mapping: %08lx", (unsigned long)hr);
    printf("  Backing BAR2 by the section as the VM runs: 0x%08lx after "
        "%lu ms\n", (unsigned long)hr, GetTickCount() - start);
  }

  if (!guestReady(&vm, report))
    goto done;

  char command[64];
  snprintf(command, sizeof(command), "map pci 0x%x 0x%x 0x2", IVSHMEM_VENDOR,
      IVSHMEM_DEVICE);
  const bool guestMapped = vmCommand(&vm, command, "map", 45000, reply,
      sizeof(reply));
  jsonString(report, "map", reply);
  printf("  %s: %s\n", guestMapped ?
      "The guest found the IVSHMEM device and mapped BAR2" :
      "The guest could not map the IVSHMEM device", reply);
  if (!guestMapped)
    goto done;

  if (!mapped)
    hdvMapLate(&vm, &device, handle, section, nonce, report, &mapped);
  if (aborted)
    goto done;

  const LONG readsBefore  = device.bar2Reads;
  const LONG writesBefore = device.bar2Writes;
  const bool checked = guestChecks(&vm, section, nonce, report);
  const bool intercepted =
    device.bar2Reads != readsBefore || device.bar2Writes != writesBefore;
  direct = checked && mapped && !intercepted;
  jsonBool(report, "bar2_intercepted", intercepted);
  jsonBool(report, "bar2_direct", direct);
  printf("  %s\n", direct ?
      "BAR2 is the section itself: no guest access reached the emulator" :
      intercepted ?
      "BAR2 accesses went through the emulator, far too slow for frames" :
      "BAR2 shows neither the section nor the emulator");
  if (checked && direct)
    result = ATTEMPT_PASSED;

done:
  if (vm.system)
    vmStop(&vm, report);
  if (handle && mapped)
    jsonResult(report, "bar2_unmap",
        api.destroySectionBackedMmioRange(handle, HDV_PCI_BAR2, 0));
  if (host)
    jsonResult(report, "device_host_teardown",
        api.teardownDeviceHost(host));
  ivshmemReport(&device, report);
  vmDestroy(&vm, report);
  DeleteCriticalSection(&device.lock);
  free(config.data);

  jsonBool(report, "passed", result == ATTEMPT_PASSED);
  jsonClose(report, '}');
  return result;
}

static bool runHdv(const struct Options * options, struct Json * report)
{
  jsonOpen(report, NULL, '{');
  jsonString(report, "case", "hdv");
  jsonOpen(report, "attempts", '[');

  bool passed = false;
  struct Section section;
  if (!hdvAvailable())
    printf("[hdv] vmdevicehost.dll or its functions are missing\n");
  else if (sectionCreate(&section, NULL, options->size))
  {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // each with the schema of Limiar's probe VMs, then with the newest one
    // in case the older one cannot declare the device
    const enum HdvMapping mappings[] = { HDV_MAP_EARLY, HDV_MAP_LATE };
    for(size_t i = 0; i < sizeof(mappings) / sizeof(mappings[0]) &&
        !passed && !aborted; ++i)
    {
      enum AttemptResult result = hdvAttempt(options, mappings[i],
          SCHEMA_MINOR, &section, report);
      if (result == ATTEMPT_REFUSED && !aborted &&
          options->schemaMinor > SCHEMA_MINOR)
        result = hdvAttempt(options, mappings[i], options->schemaMinor,
            &section, report);
      passed = result == ATTEMPT_PASSED;
    }
    sectionClose(&section);
  }

  jsonClose(report, ']');
  jsonBool(report, "available", hdvAvailable());
  jsonBool(report, "passed", passed);
  jsonClose(report, '}');
  return passed;
}

static void addCandidate(uint64_t * list, size_t * count, uint64_t address)
{
  if (!address || address % PAGE)
    return;
  for(size_t i = 0; i < *count; ++i)
    if (list[i] == address)
      return;
  list[(*count)++] = address;
}

// a SharedMemoryRegion as the HCS takes it
static void regionSettings(struct Str * s, const char * sectionName,
    uint64_t size, bool hidden)
{
  strLiteral(s, "{\"SectionName\":");
  strJsonString(s, sectionName);
  strPrintf(s, ",\"StartOffset\":0,\"Length\":%" PRIu64 ","
      "\"AllowGuestWrite\":true,\"HiddenFromGuest\":%s}", size,
      hidden ? "true" : "false");
}

// adds a region to a running VM or removes it
static void regionRequest(struct Str * request, const char * type,
    const char * sectionName, uint64_t size, bool hidden)
{
  strPrintf(request, "{\"ResourcePath\":"
      "\"VirtualMachine/Devices/SharedMemory/Regions\",\"RequestType\":"
      "\"%s\",\"Settings\":", type);
  regionSettings(request, sectionName, size, hidden);
  strLiteral(request, "}");
}

// with hotAdd, the region is added once the guest is up, as it would be to a
// VM that something else created, instead of in the VM's configuration
static enum AttemptResult shmAttempt(const struct Options * options,
    struct Section * section, const char * sectionName, bool hidden,
    bool hotAdd, uint64_t * found, struct Json * report)
{
  struct Vm vm;
  vmInit(&vm, options);

  jsonOpen(report, NULL, '{');
  jsonString(report, "section_name", sectionName);
  jsonBool(report, "hidden_from_guest", hidden);
  jsonBool(report, "hot_add", hotAdd);
  printf("[shm] VM %s with %s %s%s\n", vm.id, sectionName,
      hotAdd ? "added once the guest is up" : "mapped",
      hidden ? ", hidden from the guest's memory map" : "");

  const uint64_t nonce = random64();
  sectionFill(section, nonce);

  struct Str devices = { 0 };
  if (!hotAdd)
  {
    strLiteral(&devices, "\"SharedMemory\":{\"Regions\":[");
    regionSettings(&devices, sectionName, section->size, hidden);
    strLiteral(&devices, "]}");
  }

  struct Str config = { 0 };
  vmConfig(&vm, &config, SCHEMA_MINOR, devices.data);
  free(devices.data);

  char logName[OUT_NAME_MAX], logPath[MAX_PATH * 3];
  snprintf(logName, sizeof(logName), "shm-%s-%s%s.serial.log",
      sectionName[0] == '\\' ? "nt" : "win32", hidden ? "hidden" : "visible",
      hotAdd ? "-hotadd" : "");
  outPath(options, logName, logPath, sizeof(logPath));

  enum AttemptResult result = ATTEMPT_REFUSED;
  char reply[512], command[96];
  bool added = false;
  char * doc = NULL;
  HRESULT hr;

  if (!vmCreate(&vm, config.data, report, logPath) || !vmStart(&vm, report))
    goto done;
  result = ATTEMPT_FAILED;

  if (hotAdd)
  {
    if (!guestReady(&vm, report))
      goto done;

    struct Str request = { 0 };
    regionRequest(&request, "Add", sectionName, section->size, hidden);
    hr = hcsModify(vm.system, request.data, &doc);
    free(request.data);
    jsonResult(report, "region_add", hr);
    jsonString(report, "region_add_result", doc);
    printf("  Adding the region while the guest runs: 0x%08lx %s\n",
        (unsigned long)hr, doc ? doc : "");
    free(doc);
    doc = NULL;
    if (FAILED(hr))
      goto done;
    added = true;
  }

  hr = hcsCall(vm.system, api.getComputeSystemProperties,
      "{\"PropertyTypes\":[\"SharedMemoryRegion\"]}", 10000, &doc);
  jsonResult(report, "region_query", hr);
  jsonRaw(report, "region_info", SUCCEEDED(hr) ? doc : NULL);

  uint64_t gpa = 0;
  const bool located = SUCCEEDED(hr) &&
    jsonGetUInt64(doc, "GuestPhysicalAddress", &gpa);
  free(doc);
  if (located)
    printf("  The HCS reports the section at guest physical 0x%" PRIx64
        "\n", gpa);
  else
    printf("  The HCS did not report where it mapped the section: 0x%08lx\n",
        (unsigned long)hr);

  if (!hotAdd && !guestReady(&vm, report))
    goto done;

  // the reported number as a page, which Windows 10.0.26100 gives, and as
  // an address; else where an earlier VM had the section, or right after
  // the VM's memory
  uint64_t candidates[4];
  size_t count = 0;
  if (located && gpa < (UINT64_C(1) << 40))
    addCandidate(candidates, &count, gpa * PAGE);
  if (located)
    addCandidate(candidates, &count, gpa);
  addCandidate(candidates, &count, *found);
  addCandidate(candidates, &count, (uint64_t)VM_MEMORY_MB << 20);

  uint64_t at = 0;
  jsonOpen(report, "peeks", '[');
  for(size_t i = 0; i < count && !at && !aborted; ++i)
  {
    snprintf(command, sizeof(command), "peek phys 0x%" PRIx64,
        candidates[i]);
    const bool ok = vmCommand(&vm, command, "peek", 20000, reply,
        sizeof(reply));
    jsonString(report, NULL, reply);
    printf("  %s: %s\n", command, reply);
    if (ok && peekShows(reply, 0, nonce))
      at = candidates[i];
  }
  jsonClose(report, ']');

  jsonNumber(report, "region_gpa", at);
  jsonString(report, "gpa_units", !at || !located ? NULL :
      at == gpa * PAGE ? "pages" : at == gpa ? "bytes" : "other");
  if (!at)
  {
    printf("  The guest does not find the section at any of these "
        "addresses\n");
    goto done;
  }
  *found = at;

  snprintf(command, sizeof(command), "map phys 0x%" PRIx64 " 0x%" PRIx64, at,
      section->size);
  const bool guestMapped = vmCommand(&vm, command, "map", 20000, reply,
      sizeof(reply));
  jsonString(report, "map", reply);
  printf("  %s: %s\n", guestMapped ?
      "The guest mapped the region" : "The guest could not map the region",
      reply);

  if (guestMapped && guestChecks(&vm, section, nonce, report))
    result = ATTEMPT_PASSED;

done:
  if (added)
  {
    struct Str request = { 0 };
    regionRequest(&request, "Remove", sectionName, section->size, hidden);
    hr = hcsModify(vm.system, request.data, &doc);
    free(request.data);
    jsonResult(report, "region_remove", hr);
    jsonString(report, "region_remove_result", doc);
    free(doc);
  }
  if (vm.system)
    vmStop(&vm, report);
  vmDestroy(&vm, report);
  free(config.data);

  jsonBool(report, "passed", result == ATTEMPT_PASSED);
  jsonClose(report, '}');
  return result;
}

static bool runShm(const struct Options * options, struct Json * report)
{
  jsonOpen(report, NULL, '{');
  jsonString(report, "case", "shm");
  jsonOpen(report, "attempts", '[');

  GUID guid;
  char name[48], sectionName[96];
  newGuid(&guid, name, sizeof(name));
  snprintf(sectionName, sizeof(sectionName), "Global\\lg-hcs-probe-%s",
      name);

  bool passed = false, hotAddPassed = false;
  struct Section section;
  if (sectionCreate(&section, sectionName, options->size))
  {
    // the name as the kernel has it, which Windows 10.0.26100 wants, then
    // as a Win32 program opens it
    const char * formats[] = { "\\BaseNamedObjects\\%s", "Global\\%s" };
    uint64_t found = 0;
    for(int i = 0; i < 2 && !aborted; ++i)
    {
      char configName[96];
      snprintf(configName, sizeof(configName), formats[i],
          sectionName + strlen("Global\\"));

      enum AttemptResult result = shmAttempt(options, &section, configName,
          false, false, &found, report);
      if (result == ATTEMPT_REFUSED)
        continue;

      // either way the name worked, see how a hidden region behaves too
      passed = result == ATTEMPT_PASSED;
      if (!aborted && shmAttempt(options, &section, configName, true, false,
            &found, report) == ATTEMPT_PASSED)
        passed = true;

      // and whether a running VM takes the region, as one that something
      // else created would have to
      hotAddPassed = !aborted && shmAttempt(options, &section, configName,
          false, true, &found, report) == ATTEMPT_PASSED;
      break;
    }
    sectionClose(&section);
  }

  jsonClose(report, ']');
  printf("[shm] A running VM %s a region added to it\n", hotAddPassed ?
      "takes" : "does not take");
  jsonBool(report, "hot_add_passed", hotAddPassed);
  jsonBool(report, "passed", passed);
  jsonClose(report, '}');
  return passed;
}

static bool containsNoCase(const char * text, const char * part)
{
  const size_t len = strlen(part);
  for(; *text; ++text)
    if (_strnicmp(text, part, len) == 0)
      return true;
  return false;
}

// what the HCS lets this PC do with a VM that it did not create, such as one
// of Hyper-V Manager: open it, create a device host for it, and add a
// SharedMemory region, which the probe removes again. The VM keeps running,
// and no guest checks the region.
static bool runVm(const struct Options * options, const char * systems,
    struct Json * report)
{
  jsonOpen(report, NULL, '{');
  jsonString(report, "case", "vm");
  jsonString(report, "vm_id", options->vm);

  const bool listed = systems && containsNoCase(systems, options->vm);
  jsonBool(report, "listed", listed);
  printf("[vm] %s %s among the compute systems the HCS lists\n", options->vm,
      listed ? "is" : "is not");

  bool added = false, removed = false;
  HCS_SYSTEM system = NULL;
  struct Section section = { 0 };
  char * doc = NULL;

  if (!api.openComputeSystem || !api.modifyComputeSystem)
  {
    printf("  computecore.dll lacks HcsOpenComputeSystem or "
        "HcsModifyComputeSystem\n");
    goto done;
  }

  WCHAR * wid = widen(options->vm);
  HRESULT hr = api.openComputeSystem(wid, GENERIC_ALL, &system);
  free(wid);
  jsonResult(report, "open", hr);
  printf("  Opening it through the HCS: 0x%08lx\n", (unsigned long)hr);
  if (FAILED(hr))
  {
    system = NULL;
    goto done;
  }

  hr = hcsCall(system, api.getComputeSystemProperties, NULL, 10000, &doc);
  jsonResult(report, "properties_query", hr);
  jsonRaw(report, "properties", SUCCEEDED(hr) ? doc : NULL);
  printf("  Its properties: 0x%08lx %s\n", (unsigned long)hr,
      doc ? doc : "");
  free(doc);
  doc = NULL;

  // whether a process of this PC could emulate a device for it
  if (hdvAvailable())
  {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HDV_HOST host;
    hr = hdvHost(system, &host);
    jsonResult(report, "device_host", hr);
    printf("  A device host for it: 0x%08lx\n", (unsigned long)hr);
    if (host)
      jsonResult(report, "device_host_teardown",
          api.teardownDeviceHost(host));
  }

  GUID guid;
  char name[48], sectionName[96], configName[96];
  newGuid(&guid, name, sizeof(name));
  snprintf(sectionName, sizeof(sectionName), "Global\\lg-hcs-probe-%s",
      name);
  snprintf(configName, sizeof(configName),
      "\\BaseNamedObjects\\lg-hcs-probe-%s", name);
  if (!sectionCreate(&section, sectionName, options->size))
    goto done;
  sectionFill(&section, random64());
  jsonString(report, "section_name", configName);

  struct Str request = { 0 };
  regionRequest(&request, "Add", configName, section.size, false);
  hr = hcsModify(system, request.data, &doc);
  free(request.data);
  jsonResult(report, "region_add", hr);
  jsonString(report, "region_add_result", doc);
  printf("  Adding a %" PRIu64 " MiB SharedMemory region to it: 0x%08lx %s\n",
      section.size >> 20, (unsigned long)hr, doc ? doc : "");
  free(doc);
  doc = NULL;
  added = SUCCEEDED(hr);
  if (!added)
    goto done;

  hr = hcsCall(system, api.getComputeSystemProperties,
      "{\"PropertyTypes\":[\"SharedMemoryRegion\"]}", 10000, &doc);
  jsonResult(report, "region_query", hr);
  jsonRaw(report, "region_info", SUCCEEDED(hr) ? doc : NULL);
  printf("  Where the HCS maps it: 0x%08lx %s\n", (unsigned long)hr,
      doc ? doc : "");
  free(doc);
  doc = NULL;

  request = (struct Str){ 0 };
  regionRequest(&request, "Remove", configName, section.size, false);
  hr = hcsModify(system, request.data, &doc);
  free(request.data);
  jsonResult(report, "region_remove", hr);
  jsonString(report, "region_remove_result", doc);
  printf("  Removing the region: 0x%08lx %s\n", (unsigned long)hr,
      doc ? doc : "");
  free(doc);
  removed = SUCCEEDED(hr);

done:
  sectionClose(&section);
  if (system)
    api.closeComputeSystem(system);

  jsonBool(report, "region_added", added);
  jsonBool(report, "region_removed", removed);
  jsonBool(report, "passed", added && removed);
  jsonClose(report, '}');
  return added && removed;
}

// how often the probe starts the Windows guest's VM: Windows restarts
// while it sets itself up, and when the VM stops instead, the probe starts
// it again, as Limiar would
#define WINDOWS_STARTS 4

enum WindowsStart
{
  WINDOWS_PASSED,
  WINDOWS_FAILED,
  WINDOWS_STOPPED // before the guest answered
};

// one start of the Windows guest's VM, which ends when the checks ran or
// the VM stopped before the guest answered
static enum WindowsStart windowsStart(const struct Options * options,
    int start, DWORD timeoutMs, const char * configName,
    struct Section * section, uint64_t nonce, struct Json * report)
{
  struct Vm vm;
  vmInit(&vm, options);
  vm.reopenSerial = true;
  printf("[windows] VM %s boots %s\n", vm.id, options->windowsDisk);

  struct Str devices = { 0 };
  strLiteral(&devices, "\"SharedMemory\":{\"Regions\":[");
  regionSettings(&devices, configName, options->size, false);
  strLiteral(&devices, "]}");

  struct Str config = { 0 };
  vmConfig(&vm, &config, SCHEMA_MINOR, devices.data);
  free(devices.data);

  char logName[40], logPath[MAX_PATH * 3];
  if (start == 1)
    snprintf(logName, sizeof(logName), "windows.serial.log");
  else
    snprintf(logName, sizeof(logName), "windows-%d.serial.log", start);
  outPath(options, logName, logPath, sizeof(logPath));

  // the register page follows the shared memory
  const uint64_t shared = section->size;
  enum WindowsStart result = WINDOWS_FAILED;
  bool     located = false;
  bool     mapped  = false;
  uint64_t reported = 0, gpa = 0;
  char     reply[512], command[160];
  char   * doc = NULL;
  HRESULT  hr;

  if (!vmCreate(&vm, config.data, report, logPath) || !vmStart(&vm, report))
    goto done;

  hr = hcsCall(vm.system, api.getComputeSystemProperties,
      "{\"PropertyTypes\":[\"SharedMemoryRegion\"]}", 10000, &doc);
  jsonResult(report, "region_query", hr);
  jsonRaw(report, "region_info", SUCCEEDED(hr) ? doc : NULL);
  located = SUCCEEDED(hr) &&
    jsonGetUInt64(doc, "GuestPhysicalAddress", &reported);
  free(doc);
  if (!located)
  {
    printf("  The HCS did not report where it mapped the section: 0x%08lx\n",
        (unsigned long)hr);
    goto done;
  }

  // Windows 10.0.26100 reports a page number, and an address would be past
  // the VM's memory
  gpa = reported < ((uint64_t)WINDOWS_MEMORY_MB << 20) ?
    reported * PAGE : reported;
  jsonNumber(report, "region_gpa", gpa);
  printf("  The HCS reports the region at guest physical 0x%" PRIx64 "\n"
      "  Waiting for Windows to set itself up and answer\n", gpa);

  if (!guestReadyWithin(&vm, report, timeoutMs))
  {
    if (vm.stopped && !aborted)
      result = WINDOWS_STOPPED;
    goto done;
  }

  snprintf(command, sizeof(command), "map ivshmem 0x%" PRIx64 " 0x%" PRIx64
      " 0x%" PRIx64, gpa + shared, gpa, shared);
  mapped = vmCommand(&vm, command, "map", 300000, reply, sizeof(reply));
  jsonString(report, "map", reply);
  printf("  %s: %s\n", mapped ?
      "The IVSHMEM driver mapped the region" :
      "The IVSHMEM driver did not map the region", reply);

  if (mapped && guestChecks(&vm, section, nonce, report))
    result = WINDOWS_PASSED;

done:
  jsonNumber(report, "serial_reopened", vm.serialReopened);
  if (vm.system)
    vmStop(&vm, report);
  vmDestroy(&vm, report);
  free(config.data);
  return result;
}

// a disposable Windows guest from --windows-disk, whose lg-hyperv-ivshmem
// answers on the serial port: it gives the IVSHMEM driver a device over a
// SharedMemory region, and the checks read and write through the driver's
// mapping, as the Looking Glass host does
static bool runWindows(const struct Options * options, struct Json * report)
{
  jsonOpen(report, NULL, '{');
  jsonString(report, "case", "windows");
  jsonString(report, "disk", options->windowsDisk);

  GUID guid;
  char name[48], sectionName[96], configName[96];
  newGuid(&guid, name, sizeof(name));
  snprintf(sectionName, sizeof(sectionName), "Global\\lg-hcs-probe-%s",
      name);
  snprintf(configName, sizeof(configName),
      "\\BaseNamedObjects\\lg-hcs-probe-%s", name);

  // the shared memory, then a page for the device's registers that stays
  // zeroed, which reads as an ivshmem-plain without interrupts that is peer
  // 0; the region keeps the size that the shm case checks
  struct Section section;
  if (!sectionCreate(&section, sectionName, options->size))
  {
    jsonBool(report, "passed", false);
    jsonClose(report, '}');
    return false;
  }

  // the pattern and the checks cover the shared memory alone
  section.size = options->size - PAGE;
  const uint64_t nonce = random64();
  sectionFill(&section, nonce);

  // every start shares the boot timeout
  const DWORD begin = GetTickCount();
  enum WindowsStart result = WINDOWS_FAILED;
  jsonOpen(report, "starts", '[');
  for(int start = 1; start <= WINDOWS_STARTS; ++start)
  {
    const DWORD spent = GetTickCount() - begin;
    const DWORD left  = spent < options->bootTimeoutMs ?
      options->bootTimeoutMs - spent : 0;

    jsonOpen(report, NULL, '{');
    result = windowsStart(options, start, left, configName, &section, nonce,
        report);
    jsonClose(report, '}');
    if (result != WINDOWS_STOPPED)
      break;
    if (start < WINDOWS_STARTS)
      printf("  The VM stopped before Windows answered, starting it again\n");
  }
  jsonClose(report, ']');
  sectionClose(&section);

  const bool passed = result == WINDOWS_PASSED;
  jsonBool(report, "passed", passed);
  jsonClose(report, '}');
  return passed;
}

// the newest 2.x configuration schema in the HCS's service properties
static unsigned newestSchema(const char * doc)
{
  unsigned newest = SCHEMA_MINOR;
  for(const char * p = doc; (p = jsonFind(p, "Major"));)
  {
    const unsigned long major = strtoul(p, NULL, 10);
    const char * minor = jsonFind(p, "Minor");
    if (!minor)
      break;

    const unsigned long value = strtoul(minor, NULL, 10);
    if (major == 2 && value > newest && value < 100)
      newest = value;
    p = minor;
  }
  return newest;
}

static BOOL WINAPI onConsoleCtrl(DWORD type)
{
  InterlockedExchange(&aborted, 1);
  return TRUE;
}

static void usage(void)
{
  printf(
    "Usage: lg-windows-client-hcs-probe [options]\n"
    "\n"
    "Checks whether this PC can share memory with a Hyper-V VM the way\n"
    "IVSHMEM does for Looking Glass. It boots disposable Linux VMs through\n"
    "the Host Compute Service and leaves nothing on the host but its output\n"
    "folder. Run it from an elevated prompt.\n"
    "\n"
    "  --kernel PATH   Linux kernel to boot (default: WSL's, in\n"
    "                  %%ProgramFiles%%\\WSL\\tools\\kernel)\n"
    "  --size-mib N    size of the shared memory, a power of two\n"
    "                  (default: 32)\n"
    "  --only CASE     run only the hdv (emulated IVSHMEM device) or the\n"
    "                  shm (SharedMemory region) case\n"
    "  --out DIR       where to write the report and serial logs\n"
    "                  (default: a new folder next to this program)\n"
    "  --timeout S     how long a VM may take to boot (default: 90, or\n"
    "                  1800 with --windows-disk)\n"
    "  --vm ID         check an existing VM instead, such as one of Hyper-V\n"
    "                  Manager, by its ID ((Get-VM NAME).Id): whether the\n"
    "                  HCS opens it and lets this PC add shared memory to\n"
    "                  it, which the probe removes again. The VM keeps\n"
    "                  running.\n"
    "  --windows-disk PATH\n"
    "                  boot a disposable Windows guest from this disk\n"
    "                  instead, which runs lg-hyperv-ivshmem on COM1, and\n"
    "                  check the IVSHMEM driver over a SharedMemory region.\n"
    "                  The guest writes to the disk.\n");
}

static void parseOptions(int argc, char ** argv, struct Options * options)
{
  memset(options, 0, sizeof(*options));
  options->size          = 32 << 20;
  options->bootTimeoutMs = 90000;
  options->hdv           = true;
  options->shm           = true;
  bool only = false, timeout = false;

  for(int i = 1; i < argc; i += 2)
  {
    const char * arg   = argv[i];
    const char * value = argv[i + 1];

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
    {
      usage();
      exit(0);
    }

    if (!value)
    {
      usage();
      exit(2);
    }

    if (strcmp(arg, "--kernel") == 0)
      snprintf(options->kernelSource, sizeof(options->kernelSource), "%s",
          value);
    else if (strcmp(arg, "--out") == 0)
      snprintf(options->out, sizeof(options->out), "%s", value);
    else if (strcmp(arg, "--size-mib") == 0)
    {
      const unsigned long mib = strtoul(value, NULL, 10);
      if (!mib || mib > 1024 || (mib & (mib - 1)))
        fail("--size-mib takes a power of two up to 1024");
      options->size = (uint64_t)mib << 20;
    }
    else if (strcmp(arg, "--only") == 0)
    {
      options->hdv = strcmp(value, "hdv") == 0;
      options->shm = strcmp(value, "shm") == 0;
      if (!options->hdv && !options->shm)
        fail("--only takes hdv or shm");
      only = true;
    }
    else if (strcmp(arg, "--vm") == 0)
    {
      // a GUID, as Get-VM and the HCS give it
      bool guid = strlen(value) == 36;
      for(int c = 0; guid && c < 36; ++c)
        guid = c == 8 || c == 13 || c == 18 || c == 23 ?
          value[c] == '-' : isxdigit((unsigned char)value[c]);
      if (!guid)
        fail("--vm takes the VM's ID, such as (Get-VM NAME).Id");
      snprintf(options->vm, sizeof(options->vm), "%s", value);
    }
    else if (strcmp(arg, "--windows-disk") == 0)
      snprintf(options->windowsDisk, sizeof(options->windowsDisk), "%s",
          value);
    else if (strcmp(arg, "--timeout") == 0)
    {
      const unsigned long seconds = strtoul(value, NULL, 10);
      if (!seconds || seconds > 3600)
        fail("--timeout takes 1 to 3600 seconds");
      options->bootTimeoutMs = seconds * 1000;
      timeout = true;
    }
    else
    {
      usage();
      exit(2);
    }
  }

  if (options->vm[0])
  {
    if (only)
      fail("--vm and --only do not go together");
    options->hdv = options->shm = false;
  }

  if (options->windowsDisk[0])
  {
    if (only || options->vm[0])
      fail("--windows-disk goes with neither --only nor --vm");
    options->hdv = options->shm = false;

    // Windows sets itself up on its first boot
    if (!timeout)
      options->bootTimeoutMs = 1800000;
  }
}

static void windowsVersion(char * text, size_t size)
{
  typedef LONG (WINAPI * RtlGetVersion)(OSVERSIONINFOW *);
  RtlGetVersion getVersion = (RtlGetVersion)(void (*)(void))GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");

  OSVERSIONINFOW info = { .dwOSVersionInfoSize = sizeof(info) };
  if (getVersion && getVersion(&info) == 0)
    snprintf(text, size, "%lu.%lu.%lu", info.dwMajorVersion,
        info.dwMinorVersion, info.dwBuildNumber);
  else
    snprintf(text, size, "unknown");
}

// the output folder, and what the probe's VMs boot when it boots any
static bool prepare(struct Options * options)
{
  if (options->windowsDisk[0])
  {
    char * disk = fullPath(options->windowsDisk);
    if (!disk || !fileExists(disk))
    {
      printf("The disk %s does not exist\n", options->windowsDisk);
      free(disk);
      return false;
    }
    snprintf(options->windowsDisk, sizeof(options->windowsDisk), "%s", disk);
    free(disk);
  }

  const bool bootsVms = options->hdv || options->shm;
  if (bootsVms && !options->kernelSource[0])
  {
    // WSL's kernel, or where WSL from before version 2 kept it
    WCHAR programFiles[MAX_PATH], systemRoot[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH))
      wcscpy(programFiles, L"C:\\Program Files");
    if (!GetEnvironmentVariableW(L"SystemRoot", systemRoot, MAX_PATH))
      wcscpy(systemRoot, L"C:\\Windows");

    char * base = narrow(programFiles);
    snprintf(options->kernelSource, sizeof(options->kernelSource),
        "%s\\WSL\\tools\\kernel", base);
    free(base);

    if (!fileExists(options->kernelSource))
    {
      char * windows = narrow(systemRoot);
      char   legacy[MAX_PATH * 3];
      snprintf(legacy, sizeof(legacy), "%s\\System32\\lxss\\tools\\kernel",
          windows);
      free(windows);
      if (fileExists(legacy))
        snprintf(options->kernelSource, sizeof(options->kernelSource), "%s",
            legacy);
    }
  }

  if (bootsVms)
  {
    char * kernel = fullPath(options->kernelSource);
    if (!kernel || !fileExists(kernel))
    {
      printf("The kernel %s does not exist. Install WSL (wsl --install) or "
          "pass --kernel with a x86_64 Linux kernel that has Hyper-V PCI "
          "support.\n", options->kernelSource);
      free(kernel);
      return false;
    }
    snprintf(options->kernelSource, sizeof(options->kernelSource), "%s",
        kernel);
    free(kernel);
  }

  if (!options->out[0])
  {
    WCHAR exe[MAX_PATH];
    const DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!len || len >= MAX_PATH)
      return false;
    WCHAR * slash = wcsrchr(exe, L'\\');
    if (slash)
      *slash = L'\0';

    SYSTEMTIME now;
    GetLocalTime(&now);
    char * dir = narrow(exe);
    snprintf(options->out, sizeof(options->out),
        "%s\\lg-hcs-probe-%04u%02u%02u-%02u%02u%02u", dir, now.wYear,
        now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    free(dir);
  }

  char * out = fullPath(options->out);
  if (!out || strlen(out) + 1 + OUT_NAME_MAX > sizeof(options->out))
  {
    printf("The output folder's path is too long: %s\n", options->out);
    free(out);
    return false;
  }
  snprintf(options->out, sizeof(options->out), "%s", out);
  free(out);

  WCHAR * wout = widen(options->out);
  const bool  created = CreateDirectoryW(wout, NULL);
  const DWORD error   = GetLastError();
  free(wout);
  if (!created && error != ERROR_ALREADY_EXISTS)
  {
    printf("Cannot create %s: error %lu\n", options->out, error);
    return false;
  }
  if (!bootsVms)
    return true;

  // the VMs boot copies, so that granting them access changes no file of
  // WSL's
  outPath(options, "kernel", options->kernel, sizeof(options->kernel));
  WCHAR * source = widen(options->kernelSource);
  WCHAR * copy   = widen(options->kernel);
  const bool  copied    = CopyFileW(source, copy, FALSE);
  const DWORD copyError = GetLastError();
  free(source);
  free(copy);
  if (!copied)
  {
    printf("Cannot copy the kernel to %s: error %lu\n", options->kernel,
        copyError);
    return false;
  }

  outPath(options, "initrd.cpio", options->initrd, sizeof(options->initrd));
  if (!writeInitrd(options->initrd))
  {
    printf("Cannot write %s\n", options->initrd);
    return false;
  }
  return true;
}

int main(int argc, char ** argv)
{
  // progress shows as it happens, also through a pipe
  setvbuf(stdout, NULL, _IONBF, 0);

  struct Options options;
  parseOptions(argc, argv, &options);

  printf("Looking Glass HCS shared memory probe\n");
  if (!isElevated())
  {
    printf("Run this from an elevated prompt: the Host Compute Service and "
        "global sections need administrator rights.\n");
    return 2;
  }

  if (!loadHcs() || !prepare(&options))
    return 2;

  SetConsoleCtrlHandler(onConsoleCtrl, TRUE);

  char version[32];
  windowsVersion(version, sizeof(version));
  printf("Windows %s, %s %s, %" PRIu64 " MiB of shared memory\n"
      "Writing the report to %s\n", version,
      options.vm[0] ? "VM" : options.windowsDisk[0] ? "disk" : "kernel",
      options.vm[0] ? options.vm : options.windowsDisk[0] ?
        options.windowsDisk : options.kernelSource,
      options.size >> 20, options.out);

  struct Json report = { 0 };
  jsonOpen(&report, NULL, '{');
  jsonString(&report, "probe", "lg-windows-client-hcs-probe");
  jsonNumber(&report, "report_version", 1);
  jsonString(&report, "windows", version);
  jsonString(&report, "kernel", options.vm[0] || options.windowsDisk[0] ?
      NULL : options.kernelSource);
  jsonString(&report, "windows_disk", options.windowsDisk[0] ?
      options.windowsDisk : NULL);
  jsonNumber(&report, "shared_memory_size", options.size);

  // what the HCS runs already, such as WSL or VMs of Hyper-V Manager
  char * systems = NULL;
  const HRESULT hr = hcsEnumerate(&systems);
  jsonResult(&report, "enumerate", hr);
  jsonRaw(&report, "compute_systems", SUCCEEDED(hr) ? systems : NULL);

  // the configuration schemas the HCS supports
  options.schemaMinor = SCHEMA_MINOR;
  if (api.getServiceProperties)
  {
    PWSTR result = NULL;
    const HRESULT shr = api.getServiceProperties(
        L"{\"PropertyTypes\":[\"Basic\"]}", &result);
    char * doc = result ? narrow(result) : NULL;
    if (result)
      LocalFree(result);

    jsonResult(&report, "service_properties_query", shr);
    jsonRaw(&report, "service_properties", SUCCEEDED(shr) ? doc : NULL);
    if (SUCCEEDED(shr))
      options.schemaMinor = newestSchema(doc);
    free(doc);
  }
  jsonNumber(&report, "newest_schema_minor", options.schemaMinor);

  jsonOpen(&report, "cases", '[');
  bool hdvPassed = false, shmPassed = false, vmPassed = false,
    windowsPassed = false;
  if (options.hdv && !aborted)
    hdvPassed = runHdv(&options, &report);
  if (options.shm && !aborted)
    shmPassed = runShm(&options, &report);
  if (options.vm[0] && !aborted)
    vmPassed = runVm(&options, SUCCEEDED(hr) ? systems : NULL, &report);
  if (options.windowsDisk[0] && !aborted)
    windowsPassed = runWindows(&options, &report);
  free(systems);
  jsonClose(&report, ']');
  jsonBool(&report, "aborted", aborted);
  jsonClose(&report, '}');
  strAdd(&report.s, "\n", 1);

  char reportPath[MAX_PATH * 3];
  outPath(&options, "report.json", reportPath, sizeof(reportPath));
  if (!writeFile(reportPath, report.s.data, report.s.len))
    printf("Cannot write %s\n", reportPath);
  free(report.s.data);

  printf("\nResult\n");
  if (options.hdv)
    printf("  hdv, emulated IVSHMEM device: %s\n", hdvPassed ? "works" :
        "does not work");
  if (options.shm)
    printf("  shm, SharedMemory region    : %s\n", shmPassed ? "works" :
        "does not work");
  if (options.vm[0])
    printf("  vm, SharedMemory region added to the VM and removed: %s\n",
        vmPassed ? "yes" : "no");
  if (options.windowsDisk[0])
    printf("  windows, IVSHMEM driver over a SharedMemory region: %s\n",
        windowsPassed ? "works" : "does not work");
  printf("Report: %s\n", reportPath);

  if (aborted)
    return 1;
  return (!options.hdv || hdvPassed) && (!options.shm || shmPassed) &&
    (!options.vm[0] || vmPassed) &&
    (!options.windowsDisk[0] || windowsPassed) ? 0 : 1;
}
