input-event-codes.h is the Linux UAPI header that defines the evdev KEY_*,
BTN_* and related codes. It is copied unchanged from linux-libc-dev
6.8.0-106.106 (Ubuntu 24.04), which ships include/uapi/linux/input-event-codes.h
from Linux 6.8. Its license is GPL-2.0-only WITH Linux-syscall-note, as stated
in the file.

The client uses these codes as its internal key space on every platform. On
Linux the system header is used; this copy is only on the include path of
the Windows build.
