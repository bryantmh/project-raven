#pragma once
// Emulated devices the game can find through libudev (src/linux/udev.cpp).
#include <string>
#include <utility>
#include <vector>

namespace raven::lnx {
struct UdevDevice {
    std::string syspath;    // /sys/devices/...
    std::string sysname;    // hidraw0
    std::string devnode;    // /dev/hidraw0 (registered with register_device)
    std::string subsystem;  // hidraw, tty, usb, input
    std::vector<std::pair<std::string, std::string>> properties, sysattrs;
};
void udev_register(const UdevDevice& d);
}  // namespace raven::lnx
