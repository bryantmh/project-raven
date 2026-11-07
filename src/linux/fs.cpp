// POSIX file descriptors, stat, directories. See src/linux/fd.h and README.md (game folder overlay).
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "app/paths.h"
#include "linux/fd.h"
#include "linux/lnx.h"
#include "log.h"

namespace fs = std::filesystem;
using namespace raven::lnx;
using raven::app::to_utf8;

namespace raven::lnx {
namespace {
std::mutex g_fd_mutex;
std::vector<std::shared_ptr<FdEntry>> g_fds;
std::map<std::string, std::function<std::string()>>& vfiles() {
    static std::map<std::string, std::function<std::string()>> m;
    return m;
}
std::map<std::string, std::function<std::shared_ptr<Device>()>>& devices() {
    static std::map<std::string, std::function<std::shared_ptr<Device>()>> m;
    return m;
}

void init_std_fds() {
    static bool done = false;
    if (done) return;
    done = true;
    g_fds.resize(3);
    for (int i = 0; i < 3; i++) {
        auto e = std::make_shared<FdEntry>();
        e->kind = FdEntry::Console;
        e->h = GetStdHandle(i == 0 ? STD_INPUT_HANDLE : i == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
        e->flags = i == 0 ? L_O_RDONLY : L_O_WRONLY;
        g_fds[i] = e;
    }
}

int64_t to_unix_time(const FILETIME& ft, int64_t* nsec) {
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ull;
    if (nsec) *nsec = int64_t(t % 10000000) * 100;
    return int64_t(t / 10000000);
}

uint64_t hash_path(const std::wstring& p) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : p) h = (h ^ towlower(c)) * 1099511628211ull;
    return h;
}

int fill_stat(const std::wstring& wpath, lstat_t* st) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &a)) return fail(from_win32_error(GetLastError()));
    std::memset(st, 0, sizeof *st);
    bool dir = a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    st->st_mode = dir ? 0040755 : 0100644;
    st->st_nlink = 1;
    st->st_size = dir ? 4096 : int64_t((uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow);
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_ino = hash_path(wpath);
    st->st_dev = 0x801;
    st->st_atime_ = to_unix_time(a.ftLastAccessTime, &st->st_atime_nsec);
    st->st_mtime_ = to_unix_time(a.ftLastWriteTime, &st->st_mtime_nsec);
    st->st_ctime_ = to_unix_time(a.ftCreationTime, &st->st_ctime_nsec);
    return 0;
}

int stat_path(const char* path, lstat_t* st) {
    std::string abs = absolute_linux_path(path);
    if (vfiles().count(abs)) {
        std::memset(st, 0, sizeof *st);
        st->st_mode = 0100444;
        st->st_size = 0;  // like procfs
        st->st_nlink = 1;
        return 0;
    }
    if (devices().count(abs) || abs == "/dev/null" || abs == "/dev/zero" || abs == "/dev/urandom") {
        std::memset(st, 0, sizeof *st);
        st->st_mode = 0020666;
        st->st_nlink = 1;
        return 0;
    }
    return fill_stat(map_path(path, false), st);
}
}  // namespace

int fd_alloc(std::shared_ptr<FdEntry> e, int min_fd) {
    std::lock_guard<std::mutex> lk(g_fd_mutex);
    init_std_fds();
    for (size_t i = std::max<size_t>(3, min_fd); i < g_fds.size(); i++)
        if (!g_fds[i]) { g_fds[i] = std::move(e); return int(i); }
    g_fds.push_back(std::move(e));
    return int(g_fds.size() - 1);
}

std::shared_ptr<FdEntry> fd_get(int fd) {
    std::lock_guard<std::mutex> lk(g_fd_mutex);
    init_std_fds();
    if (fd < 0 || size_t(fd) >= g_fds.size()) return nullptr;
    return g_fds[fd];
}

void fd_close(int fd) {
    std::shared_ptr<FdEntry> e;
    {
        std::lock_guard<std::mutex> lk(g_fd_mutex);
        if (fd < 0 || size_t(fd) >= g_fds.size()) return;
        e = std::move(g_fds[fd]);
    }
    if (e && e.use_count() == 1 && e->kind == FdEntry::File && e->h != INVALID_HANDLE_VALUE) CloseHandle(e->h);
}

