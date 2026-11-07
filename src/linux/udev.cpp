// libudev over a registry of emulated devices (src/linux/udev.h). The game enumerates USB/hidraw/tty devices to
// find its I/O board; only devices the loader emulates are ever listed. libiw (wireless tools) reports no devices.
#include "linux/udev.h"

#include <mutex>
#include <string>
#include <vector>

#include "linux/fd.h"
#include "linux/lnx.h"
#include "log.h"

using namespace raven::lnx;

namespace raven::lnx {
namespace {
std::mutex g_mutex;
std::vector<UdevDevice>& registry() {
    static std::vector<UdevDevice> v;
    return v;
}
}  // namespace
void udev_register(const UdevDevice& d) {
    std::lock_guard<std::mutex> lk(g_mutex);
    registry().push_back(d);
}
}  // namespace raven::lnx

namespace {
struct ListEntry {
    std::string name, value;
    ListEntry* next = nullptr;
};
struct Udev { int refs = 1; };
struct UdevDev {
    UdevDevice d;
    std::string action;
};
struct Enumerate {
    std::vector<std::string> subsystems;
    std::vector<ListEntry*> entries;
};
struct Monitor { int fd = -1; };

ListEntry* make_list(const std::vector<std::string>& names) {
    ListEntry* head = nullptr;
    for (auto it = names.rbegin(); it != names.rend(); ++it) head = new ListEntry{*it, "", head};
    return head;
}
}  // namespace

SHIM(Udev*, udev_new, ()) { return new Udev; }
SHIM(Udev*, udev_ref, (Udev* u)) { u->refs++; return u; }
SHIM(Udev*, udev_unref, (Udev* u)) { return nullptr; }

SHIM(Enumerate*, udev_enumerate_new, (Udev*)) { return new Enumerate; }
SHIM(Enumerate*, udev_enumerate_unref, (Enumerate* e)) { delete e; return nullptr; }
SHIM(int, udev_enumerate_add_match_subsystem, (Enumerate* e, const char* s)) {
    RLOG("udev: enumerate subsystem %s", s);
    e->subsystems.push_back(s);
    return 0;
}
SHIM(int, udev_enumerate_add_match_property, (Enumerate*, const char*, const char*)) { return 0; }
SHIM(int, udev_enumerate_add_match_sysattr, (Enumerate*, const char*, const char*)) { return 0; }
SHIM(int, udev_enumerate_scan_devices, (Enumerate* e)) {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<std::string> names;
    for (auto& d : registry())
        for (auto& s : e->subsystems)
            if (d.subsystem == s) names.push_back(d.syspath);
    e->entries.push_back(make_list(names));
    return 0;
}
SHIM(ListEntry*, udev_enumerate_get_list_entry, (Enumerate* e)) { return e->entries.empty() ? nullptr : e->entries.back(); }
SHIM(ListEntry*, udev_list_entry_get_next, (ListEntry* l)) { return l ? l->next : nullptr; }
SHIM(const char*, udev_list_entry_get_name, (ListEntry* l)) { return l ? l->name.c_str() : nullptr; }
SHIM(const char*, udev_list_entry_get_value, (ListEntry* l)) { return l ? l->value.c_str() : nullptr; }

SHIM(UdevDev*, udev_device_new_from_syspath, (Udev*, const char* path)) {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& d : registry())
        if (d.syspath == path) return new UdevDev{d, ""};
    return nullptr;
}
SHIM(UdevDev*, udev_device_unref, (UdevDev* d)) { delete d; return nullptr; }
SHIM(const char*, udev_device_get_devnode, (UdevDev* d)) { return d && !d->d.devnode.empty() ? d->d.devnode.c_str() : nullptr; }
SHIM(const char*, udev_device_get_sysname, (UdevDev* d)) { return d ? d->d.sysname.c_str() : nullptr; }
SHIM(const char*, udev_device_get_syspath, (UdevDev* d)) { return d ? d->d.syspath.c_str() : nullptr; }
SHIM(const char*, udev_device_get_subsystem, (UdevDev* d)) { return d ? d->d.subsystem.c_str() : nullptr; }
SHIM(const char*, udev_device_get_action, (UdevDev* d)) { return d && !d->action.empty() ? d->action.c_str() : nullptr; }
SHIM(const char*, udev_device_get_property_value, (UdevDev* d, const char* k)) {
    if (!d) return nullptr;
    for (auto& [key, v] : d->d.properties)
        if (key == k) return v.c_str();
    return nullptr;
}
SHIM(const char*, udev_device_get_sysattr_value, (UdevDev* d, const char* k)) {
    if (!d) return nullptr;
    for (auto& [key, v] : d->d.sysattrs)
        if (key == k) return v.c_str();
    return nullptr;
}

// Hotplug monitor: never reports anything (emulated devices are present from the start).
SHIM(Monitor*, udev_monitor_new_from_netlink, (Udev*, const char*)) {
    auto m = new Monitor;
    auto e = std::make_shared<FdEntry>();
    e->kind = FdEntry::Null;
    m->fd = fd_alloc(e);
    return m;
}
SHIM(int, udev_monitor_filter_add_match_subsystem_devtype, (Monitor*, const char*, const char*)) { return 0; }
SHIM(int, udev_monitor_enable_receiving, (Monitor*)) { return 0; }
SHIM(int, udev_monitor_get_fd, (Monitor* m)) { return m->fd; }
SHIM(UdevDev*, udev_monitor_receive_device, (Monitor*)) { return nullptr; }
SHIM(Monitor*, udev_monitor_unref, (Monitor* m)) { delete m; return nullptr; }

// libiw: no wireless interfaces
SHIM(int, iw_sockets_open, ()) { return -1; }
SHIM(int, iw_get_basic_config, (int, const char*, void*)) { return -1; }
SHIM(int, iw_get_range_info, (int, const char*, void*)) { return -1; }
SHIM(int, iw_get_stats, (int, const char*, void*, const void*, int)) { return -1; }
SHIM(int, iw_init_event_stream, (void*, char*, int)) { return 0; }
SHIM(int, iw_extract_event_stream, (void*, void*, int)) { return 0; }
SHIM(double, iw_freq2float, (const void*)) { return 0; }
SHIM(int, iw_freq_to_channel, (double, const void*)) { return -1; }
SHIM(int, iw_channel_to_freq, (int, double*, const void*)) { return -1; }
SHIM(void, iw_print_bitrate, (char* buf, int n, int)) { if (n) buf[0] = 0; }
SHIM(char*, iw_ether_ntop, (const void*, char* buf)) { buf[0] = 0; return buf; }
SHIM(char*, iw_sawap_ntop, (const void*, char* buf)) { buf[0] = 0; return buf; }
SHIM(int, iw_check_mac_addr_type, (int, const char*)) { return -1; }
static int g_iw_operation_mode[8];
SHIM_VAR(iw_operation_mode, g_iw_operation_mode);
