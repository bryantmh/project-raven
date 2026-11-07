#pragma once
// Redirect a game function to one of ours: a 14-byte absolute jump written over its first instructions.
// The replacement must be SYSV (the game calls it with the System V convention).
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace raven::game {
inline void hook_jump(uint64_t at, const void* to) {
    auto p = reinterpret_cast<uint8_t*>(at);
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp qword [rip+0]
    std::memcpy(p, jmp, 6);
    uint64_t t = reinterpret_cast<uint64_t>(to);
    std::memcpy(p + 6, &t, 8);
}

// Copy the first `n` bytes of a game function (whole instructions with no rip-relative operands, checked by hand)
// to executable memory, followed by a jump back to at + n: calling the result runs the original function. Call
// before hook_jump(at, ...).
inline void* make_trampoline(uint64_t at, size_t n) {
    auto t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    std::memcpy(t, reinterpret_cast<const void*>(at), n);
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    std::memcpy(t + n, jmp, 6);
    uint64_t back = at + n;
    std::memcpy(t + n + 6, &back, 8);
    return t;
}
}  // namespace raven::game
