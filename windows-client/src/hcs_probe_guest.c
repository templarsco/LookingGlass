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
 * The init of the HCS probe's Linux VM. It maps the shared memory the host
 * offers, either a region at a guest physical address or a BAR of the
 * IVSHMEM PCI device, and answers the host's commands on the first serial
 * port, one line each way. Every number is hexadecimal.
 *
 * It uses no C library, so any compiler that targets x86_64 Linux builds it.
 * With --stdio it runs as a normal process on stdin and stdout, for tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t  u8;
typedef size_t   usize;

#define PAGE_SIZE 4096UL
#define GOLDEN    0x9E3779B97F4A7C15UL

enum
{
  SYS_read       = 0,
  SYS_write      = 1,
  SYS_open       = 2,
  SYS_close      = 3,
  SYS_poll       = 7,
  SYS_mmap       = 9,
  SYS_munmap     = 11,
  SYS_ioctl      = 16,
  SYS_pread64    = 17,
  SYS_pwrite64   = 18,
  SYS_nanosleep  = 35,
  SYS_exit       = 60,
  SYS_uname      = 63,
  SYS_mkdir      = 83,
  SYS_syslog     = 103,
  SYS_sync       = 162,
  SYS_mount      = 165,
  SYS_reboot     = 169,
  SYS_getdents64 = 217
};

#define O_RDONLY    00
#define O_WRONLY    01
#define O_RDWR      02
#define O_NOCTTY    0400
#define O_DIRECTORY 0200000
#define O_SYNC      04010000

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED 1

#define POLLIN 1

#define TCGETS 0x5401
#define TCSETS 0x5402
#define ISIG   0000001
#define ECHO   0000010
#define ECHOE  0000020
#define ECHOK  0000040
#define ECHONL 0000100

#define SYSLOG_ACTION_CONSOLE_LEVEL 8

#define REBOOT_MAGIC1    0xfee1dead
#define REBOOT_MAGIC2    672274793
#define REBOOT_POWER_OFF 0x4321fedc

struct KernelTermios
{
  u32 iflag, oflag, cflag, lflag;
  u8  line;
  u8  cc[19];
};

struct Timespec
{
  long sec, nsec;
};

struct PollFd
{
  int   fd;
  short events, revents;
};

static long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
  long ret;
  register long r10 __asm__("r10") = d;
  register long r8  __asm__("r8")  = e;
  register long r9  __asm__("r9")  = f;
  __asm__ volatile ("syscall"
    : "=a"(ret)
    : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
    : "rcx", "r11", "memory");
  return ret;
}

#define SYSCALL(n, a, b, c) \
  syscall6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

// the compiler may emit calls to these for plain loops and copies
void * memset(void * dst, int value, usize size)
{
  u8 * d = dst;
  while (size--)
    *d++ = (u8)value;
  return dst;
}

void * memcpy(void * dst, const void * src, usize size)
{
  u8 * d = dst;
  const u8 * s = src;
  while (size--)
    *d++ = *s++;
  return dst;
}

static int  in  = -1;
static int  out = -1;
static bool isInit;

static volatile u64 * map;
static u64            mapSize;

static usize length(const char * s)
{
  usize n = 0;
  while (s[n])
    ++n;
  return n;
}

static bool equal(const char * a, const char * b)
{
  while (*a && *a == *b)
    ++a, ++b;
  return *a == *b;
}

static void writeAll(int fd, const char * data, usize size)
{
  while (size)
  {
    long n = SYSCALL(SYS_write, fd, data, size);
    if (n <= 0)
      return;
    data += n;
    size -= n;
  }
}

// replies are built whole and written at once, so kernel messages that reach
// the same console cannot split them
static char  reply[1024];
static usize replyLen;

static void replyStr(const char * s)
{
  while (*s && replyLen < sizeof(reply) - 1)
    reply[replyLen++] = *s++;
}

static void replyBegin(const char * what)
{
  replyLen = 0;
  replyStr("LGSHM ");
  replyStr(what);
}

