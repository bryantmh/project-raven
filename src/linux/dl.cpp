// libdl, the dynamic linker's runtime interface (TLS, unwinder lookups) and memory mapping.
#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "elf/elf_loader.h"
#include "linux/fd.h"
#include "linux/lnx.h"
#include "log.h"

using namespace raven::lnx;
extern "C" SYSV int64_t lx_lseek(int, int64_t, int);
using raven::elf::Image;

namespace {
struct DlHandle {
    enum Kind { Global, Elf, Virtual } kind;
    Image* img = nullptr;
    std::string name;
};
DlHandle g_global{DlHandle::Global};
std::mutex g_dl_mutex;
std::map<std::string, DlHandle*> g_virtual;
thread_local std::string t_dlerror;
thread_local bool t_dlerror_set = false;

// Libraries the shim layer stands in for as a whole: dlopen succeeds and dlsym looks in the shim table.
const std::set<std::string>& virtual_libs() {
    static const std::set<std::string> s = {"libGL.so.1", "libGL.so", "libc.so.6", "libm.so.6", "libpthread.so.0",
                                            "libdl.so.2", "librt.so.1"};
    return s;
}

std::string base_name(const char* p) {
    const char* s = std::strrchr(p, '/');
    return s ? s + 1 : p;
}
}  // namespace

SHIM(void*, dlopen, (const char* file, int flags)) {
    if (!file) return &g_global;
    std::string n = base_name(file);
    std::lock_guard<std::mutex> lk(g_dl_mutex);
    if (Image* im = raven::elf::find_image(n)) return new DlHandle{DlHandle::Elf, im, n};
    if (virtual_libs().count(n)) {
        auto& h = g_virtual[n];
        if (!h) h = new DlHandle{DlHandle::Virtual, nullptr, n};
        return h;
    }
    RLOG("dlopen(%s): not available", file);
    t_dlerror = std::string(file) + ": cannot open shared object file: No such file or directory";
    t_dlerror_set = true;
    return nullptr;
}
SHIM(int, dlclose, (void*)) { return 0; }
SHIM(char*, dlerror, ()) {
    if (!t_dlerror_set) return nullptr;
    t_dlerror_set = false;
    return const_cast<char*>(t_dlerror.c_str());
}
static void* dlsym_impl(void* handle, const char* name, const char* version) {
    auto h = static_cast<DlHandle*>(handle);
    uint64_t a = 0;
    if (!h || h == reinterpret_cast<DlHandle*>(-1) /* RTLD_NEXT */ || h->kind == DlHandle::Global) {
        a = raven::elf::resolve(name, version ? version : "", nullptr);
    } else if (h->kind == DlHandle::Elf) {
        auto it = h->img->exports.find(name);
        if (it != h->img->exports.end()) {
            auto jt = it->second.find(version ? version : "");
            if (jt == it->second.end()) jt = it->second.find("");
            if (jt != it->second.end()) a = jt->second.addr;
        }
    } else {
        a = reinterpret_cast<uint64_t>(raven::shim::lookup(name));
    }
    RTRACE("dlsym(%s, %s) = %#llx", h && h->kind != DlHandle::Global ? h->name.c_str() : "global", name, a);
    if (!a) {
        t_dlerror = std::string("undefined symbol: ") + name;
        t_dlerror_set = true;
    }
    return reinterpret_cast<void*>(a);
}
SHIM(void*, dlsym, (void* h, const char* name)) { return dlsym_impl(h, name, nullptr); }
SHIM(void*, dlvsym, (void* h, const char* name, const char* ver)) { return dlsym_impl(h, name, ver); }

struct LDlInfo { const char* fname; void* fbase; const char* sname; void* saddr; };
SHIM(int, dladdr, (const void* addr, LDlInfo* info)) {
    Image* im = raven::elf::find_image(reinterpret_cast<uint64_t>(addr));
    if (!im) return 0;
    info->fname = im->executable ? "/pm/g7/halo/game2" : im->path.c_str();
    info->fbase = reinterpret_cast<void*>(im->lo);
    info->sname = nullptr;
    info->saddr = nullptr;
    uint64_t best = 0;
    for (auto& [n, vers] : im->exports)
        for (auto& [v, s] : vers)
            if (s.addr <= uint64_t(addr) && s.addr > best && uint64_t(addr) < s.addr + std::max<uint64_t>(s.size, 1)) {
                best = s.addr;
                info->sname = n.c_str();
                info->saddr = reinterpret_cast<void*>(s.addr);
            }
    return 1;
}
SHIM(int, dladdr1, (const void* addr, LDlInfo* info, void** extra, int)) {
    if (extra) *extra = nullptr;
    return lx_dladdr(addr, info);
}
SHIM(int, dlinfo, (void*, int, void*)) { return -1; }

