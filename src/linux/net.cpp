// Networking. The cabinet's online features (operator telemetry, leaderboards) have no server to talk to, so
// sockets are refused and name lookups fail; the game treats that as being offline. poll/select cover the fds the
// loader does provide (files and emulated devices).
#include <cstring>

#include "linux/fd.h"
#include "linux/lnx.h"
#include "linux/process_hooks.h"
#include "log.h"

using namespace raven::lnx;

SHIM(int, socket, (int domain, int type, int proto)) {
    RTRACE("socket(%d, %d, %d) refused", domain, type, proto);
    return fail(L_EAFNOSUPPORT);
}
SHIM(int, connect, (int, const void*, unsigned)) { return fail(L_ENETUNREACH); }
SHIM(int, bind, (int, const void*, unsigned)) { return fail(L_EBADF); }
SHIM(int, listen, (int, int)) { return fail(L_EBADF); }
SHIM(int, accept, (int, void*, unsigned*)) { return fail(L_EBADF); }
SHIM(int, shutdown, (int, int)) { return fail(L_EBADF); }
SHIM(int64_t, send, (int, const void*, size_t, int)) { return fail(L_EBADF); }
SHIM(int64_t, sendto, (int, const void*, size_t, int, const void*, unsigned)) { return fail(L_EBADF); }
SHIM(int64_t, recv, (int, void*, size_t, int)) { return fail(L_EBADF); }
SHIM(int64_t, recvfrom, (int, void*, size_t, int, void*, unsigned*)) { return fail(L_EBADF); }
SHIM(int, setsockopt, (int, int, int, const void*, unsigned)) { return fail(L_EBADF); }
SHIM(int, getsockopt, (int, int, int, void*, unsigned*)) { return fail(L_EBADF); }
SHIM(int, getsockname, (int, void*, unsigned*)) { return fail(L_EBADF); }
SHIM(int, getpeername, (int, void*, unsigned*)) { return fail(L_EBADF); }
SHIM(int, getaddrinfo, (const char* node, const char*, const void*, void** res)) {
    RTRACE("getaddrinfo(%s) refused", node ? node : "");
    *res = nullptr;
    return -2;  // EAI_NONAME
}
SHIM(void, freeaddrinfo, (void*)) {}
SHIM(const char*, gai_strerror, (int)) { return "Name or service not known"; }
SHIM(int, getnameinfo, (const void*, unsigned, char*, unsigned, char*, unsigned, int)) { return -2; }
SHIM(void*, gethostbyname, (const char*)) { return nullptr; }
SHIM(int, gethostbyname_r, (const char*, void*, char*, size_t, void** res, int* herr)) {
    *res = nullptr;
    if (herr) *herr = 1;
    return L_ENOENT;
}
SHIM(void*, getservbyname, (const char*, const char*)) { return nullptr; }
SHIM(int, getifaddrs, (void** out)) { *out = nullptr; return 0; }
SHIM(void, freeifaddrs, (void*)) {}
SHIM(uint32_t, inet_addr, (const char* s)) {
    unsigned a, b, c, d;
    if (std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0xffffffffu;
    return a | (b << 8) | (c << 16) | (d << 24);
}
SHIM(char*, inet_ntoa, (uint32_t in)) {
    static thread_local char buf[16];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", in & 255, (in >> 8) & 255, (in >> 16) & 255, in >> 24);
    return buf;
}
SHIM(const char*, inet_ntop, (int af, const void* src, char* dst, unsigned n)) {
    if (af != 2) return nullptr;
    auto b = static_cast<const uint8_t*>(src);
    std::snprintf(dst, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return dst;
}
SHIM(int, inet_pton, (int af, const char* s, void* dst)) {
    if (af != 2) return -1;
    unsigned a, b, c, d;
    if (std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    uint8_t v[4] = {uint8_t(a), uint8_t(b), uint8_t(c), uint8_t(d)};
    std::memcpy(dst, v, 4);
    return 1;
}
static uint8_t g_in6addr_any[16];
SHIM_VAR(in6addr_any, g_in6addr_any);

struct lpollfd { int fd; short events, revents; };
SHIM(int, poll, (lpollfd* fds, uint64_t n, int timeout)) {
    int64_t end = mono_ns() + int64_t(timeout) * 1000000;
    for (;;) {
        int ready = 0;
        for (uint64_t i = 0; i < n; i++) {
            fds[i].revents = 0;
            auto e = fd_get(fds[i].fd);
            if (!e) { fds[i].revents = 0x20; ready++; continue; }  // POLLNVAL
            bool rd = e->kind == FdEntry::Dev ? e->dev->readable() : e->kind != FdEntry::Socket;
            if ((fds[i].events & 1) && rd) fds[i].revents |= 1;
            if ((fds[i].events & 4) && e->kind != FdEntry::Socket) fds[i].revents |= 4;
            if (fds[i].revents) ready++;
        }
        if (ready || timeout == 0 || (timeout > 0 && mono_ns() >= end)) return ready;
        Sleep(1);
    }
}
SHIM(int, select, (int nfds, uint64_t* r, uint64_t* w, uint64_t* x, ltimeval* tv)) {
    int ready = 0;
    for (int fd = 0; fd < nfds; fd++) {
        uint64_t bit = 1ull << (fd % 64);
        auto e = fd_get(fd);
        bool rd = e && (e->kind == FdEntry::Dev ? e->dev->readable() : e->kind != FdEntry::Socket);
        if (r && (r[fd / 64] & bit)) { if (rd) ready++; else r[fd / 64] &= ~bit; }
        if (w && (w[fd / 64] & bit)) { if (e && e->kind != FdEntry::Socket) ready++; else w[fd / 64] &= ~bit; }
        if (x) x[fd / 64] &= ~bit;
    }
    if (!ready && tv) precise_sleep_ns(tv->tv_sec * 1000000000ll + tv->tv_usec * 1000);
    return ready;
}
SHIM(int, __fdelt_chk, (int64_t d)) { return int(d / 64); }