static void replyHex(u64 value)
{
  char text[19];
  int  i = sizeof(text);
  text[--i] = '\0';
  do
  {
    text[--i] = "0123456789abcdef"[value & 0xf];
    value >>= 4;
  }
  while (value);
  text[--i] = 'x';
  text[--i] = '0';
  replyStr(text + i);
}

static void replyField(const char * name, u64 value)
{
  replyStr(" ");
  replyStr(name);
  replyStr("=");
  replyHex(value);
}

static void replyEnd(void)
{
  reply[replyLen++] = '\n';
  writeAll(out, reply, replyLen);
}

static void say(const char * what)
{
  replyBegin(what);
  replyEnd();
}

static void sayError(const char * what, const char * step, long error)
{
  replyBegin(what);
  replyStr(" failed step=");
  replyStr(step);
  replyField("errno", (u64)-error);
  replyEnd();
}

static void sleepMs(long ms)
{
  struct Timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
  SYSCALL(SYS_nanosleep, &ts, NULL, 0);
}

static void powerOff(void)
{
  if (!isInit)
    SYSCALL(SYS_exit, 0, 0, 0);

  SYSCALL(SYS_sync, 0, 0, 0);
  syscall6(SYS_reboot, REBOOT_MAGIC1, REBOOT_MAGIC2, REBOOT_POWER_OFF, 0, 0,
      0);

  // init must never exit, the kernel panics if it does
  say("off failed");
  for(;;)
    sleepMs(1000);
}

static bool parseHex(const char ** text, u64 * value)
{
  const char * p = *text;
  while (*p == ' ')
    ++p;
  if (p[0] == '0' && p[1] == 'x')
    p += 2;

  u64 v = 0;
  int digits = 0;
  for(;; ++p, ++digits)
  {
    u64 d;
    if (*p >= '0' && *p <= '9')
      d = *p - '0';
    else if (*p >= 'a' && *p <= 'f')
      d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'F')
      d = *p - 'A' + 10;
    else
      break;

    if (digits == 16)
      return false;
    v = (v << 4) | d;
  }

  if (!digits || (*p && *p != ' ' && *p != '\n'))
    return false;

  *text  = p;
  *value = v;
  return true;
}

// reads a whole small file, terminated; returns the length or -errno
static long readFile(const char * path, char * buf, usize size)
{
  long fd = SYSCALL(SYS_open, path, O_RDONLY, 0);
  if (fd < 0)
    return fd;

  usize len = 0;
  while (len < size - 1)
  {
    long n = SYSCALL(SYS_read, fd, buf + len, size - 1 - len);
    if (n < 0)
    {
      SYSCALL(SYS_close, fd, 0, 0);
      return n;
    }
    if (n == 0)
      break;
    len += n;
  }

  SYSCALL(SYS_close, fd, 0, 0);
  buf[len] = '\0';
  return len;
}

static void join(char * dst, usize size, const char * a, const char * b,
    const char * c)
{
  const char * parts[] = { a, b, c };
  usize len = 0;
  for(int i = 0; i < 3; ++i)
    for(const char * p = parts[i]; p && *p && len < size - 1; ++p)
      dst[len++] = *p;
  dst[len] = '\0';
}

static char fileBuf[32768];

// prints every line of a file as "LGSHM <what> <line>"
static void dumpFile(const char * what, const char * path)
{
  long len = readFile(path, fileBuf, sizeof(fileBuf));
  if (len < 0)
  {
    sayError(what, "read", len);
    return;
  }

  char * line = fileBuf;
  for(char * p = fileBuf; p < fileBuf + len; ++p)
  {
    if (*p != '\n')
      continue;
    *p = '\0';
    replyBegin(what);
    replyStr(" ");
    replyStr(line);
    replyEnd();
    line = p + 1;
  }
}

#define PCI_DEVICES "/sys/bus/pci/devices/"

static bool pciValue(const char * device, const char * name, u64 * value)
{
  char path[256];
  char text[64];
  join(path, sizeof(path), PCI_DEVICES, device, name);
  if (readFile(path, text, sizeof(text)) <= 0)
    return false;

  const char * p = text;
  return parseHex(&p, value);
}

