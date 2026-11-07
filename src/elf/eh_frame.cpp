// Function ranges from .eh_frame (C++ port of tools/funcs.py). Used to restrict code patching to real code.
#include "elf/eh_frame.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace raven::elf {
namespace {
uint64_t uleb(const uint8_t*& p) {
    uint64_t r = 0;
    int s = 0;
    for (;;) {
        uint8_t b = *p++;
        r |= uint64_t(b & 0x7f) << s;
        s += 7;
        if (b < 0x80) return r;
    }
}
int64_t sleb(const uint8_t*& p) {
    int64_t r = 0;
    int s = 0;
    uint8_t b;
    do {
        b = *p++;
        r |= int64_t(b & 0x7f) << s;
        s += 7;
    } while (b >= 0x80);
    if (b & 0x40 && s < 64) r -= int64_t(1) << s;
    return r;
}
uint64_t read_enc(const uint8_t*& p, uint8_t enc) {
    const uint8_t* at = p;
    uint64_t v = 0;
    switch (enc & 0x0f) {
        case 0x00: std::memcpy(&v, p, 8); p += 8; break;
        case 0x01: v = uleb(p); break;
        case 0x03: { uint32_t x; std::memcpy(&x, p, 4); v = x; p += 4; break; }
        case 0x09: v = uint64_t(sleb(p)); break;
        case 0x0b: { int32_t x; std::memcpy(&x, p, 4); v = uint64_t(int64_t(x)); p += 4; break; }
        case 0x0c: std::memcpy(&v, p, 8); p += 8; break;
        default: return 0;
    }
    if ((enc & 0x70) == 0x10) v += reinterpret_cast<uint64_t>(at);  // pcrel: memory is mapped at its address
    return v;
}
}  // namespace

std::vector<std::pair<uint64_t, uint64_t>> fde_ranges(const Image& im) {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    if (!im.eh_frame_hdr) return out;
    const uint8_t* h = reinterpret_cast<const uint8_t*>(im.eh_frame_hdr);
    const uint8_t* p = h + 4;
    uint64_t eh_frame = read_enc(p, h[1]);
    std::map<const uint8_t*, uint8_t> cie_enc;
    const uint8_t* q = reinterpret_cast<const uint8_t*>(eh_frame);
    for (;;) {
        uint32_t len;
        std::memcpy(&len, q, 4);
        if (len == 0 || len == 0xffffffff) break;
        const uint8_t* start = q + 4;
        uint32_t id;
        std::memcpy(&id, start, 4);
        if (id == 0) {
            const uint8_t* c = start + 5;
            const char* aug = reinterpret_cast<const char*>(c);
            c += std::strlen(aug) + 1;
            uleb(c);
            sleb(c);
            uleb(c);
            uint8_t fenc = 0;
            if (aug[0] == 'z') {
                uleb(c);
                for (const char* a = aug + 1; *a; a++) {
                    if (*a == 'R') fenc = *c++;
                    else if (*a == 'P') { uint8_t pe = *c++; read_enc(c, pe); }
                    else if (*a == 'L') c++;
                }
            }
            cie_enc[q] = fenc;
        } else {
            const uint8_t* cie = start - id;
            auto it = cie_enc.find(cie);
            uint8_t fenc = it != cie_enc.end() ? it->second : 0x1b;
            const uint8_t* f = start + 4;
            uint64_t pc = read_enc(f, fenc);
            uint64_t range = read_enc(f, fenc & 0x0f);
            if (range) out.push_back({pc, pc + range});
        }
        q = start + len;
    }
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace raven::elf
