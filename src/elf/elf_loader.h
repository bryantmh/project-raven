#pragma once
// Maps Linux x86-64 ELF images (the game dump, libstdc++, libgcc_s, halo.so) the way ld.so would.
// See docs/architecture.md#loading.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace raven::elf {

struct Symbol {
    uint64_t addr = 0;   // absolute address once mapped
    uint64_t size = 0;
    uint8_t type = 0;    // STT_*
    bool weak = false;
};

struct Image {
    std::string name;            // soname-ish ("game", "libstdc++.so.6")
    std::string path;
    std::vector<uint8_t> file;   // whole file, kept for symbol tables and COPY sources
    uint64_t base = 0;           // load bias (0 for the fixed-address game)
    uint64_t lo = 0, hi = 0;     // mapped address range
    bool executable = false;

    // From PT_DYNAMIC (absolute addresses after mapping)
    uint64_t dyn_addr = 0;
    uint64_t symtab = 0, strtab = 0, versym = 0, verdef = 0, verneed = 0;
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
    uint64_t init = 0, init_array = 0, init_arraysz = 0, fini = 0, fini_array = 0, fini_arraysz = 0;
    uint64_t hash = 0, gnu_hash = 0;
    uint32_t verdefnum = 0, verneednum = 0;
    size_t nsyms = 0;
    std::vector<std::string> needed;

    // PT_TLS
    uint64_t tls_image = 0, tls_filesz = 0, tls_memsz = 0, tls_align = 1;
    size_t tls_module = 0;  // 1-based module id for __tls_get_addr, 0 if none

    // PT_GNU_EH_FRAME (for dl_iterate_phdr / _dl_find_object)
    uint64_t eh_frame_hdr = 0;
    uint64_t phdr_addr = 0;
    uint16_t phnum = 0;
    std::vector<uint8_t> phdr_copy;  // program headers kept in memory for dl_iterate_phdr

    // name -> versions -> symbol. "" holds the default (non-hidden) definition.
    std::unordered_map<std::string, std::unordered_map<std::string, Symbol>> exports;

    const char* str(uint32_t off) const { return reinterpret_cast<const char*>(strtab + off); }
};

// Map an ELF file. The game ("fixed") goes at its link addresses; shared objects anywhere.
Image* load(const std::string& path, const std::string& name, bool fixed);

// Resolve the symbol `name` (optionally at `version`) across the global scope: the game first, then every loaded
// library in load order, then the shim table. Returns 0 if not found.
uint64_t resolve(const std::string& name, const std::string& version, const Image* requester);

// Apply relocations of every loaded image, then COPY relocations, then run initializers (not the game's: its
// __libc_csu_init runs from __libc_start_main).
void link_all();
void run_initializers(Image* img);

const std::vector<Image*>& images();
Image* find_image(uint64_t addr);
Image* find_image(const std::string& name);

// Address of a stub that logs `name` and aborts, for imports nothing provides.
uint64_t missing_stub(const std::string& name, const std::string& who);

}  // namespace raven::elf