struct Dirent64
{
  u64            ino;
  long           off;
  unsigned short reclen;
  u8             type;
  char           name[];
};

typedef bool (*Visitor)(const char * name, void * opaque);

// calls visit for every entry of a directory until it returns true
static bool dirEach(const char * dir, Visitor visit, void * opaque)
{
  long fd = SYSCALL(SYS_open, dir, O_RDONLY | O_DIRECTORY, 0);
  if (fd < 0)
    return false;

  static char entries[8192];
  bool found = false;
  long n;
  while (!found &&
      (n = SYSCALL(SYS_getdents64, fd, entries, sizeof(entries))) > 0)
    for(long pos = 0; pos < n && !found;)
    {
      struct Dirent64 * entry = (struct Dirent64 *)(entries + pos);
      pos += entry->reclen;
      if (entry->name[0] != '.')
        found = visit(entry->name, opaque);
    }

  SYSCALL(SYS_close, fd, 0, 0);
  return found;
}

static bool pciPrint(const char * device, void * opaque)
{
  (void)opaque;
  u64 vendor = 0, id = 0, class = 0;
  pciValue(device, "/vendor", &vendor);
  pciValue(device, "/device", &id);
  pciValue(device, "/class" , &class);

  replyBegin("pci ");
  replyStr(device);
  replyField("vendor", vendor);
  replyField("device", id);
  replyField("class" , class);
  replyEnd();
  return false;
}

#define VMBUS_DEVICES "/sys/bus/vmbus/devices/"

// Hyper-V offers virtual PCI buses as VMBus channels of class
// 44c4f61d-4444-4400-9d52-802e27ede19f, which the kernel's hv_pci serves
static bool vmbusPrint(const char * device, void * opaque)
{
  (void)opaque;
  char path[256];
  char text[64];
  join(path, sizeof(path), VMBUS_DEVICES, device, "/class_id");
  if (readFile(path, text, sizeof(text)) <= 0)
    return false;

  for(char * p = text; *p; ++p)
    if (*p == '\n')
      *p = '\0';

  replyBegin("vmbus ");
  replyStr(device);
  replyStr(" class=");
  replyStr(text);
  replyEnd();
  return false;
}

struct PciMatch
{
  u64  vendor, device;
  char name[64];
};

static bool pciFind(const char * device, void * opaque)
{
  struct PciMatch * match = opaque;
  u64 vendor, id;
  if (!pciValue(device, "/vendor", &vendor) ||
      !pciValue(device, "/device", &id) ||
      vendor != match->vendor || id != match->device)
    return false;

  join(match->name, sizeof(match->name), device, NULL, NULL);
  return true;
}

static void cmdInfo(void)
{
  struct
  {
    char sysname[65], nodename[65], release[65], version[65], machine[65],
         domainname[65];
  }
  uts;

  if (SYSCALL(SYS_uname, &uts, 0, 0) == 0)
  {
    replyBegin("kernel ");
    replyStr(uts.release);
    replyEnd();
  }

  dumpFile("iomem", "/proc/iomem");
  if (isInit)
    dumpFile("cmdline", "/proc/cmdline");
  dirEach(PCI_DEVICES, pciPrint, NULL);
  dirEach(VMBUS_DEVICES, vmbusPrint, NULL);

  long dir = SYSCALL(SYS_open, "/sys/bus/vmbus/drivers/hv_pci",
      O_RDONLY | O_DIRECTORY, 0);
  say(dir >= 0 ? "hv_pci present" : "hv_pci absent");
  if (dir >= 0)
    SYSCALL(SYS_close, dir, 0, 0);

  long fd = SYSCALL(SYS_open, "/dev/mem", O_RDONLY, 0);
  say(fd >= 0 ? "devmem present" : "devmem absent");
  if (fd >= 0)
    SYSCALL(SYS_close, fd, 0, 0);

  say("info end");
}