struct LPhdrInfo {
    uint64_t addr;
    const char* name;
    const void* phdr;
    uint16_t phnum;
    unsigned long long adds, subs;
    size_t tls_modid;
    void* tls_data;
};
SHIM(int, dl_iterate_phdr, (SYSV int (*cb)(LPhdrInfo*, size_t, void*), void* data)) {
    for (Image* im : raven::elf::images()) {
        LPhdrInfo info{im->base, im->executable ? "" : im->path.c_str(), reinterpret_cast<const void*>(im->phdr_addr),
                       im->phnum, raven::elf::images().size(), 0, im->tls_module, nullptr};
        if (int r = cb(&info, sizeof info, data)) return r;
    }
    return 0;
}
struct LDlFindObject {
    unsigned long long flags;
    void* map_start;
    void* map_end;
    void* link_map;
    void* eh_frame;
    unsigned long long reserved[7];
};
static uint64_t g_fake_link_map[8];
SHIM(int, _dl_find_object, (void* pc, LDlFindObject* out)) {
    Image* im = raven::elf::find_image(reinterpret_cast<uint64_t>(pc));
    if (!im) return -1;
    out->flags = 0;
    out->map_start = reinterpret_cast<void*>(im->lo);
    out->map_end = reinterpret_cast<void*>(im->hi);
    out->link_map = g_fake_link_map;
    out->eh_frame = reinterpret_cast<void*>(im->eh_frame_hdr);
    return 0;
}
// r_debug: present for halo.so's plthook, which only reads it when walking link maps (it doesn't here).
static uint64_t g_r_debug[5];
SHIM_VAR(_r_debug, g_r_debug);

// ---- TLS for general-dynamic code (libstdc++, halo.so). docs/architecture.md#thread-local-storage
struct TlsIndex { uint64_t module, offset; };
static thread_local std::vector<uint8_t*> t_tls_blocks;
SHIM(void*, __tls_get_addr, (const TlsIndex* ti)) {
    auto& blocks = t_tls_blocks;
    if (blocks.size() <= ti->module) blocks.resize(ti->module + 1, nullptr);
    uint8_t*& b = blocks[ti->module];
    if (!b) {
        for (Image* im : raven::elf::images())
            if (im->tls_module == ti->module) {
                b = static_cast<uint8_t*>(_aligned_malloc(std::max<uint64_t>(im->tls_memsz, 16), std::max<uint64_t>(im->tls_align, 16)));
                std::memcpy(b, reinterpret_cast<void*>(im->tls_image), im->tls_filesz);
                std::memset(b + im->tls_filesz, 0, im->tls_memsz - im->tls_filesz);
                break;
            }
        if (!b) raven::log_fatal("__tls_get_addr: unknown TLS module %llu", (unsigned long long)ti->module);
    }
    return b + ti->offset;
}
// libstdc++ checks this to skip atomics in single-threaded programs; the game is multi-threaded.
static char g_single_threaded = 0;
SHIM_VAR(__libc_single_threaded, &g_single_threaded);

// ---- memory mapping
namespace {
DWORD win_prot(int prot) {
    bool r = prot & 1, w = prot & 2, x = prot & 4;
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
}  // namespace

SHIM(void*, mmap, (void* addr, size_t len, int prot, int flags, int fd, int64_t off)) {
    constexpr int MAP_FIXED = 0x10, MAP_ANON = 0x20;
    void* p = nullptr;
    if (addr) p = VirtualAlloc(addr, len, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!p && addr && (flags & MAP_FIXED)) {
        // Fixed mapping over memory we already own (re-mapping): just make sure it's committed.
        p = VirtualAlloc(addr, len, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) std::memset(addr, 0, len);
    }
    if (!p && !(flags & MAP_FIXED)) p = VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!p) { fail(L_ENOMEM); return reinterpret_cast<void*>(-1); }
    if (!(flags & MAP_ANON) && fd >= 0) {
        lx_lseek(fd, off, 0);
        size_t got = 0;
        while (got < len) {
            int64_t r = fd_read(fd, static_cast<uint8_t*>(p) + got, len - got);
            if (r <= 0) break;
            got += size_t(r);
        }
    }
    DWORD old;
    VirtualProtect(p, len, win_prot(prot), &old);
    return p;
}
SHIM_ALIAS(mmap64, mmap);
SHIM(int, munmap, (void* addr, size_t len)) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof mbi) && mbi.AllocationBase == addr && mbi.RegionSize <= len)
        VirtualFree(addr, 0, MEM_RELEASE);
    else
        VirtualFree(addr, len, MEM_DECOMMIT);
    return 0;
}
SHIM(int, mprotect, (void* addr, size_t len, int prot)) {
    DWORD old;
    if (!VirtualProtect(addr, len, win_prot(prot), &old)) {
        // Linux allows mprotect on reserved-but-uncommitted ranges; ours are all committed. Ignore failures on
        // the game image, whose pages are RWX already.
        return 0;
    }
    return 0;
}
SHIM(int, madvise, (void*, size_t, int)) { return 0; }
SHIM(int, mlock, (const void*, size_t)) { return 0; }
SHIM(int, munlock, (const void*, size_t)) { return 0; }
