// Game-specific loading steps. docs/architecture.md#the-embedded-import-table and #thread-local-storage.
#include "game/boot.h"

#include <windows.h>

#include <cstring>

#include "elf/eh_frame.h"
#include "log.h"

namespace raven::game {

namespace {
constexpr uint64_t kImportTable = 0x1f4d938;  // protector import table: 0x24-byte entries
constexpr uint64_t kImportStrings = 0x1f492cc;
}  // namespace

int bind_embedded_imports(elf::Image& game) {
    int n = 0, missing = 0;
    for (uint64_t e = kImportTable;; e += 0x24) {
        uint32_t type = *reinterpret_cast<const uint32_t*>(e + 0x20);
        if (type != 6 && type != 7) break;
        uint32_t name_off = *reinterpret_cast<const uint32_t*>(e);
        uint32_t ver_off = *reinterpret_cast<const uint32_t*>(e + 4);
        uint64_t got = *reinterpret_cast<const uint64_t*>(e + 8);
        const char* name = reinterpret_cast<const char*>(kImportStrings + name_off);
        const char* ver = reinterpret_cast<const char*>(kImportStrings + ver_off);
        uint64_t a = elf::resolve(name, ver, &game);
        if (!a) {
            a = elf::missing_stub(name, "game");
            missing++;
        }
        *reinterpret_cast<uint64_t*>(got) = a;
        n++;
    }
    RLOG("bound %d embedded imports (%d not implemented yet)", n, missing);
    return n;
}

int patch_stack_protector(elf::Image& img, DWORD tls_slot) {
    if (tls_slot >= 64) log_fatal("No TEB TLS slot below 64 for the stack protector (got %lu)", tls_slot);
    const uint32_t disp = 0x1480 + tls_slot * 8;
    int patched = 0;
    for (auto [a, b] : elf::fde_ranges(img)) {
        auto p = reinterpret_cast<uint8_t*>(a);
        auto end = reinterpret_cast<uint8_t*>(b);
        for (; p + 9 <= end; p++) {
            // 64 REX.W(48|4c) (8b|33|2b|3b) modrm(mod=00 rm=100) 25 28000000:  mov/xor/sub/cmp reg, qword fs:[0x28]
            if (p[0] != 0x64 || (p[1] != 0x48 && p[1] != 0x4c) || (p[2] != 0x8b && p[2] != 0x33 && p[2] != 0x2b && p[2] != 0x3b) ||
                (p[3] & 0xc7) != 0x04 || p[4] != 0x25 || p[5] != 0x28 || p[6] || p[7] || p[8])
                continue;
            p[0] = 0x65;  // gs:
            std::memcpy(p + 5, &disp, 4);
            patched++;
            p += 8;
        }
    }
    RLOG("%s: %d stack protector reads moved to TEB slot %lu", img.name.c_str(), patched, tls_slot);
    return patched;
}

}  // namespace raven::game