static volatile u64 * mapAt(const char * path, u64 offset, u64 size,
    int flags, const char ** step, long * error)
{
  *step  = "open";
  *error = SYSCALL(SYS_open, path, O_RDWR | flags, 0);
  if (*error < 0)
    return NULL;

  const long fd = *error;
  long addr = syscall6(SYS_mmap, 0, size, PROT_READ | PROT_WRITE, MAP_SHARED,
      fd, offset);
  SYSCALL(SYS_close, fd, 0, 0);

  *step  = "mmap";
  *error = addr;
  return addr < 0 && addr > -4096 ? NULL : (volatile u64 *)addr;
}

static bool mapFile(const char * path, u64 offset, u64 size, int flags,
    const char ** step, long * error)
{
  volatile u64 * addr = mapAt(path, offset, size, flags, step, error);
  if (!addr)
    return false;

  map     = addr;
  mapSize = size;
  return true;
}

// physical memory through /dev/mem: cached first, as a region the kernel
// tracks as uncached only maps as such
static volatile u64 * mapPhys(u64 addr, u64 size, const char ** step,
    long * error)
{
  volatile u64 * p = mapAt("/dev/mem", addr, size, 0, step, error);
  return p ? p : mapAt("/dev/mem", addr, size, O_SYNC, step, error);
}

// map phys ADDR SIZE: a region at a guest physical address, through /dev/mem
static void cmdMapPhys(const char * args)
{
  u64 addr, size;
  if (!parseHex(&args, &addr) || !parseHex(&args, &size) || !size ||
      (addr | size) & (PAGE_SIZE - 1))
  {
    say("map failed step=args");
    return;
  }

  const char * step;
  long error;
  volatile u64 * p = mapPhys(addr, size, &step, &error);
  if (!p)
  {
    sayError("map", step, error);
    return;
  }

  map     = p;
  mapSize = size;
  replyBegin("map ok");
  replyField("addr", addr);
  replyField("size", size);
  replyEnd();
}

// Hyper-V offers virtual PCI devices over VMBus, after init has started
static bool pciWait(struct PciMatch * match)
{
  for(int i = 0; i < 300; ++i)
  {
    if (dirEach(PCI_DEVICES, pciFind, match))
      return true;
    sleepMs(100);
  }
  return false;
}

// where the guest placed a BAR, from the device's resource file
static bool pciBar(const char * device, u64 bar, u64 * start, u64 * end,
    u64 * flags)
{
  char path[256];
  join(path, sizeof(path), PCI_DEVICES, device, "/resource");
  if (readFile(path, fileBuf, sizeof(fileBuf)) < 0)
    return false;

  // one "start end flags" line per resource, BARs first
  const char * p = fileBuf;
  for(u64 i = 0; i < bar && p; ++i)
    for(; *p && *p++ != '\n';);

  return parseHex(&p, start) && parseHex(&p, end) && parseHex(&p, flags) &&
    *start && *end > *start;
}

// VENDOR DEVICE, then the device must be there
static bool pciArgs(const char ** args, struct PciMatch * match,
    const char * what)
{
  if (!parseHex(args, &match->vendor) || !parseHex(args, &match->device))
  {
    replyBegin(what);
    replyStr(" failed step=args");
    replyEnd();
    return false;
  }

  if (!pciWait(match))
  {
    replyBegin(what);
    replyStr(" failed step=find");
    replyEnd();
    return false;
  }
  return true;
}