void register_virtual_file(const std::string& p, std::function<std::string()> gen) { vfiles()[p] = std::move(gen); }
void register_device(const std::string& p, std::function<std::shared_ptr<Device>()> make) { devices()[p] = std::move(make); }

int64_t fd_read(int fd, void* buf, size_t n) {
    auto e = fd_get(fd);
    if (!e) return fail(L_EBADF);
    switch (e->kind) {
        case FdEntry::Null: return 0;
        case FdEntry::Memory: {
            size_t avail = e->pos < e->data.size() ? e->data.size() - e->pos : 0;
            size_t k = std::min(avail, n);
            std::memcpy(buf, e->data.data() + e->pos, k);
            e->pos += k;
            return int64_t(k);
        }
        case FdEntry::Dev: {
            int64_t r = e->dev->read(buf, n);
            return r < 0 ? fail(int(-r)) : r;
        }
        case FdEntry::Console:
        case FdEntry::File: {
            DWORD got = 0;
            if (!ReadFile(e->h, buf, DWORD(std::min<size_t>(n, 0x7fffffff)), &got, nullptr)) {
                DWORD er = GetLastError();
                if (er == ERROR_BROKEN_PIPE || er == ERROR_HANDLE_EOF) return 0;
                return fail(from_win32_error(er));
            }
            return got;
        }
        default: return fail(L_EBADF);
    }
}

int64_t fd_write(int fd, const void* buf, size_t n) {
    auto e = fd_get(fd);
    if (!e) return fail(L_EBADF);
    switch (e->kind) {
        case FdEntry::Null: return int64_t(n);
        case FdEntry::Memory: return fail(L_EACCES);
        case FdEntry::Dev: {
            int64_t r = e->dev->write(buf, n);
            return r < 0 ? fail(int(-r)) : r;
        }
        case FdEntry::Console: {
            // Game console output goes to the log too.
            std::string s(static_cast<const char*>(buf), n);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (!s.empty()) RLOG("[game] %s", s.c_str());
            return int64_t(n);
        }
        case FdEntry::File: {
            if (e->flags & L_O_APPEND) SetFilePointer(e->h, 0, nullptr, FILE_END);
            DWORD put = 0;
            if (!WriteFile(e->h, buf, DWORD(n), &put, nullptr)) return fail(from_win32_error(GetLastError()));
            return put;
        }
        default: return fail(L_EBADF);
    }
}

}  // namespace raven::lnx

// ---------------------------------------------------------------------------------------------------------------

static std::string proc_maps();

