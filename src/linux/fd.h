#pragma once
// The Linux file descriptor table. Each fd is a Windows file handle, an in-memory virtual file (/proc, /sys), a
// device the loader emulates, or a socket.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace raven::lnx {

struct Device {
    virtual ~Device() = default;
    virtual int64_t read(void* buf, size_t n) = 0;    // -errno on failure
    virtual int64_t write(const void* buf, size_t n) = 0;
    virtual int ioctl(uint64_t req, void* arg) { return -25; /* ENOTTY */ }
    virtual bool readable() { return true; }          // for poll/select
};

struct FdEntry {
    enum Kind { File, Memory, Null, Console, Dev, Socket } kind = File;
    HANDLE h = INVALID_HANDLE_VALUE;
    std::string data;   // Memory
    uint64_t pos = 0;   // Memory
    int flags = 0;      // Linux open flags
    std::string linux_path;
    std::shared_ptr<Device> dev;
    uint64_t sock = ~0ull;
    bool is_dir = false;
};

int fd_alloc(std::shared_ptr<FdEntry> e, int min_fd = 0);
std::shared_ptr<FdEntry> fd_get(int fd);
void fd_close(int fd);

// Register a virtual file whose contents are generated on open (/proc/cpuinfo, /sys/class/dmi/...).
void register_virtual_file(const std::string& linux_path, std::function<std::string()> gen);
// Register a device node (/dev/hidraw0 and so on) opened through `make`.
void register_device(const std::string& linux_path, std::function<std::shared_ptr<Device>()> make);

int64_t fd_read(int fd, void* buf, size_t n);
int64_t fd_write(int fd, const void* buf, size_t n);

std::string absolute_linux_path(const char* path);
void set_cwd(const std::string& linux_abs);
std::string get_cwd();
std::wstring overlay_dir(const char* linux_path);

}  // namespace raven::lnx
