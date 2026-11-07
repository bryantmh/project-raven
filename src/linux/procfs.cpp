// /proc, /sys and /etc files the game reads, generated from the real machine where it matters.
#include <windows.h>
#include <intrin.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "linux/fd.h"
#include "linux/lnx.h"

using namespace raven::lnx;

namespace {
std::string cpu_brand() {
    int r[4];
    char brand[49] = {};
    for (int i = 0; i < 3; i++) {
        __cpuid(r, 0x80000002 + i);
        std::memcpy(brand + i * 16, r, 16);
    }
    std::string s = brand;
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}
double cpu_mhz() {
    HKEY k;
    DWORD mhz = 3000, sz = sizeof mhz;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == 0) {
        RegQueryValueExW(k, L"~MHz", nullptr, nullptr, reinterpret_cast<LPBYTE>(&mhz), &sz);
        RegCloseKey(k);
    }
    return double(mhz);
}

std::string cpuinfo() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::string out, brand = cpu_brand();
    char buf[512];
    for (DWORD i = 0; i < si.dwNumberOfProcessors; i++) {
        std::snprintf(buf, sizeof buf,
                      "processor\t: %lu\nvendor_id\t: GenuineIntel\nmodel name\t: %s\ncpu MHz\t\t: %.3f\n"
                      "cache size\t: 8192 KB\ncpu cores\t: %lu\nflags\t\t: fpu sse sse2 ssse3 sse4_1 sse4_2 avx avx2\n\n",
                      i, brand.c_str(), cpu_mhz(), si.dwNumberOfProcessors);
        out += buf;
    }
    return out;
}

std::string meminfo() {
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    char buf[256];
    std::snprintf(buf, sizeof buf, "MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\n",
                  ms.ullTotalPhys / 1024, ms.ullAvailPhys / 1024, ms.ullAvailPhys / 1024);
    return buf;
}

struct Register {
    Register() {
        register_virtual_file("/proc/cpuinfo", cpuinfo);
        register_virtual_file("/proc/meminfo", meminfo);
        register_virtual_file("/proc/self/status", [] { return std::string("Name:\tgame2\nState:\tR (running)\nTracerPid:\t0\n"); });
        register_virtual_file("/proc/version", [] { return std::string("Linux version 4.15.0 (raven) #1 SMP\n"); });
        register_virtual_file("/sys/class/dmi/id/bios_vendor", [] { return std::string("American Megatrends Inc.\n"); });
        register_virtual_file("/sys/class/dmi/id/bios_date", [] { return std::string("01/01/2018\n"); });
        register_virtual_file("/sys/class/dmi/id/bios_version", [] { return std::string("1.0\n"); });
        register_virtual_file("/sys/devices/system/cpu/present", [] {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            return "0-" + std::to_string(si.dwNumberOfProcessors - 1) + "\n";
        });
        register_virtual_file("/etc/resolv.conf", [] { return std::string("nameserver 127.0.0.1\n"); });
        register_virtual_file("/etc/hosts", [] { return std::string("127.0.0.1\tlocalhost\n"); });
    }
} g_register;
}  // namespace
