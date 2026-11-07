#pragma once
// Linux constants, struct layouts and helpers shared by the shim files. Values are glibc x86-64.
#include <cstdint>
#include <string>

#include "linux/shim.h"

namespace raven::lnx {

// errno values (asm-generic/errno-base.h, errno.h)
enum : int {
    L_EPERM = 1, L_ENOENT = 2, L_ESRCH = 3, L_EINTR = 4, L_EIO = 5, L_ENXIO = 6, L_E2BIG = 7, L_ENOEXEC = 8,
    L_EBADF = 9, L_ECHILD = 10, L_EAGAIN = 11, L_ENOMEM = 12, L_EACCES = 13, L_EFAULT = 14, L_EBUSY = 16,
    L_EEXIST = 17, L_EXDEV = 18, L_ENODEV = 19, L_ENOTDIR = 20, L_EISDIR = 21, L_EINVAL = 22, L_ENFILE = 23,
    L_EMFILE = 24, L_ENOTTY = 25, L_EFBIG = 27, L_ENOSPC = 28, L_ESPIPE = 29, L_EROFS = 30, L_EPIPE = 32,
    L_EDOM = 33, L_ERANGE = 34, L_EDEADLK = 35, L_ENAMETOOLONG = 36, L_ENOSYS = 38, L_ENOTEMPTY = 39,
    L_ENOTSUP = 95, L_EAFNOSUPPORT = 97, L_EADDRINUSE = 98, L_ENETDOWN = 100, L_ENETUNREACH = 101,
    L_ECONNRESET = 104, L_ENOTCONN = 107, L_ETIMEDOUT = 110, L_ECONNREFUSED = 111, L_EINPROGRESS = 115,
};

int& err();                    // this thread's Linux errno
int from_crt_errno(int e);     // Windows CRT errno -> Linux
int from_win32_error(unsigned long e);
inline int fail(int e) { err() = e; return -1; }

// open() flags
enum : int {
    L_O_RDONLY = 0, L_O_WRONLY = 1, L_O_RDWR = 2, L_O_ACCMODE = 3, L_O_CREAT = 0100, L_O_EXCL = 0200,
    L_O_NOCTTY = 0400, L_O_TRUNC = 01000, L_O_APPEND = 02000, L_O_NONBLOCK = 04000, L_O_DIRECTORY = 0200000,
    L_O_CLOEXEC = 02000000,
};

// struct stat (x86-64)
struct lstat_t {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, pad0;
    uint64_t st_rdev;
    int64_t st_size, st_blksize, st_blocks;
    int64_t st_atime_, st_atime_nsec, st_mtime_, st_mtime_nsec, st_ctime_, st_ctime_nsec;
    int64_t reserved[3];
};
static_assert(sizeof(lstat_t) == 144);

struct ltimespec {
    int64_t tv_sec, tv_nsec;
};
struct ltimeval {
    int64_t tv_sec, tv_usec;
};
struct ltm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    int pad;
    int64_t tm_gmtoff;
    const char* tm_zone;
};
static_assert(sizeof(ltm) == 56);

struct ldirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

// Paths: Linux path as the game sees it -> Windows path. `for_write` sends writes under the game folder to the
// save overlay (the game folder is never modified). See src/linux/vfs.cpp.
std::wstring map_path(const char* linux_path, bool for_write);
std::string game_root();  // the game's halo/ folder (UTF-8)

// The game's allocator (anything the game may free must come from here).
extern "C" SYSV void* lx_malloc(size_t n);
extern "C" SYSV void lx_free(void* p);

// Monotonic and wall clock in nanoseconds.
int64_t mono_ns();
int64_t wall_ns();

}  // namespace raven::lnx