// map pci VENDOR DEVICE BAR: a BAR of the first matching PCI device
static void cmdMapPci(const char * args)
{
  struct PciMatch match;
  u64 bar, start, end, flags;
  if (!pciArgs(&args, &match, "map"))
    return;

  if (!parseHex(&args, &bar) || bar > 5)
  {
    say("map failed step=args");
    return;
  }

  if (!pciBar(match.name, bar, &start, &end, &flags))
  {
    say("map failed step=bar");
    return;
  }

  // enables memory decoding, which no driver does for this device
  char path[256];
  join(path, sizeof(path), PCI_DEVICES, match.name, "/enable");
  long fd = SYSCALL(SYS_open, path, O_WRONLY, 0);
  if (fd >= 0)
  {
    SYSCALL(SYS_write, fd, "1", 1);
    SYSCALL(SYS_close, fd, 0, 0);
  }

  char name[16] = "/resource0";
  name[9] = '0' + bar;
  join(path, sizeof(path), PCI_DEVICES, match.name, name);

  const char * step;
  long error;
  if (!mapFile(path, 0, end - start + 1, 0, &step, &error))
  {
    sayError("map", step, error);
    return;
  }

  u64 command = 0;
  pciValue(match.name, "/enable", &command);
  replyBegin("map ok device=");
  replyStr(match.name);
  replyField("addr" , start);
  replyField("size" , mapSize);
  replyField("flags", flags);
  replyField("enabled", command);
  replyEnd();
}

// map file PATH SIZE: a file standing in for the shared memory, for tests
static void cmdMapFile(const char * args)
{
  char path[256];
  usize len = 0;
  while (*args == ' ')
    ++args;
  while (*args && *args != ' ' && len < sizeof(path) - 1)
    path[len++] = *args++;
  path[len] = '\0';

  u64 size;
  if (!len || !parseHex(&args, &size) || !size || size & (PAGE_SIZE - 1))
  {
    say("map failed step=args");
    return;
  }

  const char * step;
  long error;
  if (!mapFile(path, 0, size, 0, &step, &error))
  {
    sayError("map", step, error);
    return;
  }

  replyBegin("map ok");
  replyField("size", size);
  replyEnd();
}

// the two words at a guest physical address, through a mapping of its page
// that only lasts for the read
static void peek(u64 addr)
{
  const char * step;
  long error;
  volatile u64 * page = mapPhys(addr & ~(PAGE_SIZE - 1), PAGE_SIZE, &step,
      &error);
  if (!page)
  {
    sayError("peek", step, error);
    return;
  }

  const volatile u64 * word = page + (addr & (PAGE_SIZE - 1)) / sizeof(u64);
  const u64 w0 = word[0];
  const u64 w1 = word[1];
  SYSCALL(SYS_munmap, page, PAGE_SIZE, 0);

  replyBegin("peek ok");
  replyField("addr", addr);
  replyField("w0"  , w0  );
  replyField("w1"  , w1  );
  replyEnd();
}

static bool peekable(u64 addr)
{
  return !(addr & 7) && (addr & (PAGE_SIZE - 1)) <= PAGE_SIZE - 16;
}

// peek phys ADDR: the two words at a guest physical address
static void cmdPeekPhys(const char * args)
{
  u64 addr;
  if (!parseHex(&args, &addr) || !peekable(addr))
  {
    say("peek failed step=args");
    return;
  }
  peek(addr);
}

// peek pci VENDOR DEVICE BAR OFFSET: the two words at an offset into a BAR,
// read where the guest placed the BAR
static void cmdPeekPci(const char * args)
{
  struct PciMatch match;
  u64 bar, offset, start, end, flags;
  if (!pciArgs(&args, &match, "peek"))
    return;

  if (!parseHex(&args, &bar) || bar > 5 || !parseHex(&args, &offset))
  {
    say("peek failed step=args");
    return;
  }

  if (!pciBar(match.name, bar, &start, &end, &flags) ||
      offset > end - start || end - start - offset < 15 ||
      !peekable(start + offset))
  {
    say("peek failed step=bar");
    return;
  }
  peek(start + offset);
}

