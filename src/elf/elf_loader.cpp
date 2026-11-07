// ELF loading, symbol lookup and relocation. See docs/architecture.md#loading.
#include "elf/elf_loader.h"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <mutex>

#include "linux/shim.h"
#include "log.h"

namespace raven::elf {
namespace {

constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, PT_GNU_EH_FRAME = 0x6474e550;
constexpr int64_t DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7,
                  DT_RELASZ = 8, DT_INIT = 12, DT_FINI = 13, DT_JMPREL = 23, DT_INIT_ARRAY = 25,
                  DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28, DT_GNU_HASH = 0x6ffffef5,
                  DT_VERSYM = 0x6ffffff0, DT_VERDEF = 0x6ffffffc, DT_VERDEFNUM = 0x6ffffffd,
                  DT_VERNEED = 0x6ffffffe, DT_VERNEEDNUM = 0x6fffffff;
constexpr uint32_t R_64 = 1, R_COPY = 5, R_GLOB_DAT = 6, R_JUMP_SLOT = 7, R_RELATIVE = 8, R_DTPMOD64 = 16,
                   R_DTPOFF64 = 17, R_TPOFF64 = 18, R_IRELATIVE = 37;
constexpr uint8_t STT_OBJECT = 1, STT_TLS = 6, STB_WEAK = 2;

#pragma pack(push, 1)
struct Ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct Sym {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
};
struct Rela {
    uint64_t offset, info;
    int64_t addend;
};
struct Verdef {
    uint16_t version, flags, ndx, cnt;
    uint32_t hash, aux, next;
};
struct Verdaux {
    uint32_t name, next;
};
struct Verneed {
    uint16_t version, cnt;
    uint32_t file, aux, next;
};
struct Vernaux {
    uint32_t hash;
    uint16_t flags, other;
    uint32_t name, next;
};
#pragma pack(pop)

std::vector<Image*> g_images;
size_t g_next_tls_module = 1;

size_t count_symbols(const Image& im) {
    if (im.hash) return reinterpret_cast<const uint32_t*>(im.hash)[1];
    if (!im.gnu_hash) return 0;
    auto h = reinterpret_cast<const uint32_t*>(im.gnu_hash);
    uint32_t nbuckets = h[0], symoffset = h[1], bloom = h[2];
    auto buckets = reinterpret_cast<const uint32_t*>(im.gnu_hash + 16 + bloom * 8);
    auto chains = buckets + nbuckets;
    uint32_t hi = 0;
    for (uint32_t i = 0; i < nbuckets; i++) hi = buckets[i] > hi ? buckets[i] : hi;
    if (hi < symoffset) return symoffset;
    while (!(chains[hi - symoffset] & 1)) hi++;
    return hi + 1;
}

// Version index -> version name, from VERDEF (what this image defines) and VERNEED (what it requires).
std::unordered_map<uint16_t, std::string> version_names(const Image& im) {
    std::unordered_map<uint16_t, std::string> out;
    if (im.verdef) {
        uint64_t p = im.verdef;
        for (uint32_t i = 0; i < im.verdefnum; i++) {
            auto vd = reinterpret_cast<const Verdef*>(p);
            auto aux = reinterpret_cast<const Verdaux*>(p + vd->aux);
            if (!(vd->flags & 1)) out[vd->ndx] = im.str(aux->name);  // skip VER_FLG_BASE (the soname)
            if (!vd->next) break;
            p += vd->next;
        }
    }
    if (im.verneed) {
        uint64_t p = im.verneed;
        for (uint32_t i = 0; i < im.verneednum; i++) {
            auto vn = reinterpret_cast<const Verneed*>(p);
            uint64_t a = p + vn->aux;
            for (uint16_t k = 0; k < vn->cnt; k++) {
                auto va = reinterpret_cast<const Vernaux*>(a);
                out[va->other] = im.str(va->name);
                if (!va->next) break;
                a += va->next;
            }
            if (!vn->next) break;
            p += vn->next;
        }
    }
    return out;
}

void collect_exports(Image& im) {
    auto syms = reinterpret_cast<const Sym*>(im.symtab);
    auto vers = version_names(im);
    auto vs = reinterpret_cast<const uint16_t*>(im.versym);
    for (size_t i = 1; i < im.nsyms; i++) {
        const Sym& s = syms[i];
        if (!s.name || s.shndx == 0) continue;
        uint8_t type = s.info & 15, bind = s.info >> 4;
        if (bind == 0) continue;  // local
        Symbol out;
        out.addr = type == STT_TLS ? s.value : s.value + im.base;
        out.size = s.size;
        out.type = type;
        out.weak = bind == STB_WEAK;
        std::string name = im.str(s.name);
        uint16_t v = vs ? vs[i] : 1;
        bool hidden = v & 0x8000;
        auto it = vers.find(v & 0x7fff);
        auto& slot = im.exports[name];
        if (it != vers.end()) slot[it->second] = out;
        if (!hidden) slot[""] = out;
    }
}

struct Resolved {
    uint64_t addr = 0;
    const Image* img = nullptr;  // null for shims
    uint8_t type = 0;
    uint64_t size = 0;
    bool found = false;
};

Resolved lookup(const std::string& name, const std::string& version, const Image* skip) {
    // Exact version first, then the default definition, across the global scope (executable, then libraries).
    for (int pass = 0; pass < 2; pass++) {
        const std::string& v = pass == 0 ? version : std::string();
        if (pass == 0 && version.empty()) continue;
        for (Image* im : g_images) {
            if (im == skip) continue;
            auto it = im->exports.find(name);
            if (it == im->exports.end()) continue;
            auto jt = it->second.find(v);
            if (jt == it->second.end()) continue;
            return {jt->second.addr, im, jt->second.type, jt->second.size, true};
        }
    }
    if (void* p = shim::lookup(name.c_str())) return {reinterpret_cast<uint64_t>(p), nullptr, 0, 0, true};
    return {};
}

SYSV void missing_called(const char* name, const char* who) {
    log_fatal("The game called %s (imported by %s), which Project Raven doesn't implement yet.", name, who);
}

uint8_t* g_stub_page = nullptr;
size_t g_stub_used = 0;
std::mutex g_stub_mutex;

}  // namespace

const std::vector<Image*>& images() { return g_images; }

Image* find_image(uint64_t addr) {
    for (Image* im : g_images)
        if (addr >= im->lo && addr < im->hi) return im;
    return nullptr;
}

Image* find_image(const std::string& name) {
    for (Image* im : g_images)
        if (im->name == name) return im;
    return nullptr;
}

uint64_t missing_stub(const std::string& name, const std::string& who) {
    std::lock_guard<std::mutex> lk(g_stub_mutex);
    RLOG("unresolved: %s (%s)", name.c_str(), who.c_str());
    if (!g_stub_page || g_stub_used + 64 > 0x10000) {
        g_stub_page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        g_stub_used = 0;
    }
    char* n = _strdup(name.c_str());
    char* w = _strdup(who.c_str());
    uint8_t* p = g_stub_page + g_stub_used;
    uint8_t* q = p;
    auto imm = [&](uint8_t a, uint8_t b, uint64_t v) { *q++ = a; *q++ = b; std::memcpy(q, &v, 8); q += 8; };
    imm(0x48, 0xBF, reinterpret_cast<uint64_t>(n));               // mov rdi, name
    imm(0x48, 0xBE, reinterpret_cast<uint64_t>(w));               // mov rsi, who
    imm(0x48, 0xB8, reinterpret_cast<uint64_t>(&missing_called)); // mov rax, handler
    *q++ = 0xFF; *q++ = 0xE0;                                     // jmp rax
    g_stub_used += 64;
    return reinterpret_cast<uint64_t>(p);
}

Image* load(const std::string& path, const std::string& name, bool fixed) {
    auto im = new Image;
    im->name = name;
    im->path = path;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) log_fatal("Can't open %s", path.c_str());
        im->file.assign(std::istreambuf_iterator<char>(f), {});
    }
    auto& d = im->file;
    auto eh = reinterpret_cast<const Ehdr*>(d.data());
    if (d.size() < sizeof(Ehdr) || std::memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->machine != 62)
        log_fatal("%s is not an x86-64 ELF file", path.c_str());
    im->executable = eh->type == 2;
    auto ph = reinterpret_cast<const Phdr*>(d.data() + eh->phoff);
    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD) {
            lo = std::min(lo, ph[i].vaddr & ~0xfffull);
            hi = std::max(hi, (ph[i].vaddr + ph[i].memsz + 0xfff) & ~0xfffull);
        }
    uint8_t* mem;
    if (fixed) {
        mem = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(lo), hi - lo, MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (!mem) mem = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(lo), hi - lo, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (mem != reinterpret_cast<uint8_t*>(lo))
            log_fatal("Can't map %s at %#llx (error %lu)", path.c_str(), lo, GetLastError());
        im->base = 0;
    } else {
        mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, hi - lo, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (!mem) log_fatal("Out of memory mapping %s", path.c_str());
        im->base = reinterpret_cast<uint64_t>(mem) - lo;
    }
    im->lo = lo + im->base;
    im->hi = hi + im->base;
    for (int i = 0; i < eh->phnum; i++) {
        const Phdr& p = ph[i];
        if (p.type == PT_LOAD) {
            if (p.offset + p.filesz > d.size()) log_fatal("%s is truncated", path.c_str());
            std::memcpy(reinterpret_cast<void*>(p.vaddr + im->base), d.data() + p.offset, p.filesz);
        } else if (p.type == PT_DYNAMIC) {
            im->dyn_addr = p.vaddr + im->base;
        } else if (p.type == PT_TLS) {
            im->tls_image = p.vaddr + im->base;
            im->tls_filesz = p.filesz;
            im->tls_memsz = p.memsz;
            im->tls_align = p.align ? p.align : 1;
            im->tls_module = g_next_tls_module++;
        } else if (p.type == PT_GNU_EH_FRAME) {
            im->eh_frame_hdr = p.vaddr + im->base;
        }
    }
    im->phdr_copy.assign(d.data() + eh->phoff, d.data() + eh->phoff + eh->phnum * sizeof(Phdr));
    im->phdr_addr = reinterpret_cast<uint64_t>(im->phdr_copy.data());
    im->phnum = eh->phnum;

    if (im->dyn_addr) {
        std::vector<uint64_t> needed;
        for (auto dyn = reinterpret_cast<const int64_t*>(im->dyn_addr); dyn[0]; dyn += 2) {
            uint64_t v = dyn[1], a = v + im->base;
            switch (dyn[0]) {
                case DT_NEEDED: needed.push_back(v); break;
                case DT_PLTRELSZ: im->pltrelsz = v; break;
                case DT_HASH: im->hash = a; break;
                case DT_GNU_HASH: im->gnu_hash = a; break;
                case DT_STRTAB: im->strtab = a; break;
                case DT_SYMTAB: im->symtab = a; break;
                case DT_RELA: im->rela = a; break;
                case DT_RELASZ: im->relasz = v; break;
                case DT_JMPREL: im->jmprel = a; break;
                case DT_INIT: im->init = a; break;
                case DT_FINI: im->fini = a; break;
                case DT_INIT_ARRAY: im->init_array = a; break;
                case DT_INIT_ARRAYSZ: im->init_arraysz = v; break;
                case DT_FINI_ARRAY: im->fini_array = a; break;
                case DT_FINI_ARRAYSZ: im->fini_arraysz = v; break;
                case DT_VERSYM: im->versym = a; break;
                case DT_VERDEF: im->verdef = a; break;
                case DT_VERDEFNUM: im->verdefnum = (uint32_t)v; break;
                case DT_VERNEED: im->verneed = a; break;
                case DT_VERNEEDNUM: im->verneednum = (uint32_t)v; break;
            }
        }
        for (uint64_t n : needed) im->needed.emplace_back(im->str((uint32_t)n));
        im->nsyms = count_symbols(*im);
        collect_exports(*im);
    }
    RLOG("mapped %s at %#llx-%#llx (%zu symbols, tls module %zu)", name.c_str(), im->lo, im->hi, im->nsyms,
         im->tls_module);
    g_images.push_back(im);
    return im;
}