SHIM(int, open, (const char* path, int flags, int mode)) {
    std::string abs = absolute_linux_path(path);
    RTRACE("open %s flags %#x", abs.c_str(), flags);
    auto e = std::make_shared<FdEntry>();
    e->flags = flags;
    e->linux_path = abs;
    if (abs == "/dev/null" || abs == "/dev/zero") {
        e->kind = FdEntry::Null;
        return fd_alloc(e);
    }
    if (abs == "/dev/urandom" || abs == "/dev/random") {
        e->kind = FdEntry::Memory;
        e->data.resize(1 << 16);
        for (auto& c : e->data) c = char(rand());
        return fd_alloc(e);
    }
    if (abs == "/proc/self/maps") {
        e->kind = FdEntry::Memory;
        e->data = proc_maps();
        return fd_alloc(e);
    }
    if (auto it = devices().find(abs); it != devices().end()) {
        e->kind = FdEntry::Dev;
        e->dev = it->second();
        if (!e->dev) return fail(L_ENODEV);
        return fd_alloc(e);
    }
    if (auto it = vfiles().find(abs); it != vfiles().end()) {
        e->kind = FdEntry::Memory;
        e->data = it->second();
        return fd_alloc(e);
    }
    if (abs.rfind("/dev/", 0) == 0 || abs.rfind("/proc/", 0) == 0 || abs.rfind("/sys/", 0) == 0) {
        RTRACE("open: no such virtual node %s", abs.c_str());
        return fail(L_ENOENT);
    }
    int acc = flags & L_O_ACCMODE;
    bool writing = acc != L_O_RDONLY || (flags & (L_O_CREAT | L_O_TRUNC));
    std::wstring w = map_path(path, writing);
    DWORD attrs = GetFileAttributesW(w.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (writing) return fail(L_EISDIR);
        e->is_dir = true;
        e->kind = FdEntry::Null;
        return fd_alloc(e);
    }
    if (flags & L_O_DIRECTORY) return fail(L_ENOTDIR);
    DWORD access = acc == L_O_RDONLY ? GENERIC_READ : acc == L_O_WRONLY ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
    DWORD disp;
    if (flags & L_O_CREAT) disp = (flags & L_O_EXCL) ? CREATE_NEW : (flags & L_O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else disp = (flags & L_O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    HANDLE h = CreateFileW(w.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail(from_win32_error(GetLastError()));
    e->h = h;
    return fd_alloc(e);
}
SHIM_ALIAS(open64, open);
SHIM(int, __open_2, (const char* path, int flags)) { return lx_open(path, flags, 0); }
SHIM(int, openat, (int dirfd, const char* path, int flags, int mode)) { return lx_open(path, flags, mode); }
SHIM(int, creat, (const char* path, int mode)) { return lx_open(path, L_O_CREAT | L_O_WRONLY | L_O_TRUNC, mode); }

SHIM(int, close, (int fd)) {
    if (!fd_get(fd)) return fail(L_EBADF);
    fd_close(fd);
    return 0;
}
SHIM(int64_t, read, (int fd, void* buf, size_t n)) { return fd_read(fd, buf, n); }
SHIM(int64_t, write, (int fd, const void* buf, size_t n)) { return fd_write(fd, buf, n); }
struct liovec { void* base; size_t len; };
SHIM(int64_t, writev, (int fd, const liovec* iov, int cnt)) {
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        int64_t r = fd_write(fd, iov[i].base, iov[i].len);
        if (r < 0) return total ? total : r;
        total += r;
    }
    return total;
}
SHIM(int64_t, lseek, (int fd, int64_t off, int whence)) {
    auto e = fd_get(fd);
    if (!e) return fail(L_EBADF);
    if (e->kind == FdEntry::Memory) {
        int64_t np = whence == 0 ? off : whence == 1 ? int64_t(e->pos) + off : int64_t(e->data.size()) + off;
        if (np < 0) return fail(L_EINVAL);
        e->pos = uint64_t(np);
        return np;
    }
    if (e->kind != FdEntry::File) return e->kind == FdEntry::Null ? 0 : fail(L_ESPIPE);
    LARGE_INTEGER d, out;
    d.QuadPart = off;
    if (!SetFilePointerEx(e->h, d, &out, whence == 0 ? FILE_BEGIN : whence == 1 ? FILE_CURRENT : FILE_END))
        return fail(from_win32_error(GetLastError()));
    return out.QuadPart;
}
SHIM_ALIAS(lseek64, lseek);

SHIM(int, fstat, (int fd, lstat_t* st)) {
    auto e = fd_get(fd);
    if (!e) return fail(L_EBADF);
    std::memset(st, 0, sizeof *st);
    st->st_nlink = 1;
    st->st_blksize = 4096;
    switch (e->kind) {
        case FdEntry::File: {
            BY_HANDLE_FILE_INFORMATION bi;
            if (!GetFileInformationByHandle(e->h, &bi)) return fail(L_EBADF);
            st->st_mode = 0100644;
            st->st_size = int64_t((uint64_t(bi.nFileSizeHigh) << 32) | bi.nFileSizeLow);
            st->st_ino = (uint64_t(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow;
            st->st_dev = bi.dwVolumeSerialNumber;
            st->st_mtime_ = to_unix_time(bi.ftLastWriteTime, &st->st_mtime_nsec);
            st->st_atime_ = to_unix_time(bi.ftLastAccessTime, &st->st_atime_nsec);
            st->st_ctime_ = to_unix_time(bi.ftCreationTime, &st->st_ctime_nsec);
            st->st_blocks = (st->st_size + 511) / 512;
            return 0;
        }
        case FdEntry::Memory: st->st_mode = 0100444; st->st_size = int64_t(e->data.size()); return 0;
        case FdEntry::Null: st->st_mode = e->is_dir ? 0040755 : 0020666; return 0;
        case FdEntry::Socket: st->st_mode = 0140777; return 0;
        default: st->st_mode = 0020666; return 0;
    }
}
SHIM_ALIAS(fstat64, fstat);
SHIM(int, stat, (const char* p, lstat_t* st)) { return stat_path(p, st); }
SHIM_ALIAS(stat64, stat);
SHIM_ALIAS(lstat, stat);
SHIM_ALIAS(lstat64, stat);
SHIM(int, __xstat, (int, const char* p, lstat_t* st)) { return stat_path(p, st); }
SHIM_ALIAS(__xstat64, __xstat);
SHIM_ALIAS(__lxstat, __xstat);
SHIM_ALIAS(__lxstat64, __xstat);
SHIM(int, __fxstat, (int, int fd, lstat_t* st)) { return lx_fstat(fd, st); }
SHIM_ALIAS(__fxstat64, __fxstat);

struct lstatvfs {
    uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
    int32_t spare[6];
};
SHIM(int, statvfs, (const char* path, lstatvfs* s)) {
    std::memset(s, 0, sizeof *s);
    ULARGE_INTEGER avail, total, free_;
    std::wstring w = map_path(path, false);
    if (!GetDiskFreeSpaceExW(fs::path(w).root_path().c_str(), &avail, &total, &free_)) avail.QuadPart = total.QuadPart = free_.QuadPart = 1ull << 36;
    s->f_bsize = s->f_frsize = 4096;
    s->f_blocks = total.QuadPart / 4096;
    s->f_bfree = free_.QuadPart / 4096;
    s->f_bavail = avail.QuadPart / 4096;
    s->f_files = s->f_ffree = s->f_favail = 1 << 20;
    s->f_namemax = 255;
    return 0;
}
SHIM_ALIAS(statvfs64, statvfs);

SHIM(int, access, (const char* path, int mode)) {
    lstat_t st;
    if (stat_path(path, &st) != 0) return -1;
    return 0;
}
SHIM(int, mkdir, (const char* path, int mode)) {
    std::wstring w = map_path(path, true);
    if (CreateDirectoryW(w.c_str(), nullptr)) return 0;
    DWORD er = GetLastError();
    if (er == ERROR_ALREADY_EXISTS) {
        // The directory may exist in the game folder but not yet in the overlay: that counts as existing.
        return fail(L_EEXIST);
    }
    return fail(from_win32_error(er));
}
SHIM(int, rmdir, (const char* path)) {
    std::wstring w = map_path(path, true);
    return RemoveDirectoryW(w.c_str()) ? 0 : fail(from_win32_error(GetLastError()));
}
SHIM(int, unlink, (const char* path)) {
    std::wstring w = map_path(path, true);
    return DeleteFileW(w.c_str()) ? 0 : fail(from_win32_error(GetLastError()));
}
SHIM(int, unlinkat, (int, const char* path, int flags)) { return (flags & 0x200) ? lx_rmdir(path) : lx_unlink(path); }
SHIM(int, remove, (const char* path)) {
    std::wstring w = map_path(path, true);
    if (DeleteFileW(w.c_str()) || RemoveDirectoryW(w.c_str())) return 0;
    return fail(from_win32_error(GetLastError()));
}
SHIM(int, rename, (const char* a, const char* b)) {
    std::wstring wa = map_path(a, true), wb = map_path(b, true);
    return MoveFileExW(wa.c_str(), wb.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? 0
                                                                                                : fail(from_win32_error(GetLastError()));
}
SHIM(int, link, (const char*, const char*)) { return fail(L_EPERM); }
SHIM(int, symlink, (const char*, const char*)) { return fail(L_EPERM); }
SHIM(int64_t, readlink, (const char* path, char* buf, size_t n)) {
    std::string abs = absolute_linux_path(path);
    std::string target;
    if (abs == "/proc/self/exe") target = "/pm/g7/halo/game2";
    else if (abs.rfind("/proc/self/fd/", 0) == 0) {
        auto e = fd_get(std::atoi(abs.c_str() + 14));
        if (!e) return fail(L_ENOENT);
        target = e->linux_path;
    } else return fail(L_EINVAL);
    size_t k = std::min(n, target.size());
    std::memcpy(buf, target.data(), k);
    return int64_t(k);
}
SHIM(char*, realpath, (const char* path, char* out)) {
    lstat_t st;
    if (stat_path(path, &st) != 0) return nullptr;
    std::string abs = absolute_linux_path(path);
    if (!out) out = static_cast<char*>(lx_malloc(4096));
    std::snprintf(out, 4096, "%s", abs.c_str());
    return out;
}
SHIM(char*, __realpath_chk, (const char* path, char* out, size_t)) { return lx_realpath(path, out); }
SHIM(char*, getcwd, (char* buf, size_t n)) {
    std::string c = get_cwd();
    if (!buf) {
        buf = static_cast<char*>(lx_malloc(c.size() + 1));
        n = c.size() + 1;
    }
    if (c.size() + 1 > n) { fail(L_ERANGE); return nullptr; }
    std::memcpy(buf, c.c_str(), c.size() + 1);
    return buf;
}
SHIM(int, chdir, (const char* path)) {
    set_cwd(absolute_linux_path(path));
    return 0;
}
SHIM(int, ftruncate, (int fd, int64_t len)) {
    auto e = fd_get(fd);
    if (!e || e->kind != FdEntry::File) return fail(L_EBADF);
    LARGE_INTEGER cur, z, l;
    z.QuadPart = 0;
    SetFilePointerEx(e->h, z, &cur, FILE_CURRENT);
    l.QuadPart = len;
    SetFilePointerEx(e->h, l, nullptr, FILE_BEGIN);
    BOOL ok = SetEndOfFile(e->h);
    SetFilePointerEx(e->h, cur, nullptr, FILE_BEGIN);
    return ok ? 0 : fail(from_win32_error(GetLastError()));
}
SHIM(int, truncate, (const char* path, int64_t len)) {
    int fd = lx_open(path, L_O_WRONLY, 0);
    if (fd < 0) return -1;
    int r = lx_ftruncate(fd, len);
    lx_close(fd);
    return r;
}
SHIM(int, fsync, (int fd)) {
    auto e = fd_get(fd);
    if (e && e->kind == FdEntry::File) FlushFileBuffers(e->h);
    return 0;
}
SHIM_ALIAS(fdatasync, fsync);
SHIM(void, sync, ()) {}
SHIM(int, flock, (int, int)) { return 0; }
SHIM(int, chmod, (const char*, int)) { return 0; }
SHIM(int, fchmod, (int, int)) { return 0; }
SHIM(int, fchmodat, (int, const char*, int, int)) { return 0; }
SHIM(int, utimensat, (int, const char*, const void*, int)) { return 0; }
SHIM(int, umask, (int m)) { return 022; }
SHIM(int, dup, (int fd)) {
    auto e = fd_get(fd);
    if (!e) return fail(L_EBADF);
    return fd_alloc(e);
}
SHIM(int, dup2, (int a, int b)) {
    auto e = fd_get(a);
    if (!e) return fail(L_EBADF);
    return b;  // the game only redirects its own std streams; keep them where they are
}

// fcntl/ioctl are variadic, but their third argument always travels in the third integer register, so a 3-argument
// SysV function receives it unchanged.
SHIM(int, fcntl, (int fd, int cmd, uint64_t arg)) {
    auto e = fd_get(fd);
    if (!e) return fail(L_EBADF);
    switch (cmd) {
        case 0: return fd_alloc(e, int(arg));            // F_DUPFD
        case 1030: return fd_alloc(e, int(arg));         // F_DUPFD_CLOEXEC
        case 1: return 0;                                // F_GETFD
        case 2: return 0;                                // F_SETFD
        case 3: return e->flags;                         // F_GETFL
        case 4: e->flags = (e->flags & L_O_ACCMODE) | (int(arg) & ~L_O_ACCMODE); return 0;  // F_SETFL
        case 5: case 6: case 7: return 0;                // locks
        default: return 0;
    }
}
SHIM(int, ioctl, (int fd, uint64_t req, void* arg)) {
    auto e = fd_get(fd);
    if (!e) return fail(L_EBADF);
    if (e->kind == FdEntry::Dev) {
        int r = e->dev->ioctl(req, arg);
        return r < 0 ? fail(-r) : r;
    }
    RTRACE("ioctl fd %d (%s) req %#llx", fd, e->linux_path.c_str(), (unsigned long long)req);
    return fail(L_ENOTTY);
}

SHIM(int, pipe, (int fds[2])) {
    HANDLE r, w;
    if (!CreatePipe(&r, &w, nullptr, 1 << 16)) return fail(L_EMFILE);
    auto a = std::make_shared<FdEntry>();
    a->kind = FdEntry::File;
    a->h = r;
    a->flags = L_O_RDONLY;
    auto b = std::make_shared<FdEntry>();
    b->kind = FdEntry::File;
    b->h = w;
    b->flags = L_O_WRONLY;
    fds[0] = fd_alloc(a);
    fds[1] = fd_alloc(b);
    return 0;
}

// ---- directories
struct LDir {
    std::vector<std::pair<std::string, bool>> entries;
    size_t pos = 0;
    ldirent ent;
    int fd = -1;
};
SHIM(LDir*, opendir, (const char* path)) {
    std::string abs = absolute_linux_path(path);
    if (abs == "/dev" || abs.rfind("/dev/", 0) == 0 || abs.rfind("/sys/", 0) == 0 || abs.rfind("/proc/", 0) == 0) {
        // Device trees exist but list nothing: devices are only reached through the emulated ones.
        RTRACE("opendir %s (empty virtual dir)", abs.c_str());
        auto d = new LDir;
        d->entries = {{".", true}, {"..", true}};
        return d;
    }
    std::wstring w = map_path(path, false);
    std::error_code ec;
    if (!fs::is_directory(w, ec)) { fail(L_ENOENT); return nullptr; }
    auto d = new LDir;
    std::set<std::string> seen;
    auto add = [&](const std::wstring& dir) {
        for (auto& it : fs::directory_iterator(dir, ec)) {
            std::string n = to_utf8(it.path().filename().wstring());
            if (seen.insert(n).second) d->entries.push_back({n, it.is_directory(ec)});
        }
    };
    d->entries.push_back({".", true});
    d->entries.push_back({"..", true});
    add(w);
    std::wstring ov = overlay_dir(path);
    if (!ov.empty() && ov != w && fs::is_directory(ov, ec)) add(ov);
    return d;
}
SHIM(LDir*, fdopendir, (int fd)) {
    auto e = fd_get(fd);
    if (!e) { fail(L_EBADF); return nullptr; }
    LDir* d = lx_opendir(e->linux_path.c_str());
    if (d) d->fd = fd;
    return d;
}
SHIM(ldirent*, readdir, (LDir* d)) {
    if (d->pos >= d->entries.size()) return nullptr;
    auto& [name, dir] = d->entries[d->pos++];
    std::memset(&d->ent, 0, sizeof d->ent);
    d->ent.d_ino = d->pos;
    d->ent.d_off = int64_t(d->pos);
    d->ent.d_reclen = sizeof(ldirent);
    d->ent.d_type = dir ? 4 : 8;  // DT_DIR / DT_REG
    std::snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", name.c_str());
    return &d->ent;
}
SHIM_ALIAS(readdir64, readdir);
SHIM(int, closedir, (LDir* d)) {
    if (!d) return fail(L_EBADF);
    if (d->fd >= 0) fd_close(d->fd);
    delete d;
    return 0;
}
SHIM(int, dirfd, (LDir* d)) {
    if (d->fd < 0) {
        auto e = std::make_shared<FdEntry>();
        e->kind = FdEntry::Null;
        e->is_dir = true;
        d->fd = fd_alloc(e);
    }
    return d->fd;
}

// /proc/self/maps for halo.so's hook allocator and anything else that walks it.
#include "elf/elf_loader.h"
static std::string proc_maps() {
    std::string out;
    char line[256];
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t* p = nullptr;
    while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_COMMIT) {
            DWORD pr = mbi.Protect & 0xff;
            bool r = pr != PAGE_NOACCESS, w = pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_WRITECOPY,
                 x = pr & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE);
            uint64_t a = uint64_t(mbi.BaseAddress), b = a + mbi.RegionSize;
            auto im = raven::elf::find_image(a);
            std::snprintf(line, sizeof line, "%012llx-%012llx %c%c%cp 00000000 00:00 0 %s\n", (unsigned long long)a,
                          (unsigned long long)b, r ? 'r' : '-', w ? 'w' : '-', x ? 'x' : '-',
                          im ? (im->executable ? "/pm/g7/halo/game2" : im->path.c_str()) : "");
            out += line;
        }
        p = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (uint64_t(p) >= 0x7fffffff0000ull) break;
    }
    return out;
}