// config VENDOR DEVICE OFFSET SIZE [VALUE]: a register of the device's
// configuration space, read back after writing VALUE to it if given
static void cmdConfig(const char * args)
{
  struct PciMatch match;
  u64 offset, size, value = 0;
  if (!pciArgs(&args, &match, "config"))
    return;

  if (!parseHex(&args, &offset) || !parseHex(&args, &size) ||
      (size != 1 && size != 2 && size != 4) || offset & (size - 1) ||
      offset >= 4096)
  {
    say("config failed step=args");
    return;
  }

  while (*args == ' ')
    ++args;
  const bool write = *args && *args != '\n';
  if (write && !parseHex(&args, &value))
  {
    say("config failed step=args");
    return;
  }

  char path[256];
  join(path, sizeof(path), PCI_DEVICES, match.name, "/config");
  long fd = SYSCALL(SYS_open, path, write ? O_RDWR : O_RDONLY, 0);
  if (fd < 0)
  {
    sayError("config", "open", fd);
    return;
  }

  u8 bytes[4] = { 0 };
  for(u64 i = 0; i < size; ++i)
    bytes[i] = (u8)(value >> (i * 8));

  long n = size;
  const char * step = "write";
  if (write)
    n = syscall6(SYS_pwrite64, fd, (long)bytes, size, offset, 0, 0);
  if (n == (long)size)
  {
    step = "read";
    n = syscall6(SYS_pread64, fd, (long)bytes, size, offset, 0, 0);
  }
  SYSCALL(SYS_close, fd, 0, 0);

  // errno 0 is a short transfer
  if (n != (long)size)
  {
    sayError("config", step, n < 0 ? n : 0);
    return;
  }

  value = 0;
  for(u64 i = 0; i < size; ++i)
    value |= (u64)bytes[i] << (i * 8);

  replyBegin("config ok device=");
  replyStr(match.name);
  replyField("offset", offset);
  replyField("value" , value );
  replyEnd();
}

static volatile u64 * pageWord(u64 page, u64 word)
{
  return map + page * (PAGE_SIZE / sizeof(u64)) + word;
}