uint64_t resolve(const std::string& name, const std::string& version, const Image* requester) {
    (void)requester;
    return lookup(name, version, nullptr).addr;
}

namespace {

struct ImportInfo {
    std::string name, version;
    uint8_t type;
    bool weak;
};

ImportInfo import_info(const Image& im, uint32_t idx, const std::unordered_map<uint16_t, std::string>& vers) {
    auto s = reinterpret_cast<const Sym*>(im.symtab) + idx;
    ImportInfo out{im.str(s->name), "", uint8_t(s->info & 15), (s->info >> 4) == STB_WEAK};
    if (im.versym) {
        uint16_t v = reinterpret_cast<const uint16_t*>(im.versym)[idx] & 0x7fff;
        auto it = vers.find(v);
        if (it != vers.end()) out.version = it->second;
    }
    return out;
}

// Zero-filled storage for data imports nothing provides, so reading one gives 0 instead of crashing.
uint64_t missing_data(const std::string& name, const std::string& who) {
    RLOG("warning: data symbol %s (imported by %s) is not provided; using zeroed storage", name.c_str(), who.c_str());
    return reinterpret_cast<uint64_t>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

struct CopyReloc {
    uint64_t dest, size;
    std::string name, version;
    const Image* from;
};
std::vector<CopyReloc> g_copies;

void relocate(Image& im) {
    auto vers = version_names(im);
    auto apply = [&](uint64_t table, uint64_t size) {
        auto r = reinterpret_cast<const Rela*>(table);
        for (size_t i = 0; i < size / sizeof(Rela); i++) {
            uint32_t type = r[i].info & 0xffffffff, sym = r[i].info >> 32;
            auto P = reinterpret_cast<uint64_t*>(r[i].offset + im.base);
            int64_t A = r[i].addend;
            if (type == R_RELATIVE) { *P = im.base + A; continue; }
            if (type == R_IRELATIVE) {
                auto resolver = reinterpret_cast<SYSV uint64_t (*)()>(im.base + A);
                *P = resolver();
                continue;
            }
            Resolved S;
            ImportInfo info{};
            if (sym) {
                info = import_info(im, sym, vers);
                // A symbol the image defines itself still goes through the global scope (interposition), except
                // for COPY, whose source must be another image.
                S = lookup(info.name, info.version, type == R_COPY ? &im : nullptr);
            }
            switch (type) {
                case R_64:
                case R_GLOB_DAT:
                case R_JUMP_SLOT:
                    if (!S.found) {
                        if (info.weak && type != R_JUMP_SLOT) { *P = 0; break; }
                        S.addr = info.type == STT_OBJECT ? missing_data(info.name, im.name)
                                                         : missing_stub(info.name, im.name);
                    }
                    *P = S.addr + (type == R_64 ? A : 0);
                    break;
                case R_COPY:
                    if (!S.found) {
                        RLOG("warning: COPY source for %s not found", info.name.c_str());
                        break;
                    }
                    g_copies.push_back({r[i].offset + im.base, reinterpret_cast<const Sym*>(im.symtab)[sym].size,
                                        info.name, info.version, S.img});
                    break;
                case R_DTPMOD64:
                    *P = sym ? (S.img ? S.img->tls_module : 0) : im.tls_module;
                    break;
                case R_DTPOFF64:
                    *P = (sym ? S.addr : 0) + A;
                    break;
                case R_TPOFF64:
                    log_fatal("%s uses static TLS (TPOFF64 for %s), which isn't supported", im.name.c_str(),
                              info.name.c_str());
                default:
                    log_fatal("%s: unsupported relocation type %u", im.name.c_str(), type);
            }
        }
    };
    if (im.rela) apply(im.rela, im.relasz);
    if (im.jmprel) apply(im.jmprel, im.pltrelsz);
}

}  // namespace

void link_all() {
    for (Image* im : g_images) relocate(*im);
    for (auto& c : g_copies) {
        // The executable's copy is canonical: copy the library's initial contents in. Lookups already bind
        // to the executable's address because it comes first in the scope.
        Resolved src;
        for (Image* im : g_images) {
            if (im->executable) continue;
            auto it = im->exports.find(c.name);
            if (it == im->exports.end()) continue;
            auto jt = it->second.find(c.version);
            if (jt == it->second.end()) jt = it->second.find("");
            if (jt == it->second.end()) continue;
            src = {jt->second.addr, im, jt->second.type, jt->second.size, true};
            break;
        }
        if (!src.found) {
            if (void* p = shim::lookup(c.name.c_str())) src = {reinterpret_cast<uint64_t>(p), nullptr, 0, c.size, true};
        }
        if (src.found) std::memcpy(reinterpret_cast<void*>(c.dest), reinterpret_cast<void*>(src.addr), c.size);
        else RLOG("warning: nothing provides COPY symbol %s", c.name.c_str());
    }
}

void run_initializers(Image* im) {
    using InitFn = SYSV void (*)(int, char**, char**);
    if (im->init) reinterpret_cast<InitFn>(im->init)(0, nullptr, nullptr);
    auto arr = reinterpret_cast<const uint64_t*>(im->init_array);
    for (size_t i = 0; i < im->init_arraysz / 8; i++)
        if (arr[i] && arr[i] != ~0ull) reinterpret_cast<InitFn>(arr[i])(0, nullptr, nullptr);
}

}  // namespace raven::elf