// verify MAGIC NONCE PAGES: the host's pattern at the start of every page
static void cmdVerify(const char * args)
{
  u64 magic, nonce, pages;
  if (!map || !parseHex(&args, &magic) || !parseHex(&args, &nonce) ||
      !parseHex(&args, &pages) || !pages || pages > mapSize / PAGE_SIZE)
  {
    say("verify failed step=args");
    return;
  }

  u64 bad = 0, first = 0, got0 = 0, got1 = 0;
  for(u64 i = 0; i < pages; ++i)
  {
    const u64 v0 = *pageWord(i, 0);
    const u64 v1 = *pageWord(i, 1);
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
  {
    replyBegin("verify ok");
    replyField("pages", pages);
    replyEnd();
    return;
  }

  replyBegin("verify bad");
  replyField("count", bad  );
  replyField("first", first);
  replyField("got0" , got0 );
  replyField("got1" , got1 );
  replyEnd();
}

// write PAGE VALUE and read PAGE: the word after the pattern in a page
static bool pageArg(const char ** args, u64 * page)
{
  return map && parseHex(args, page) && *page < mapSize / PAGE_SIZE;
}

static void cmdWrite(const char * args)
{
  u64 page, value;
  if (!pageArg(&args, &page) || !parseHex(&args, &value))
  {
    say("write failed step=args");
    return;
  }

  *pageWord(page, 2) = value;
  __asm__ volatile ("mfence" ::: "memory");

  replyBegin("write ok");
  replyField("page", page);
  replyEnd();
}

static void cmdRead(const char * args)
{
  u64 page;
  if (!pageArg(&args, &page))
  {
    say("read failed step=args");
    return;
  }

  __asm__ volatile ("mfence" ::: "memory");
  replyBegin("read ok");
  replyField("page" , page);
  replyField("value", *pageWord(page, 2));
  replyEnd();
}

static bool command(const char * line, const char * name, const char ** args)
{
  usize len = length(name);
  for(usize i = 0; i < len; ++i)
    if (line[i] != name[i])
      return false;
  if (line[len] != ' ' && line[len] != '\0')
    return false;

  *args = line + len;
  return true;
}

static void handle(const char * line)
{
  const char * args;
  if (command(line, "info", &args))
    cmdInfo();
  else if (command(line, "map phys", &args))
    cmdMapPhys(args);
  else if (command(line, "map pci", &args))
    cmdMapPci(args);
  else if (command(line, "map file", &args))
    cmdMapFile(args);
  else if (command(line, "peek phys", &args))
    cmdPeekPhys(args);
  else if (command(line, "peek pci", &args))
    cmdPeekPci(args);
  else if (command(line, "config", &args))
    cmdConfig(args);
  else if (command(line, "verify", &args))
    cmdVerify(args);
  else if (command(line, "write", &args))
    cmdWrite(args);
  else if (command(line, "read", &args))
    cmdRead(args);
  else if (command(line, "off", &args))
  {
    say("off");
    powerOff();
  }
  else if (line[0])
    say("error unknown command");
}

static void setupInit(void)
{
  SYSCALL(SYS_mkdir, "/proc", 0555, 0);
  SYSCALL(SYS_mkdir, "/sys" , 0555, 0);
  SYSCALL(SYS_mkdir, "/dev" , 0755, 0);
  syscall6(SYS_mount, (long)"proc"    , (long)"/proc", (long)"proc"    , 0, 0,
      0);
  syscall6(SYS_mount, (long)"sysfs"   , (long)"/sys" , (long)"sysfs"   , 0, 0,
      0);
  syscall6(SYS_mount, (long)"devtmpfs", (long)"/dev" , (long)"devtmpfs", 0, 0,
      0);

  long fd = SYSCALL(SYS_open, "/dev/ttyS0", O_RDWR | O_NOCTTY, 0);
  if (fd < 0)
  {
    // nothing can reach the host, so stop the VM for it to notice
    powerOff();
  }
  in = out = fd;

  // commands must not echo back into the replies
  struct KernelTermios tio;
  if (SYSCALL(SYS_ioctl, fd, TCGETS, &tio) == 0)
  {
    tio.lflag &= ~(ISIG | ECHO | ECHOE | ECHOK | ECHONL);
    SYSCALL(SYS_ioctl, fd, TCSETS, &tio);
  }
}

int guestMain(long * stack) __attribute__((used));
int guestMain(long * stack)
{
  const long   argc = stack[0];
  char ** const argv = (char **)(stack + 1);

  if (argc > 1 && equal(argv[1], "--stdio"))
  {
    in  = 0;
    out = 1;
  }
  else
  {
    isInit = true;
    setupInit();
  }

  static char buf[4096];
  usize len       = 0;
  u64   greetings = 0;
  bool  heard     = false;
  for(;;)
  {
    // the host may open the serial port after the guest has written to it,
    // which loses the output, so the greeting repeats until a command comes
    if (!heard)
    {
      replyBegin("ready");
      replyField("version", 1);
      replyField("greeting", ++greetings);
      replyEnd();

      // boot messages were logged already, keep later ones off the protocol
      if (isInit && greetings == 1)
        SYSCALL(SYS_syslog, SYSLOG_ACTION_CONSOLE_LEVEL, NULL, 1);

      struct PollFd pfd = { .fd = (int)in, .events = POLLIN };
      const long ready = SYSCALL(SYS_poll, &pfd, 1, 500);
      if (ready < 0)
        sleepMs(500);
      if (ready <= 0)
        continue;
    }

    long n = SYSCALL(SYS_read, in, buf + len, sizeof(buf) - 1 - len);
    if (n <= 0)
    {
      if (!isInit)
        return 0;
      sleepMs(100);
      continue;
    }
    heard = true;
    len  += n;

    usize start = 0;
    for(usize i = 0; i < len; ++i)
    {
      if (buf[i] != '\n' && buf[i] != '\r')
        continue;
      buf[i] = '\0';
      handle(buf + start);
      start = i + 1;
    }

    // keeps the incomplete line; a line that fills the buffer is dropped
    len -= start;
    memcpy(buf, buf + start, len);
    if (len == sizeof(buf) - 1)
      len = 0;
  }
}

__asm__(
  ".text\n"
  ".global _start\n"
  ".type _start, @function\n"
  "_start:\n"
  "  xor %ebp, %ebp\n"
  "  mov %rsp, %rdi\n"
  "  and $-16, %rsp\n"
  "  call guestMain\n"
  "  mov %eax, %edi\n"
  "  mov $60, %eax\n"
  "  syscall\n"
  "  hlt\n"
);
