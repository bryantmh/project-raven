#pragma once
// SysV variadic arguments on Windows. See docs/architecture.md#calling-convention.
//
// clang has no SysV va_list when targeting Windows, so variadic shims are entered through a naked stub
// (SYSV_VARIADIC) that spills the six integer and eight vector argument registers into a register save area, builds
// a sysv_va_list exactly like gcc's va_start would, and calls the implementation with a pointer to it after the
// fixed arguments. The game's own va_lists (vsnprintf and friends) have the same layout, so both paths read
// arguments with sysv_va_gp / sysv_va_fp.
#include <cstdint>
#include <cstring>

#include "linux/shim.h"

struct sysv_va_list {
    uint32_t gp_offset;          // 0..48 into reg_save_area
    uint32_t fp_offset;          // 48..176 into reg_save_area
    uint8_t* overflow_arg_area;  // stack arguments
    uint8_t* reg_save_area;
};

inline uint64_t sysv_va_gp(sysv_va_list* ap) {
    uint64_t v;
    if (ap->gp_offset < 48) {
        std::memcpy(&v, ap->reg_save_area + ap->gp_offset, 8);
        ap->gp_offset += 8;
    } else {
        std::memcpy(&v, ap->overflow_arg_area, 8);
        ap->overflow_arg_area += 8;
    }
    return v;
}

inline double sysv_va_fp(sysv_va_list* ap) {
    double v;
    if (ap->fp_offset < 176) {
        std::memcpy(&v, ap->reg_save_area + ap->fp_offset, 8);
        ap->fp_offset += 16;
    } else {
        std::memcpy(&v, ap->overflow_arg_area, 8);
        ap->overflow_arg_area += 8;
    }
    return v;
}

// long double (x87 80-bit) always travels on the stack, 16-byte aligned.
double sysv_va_long_double(sysv_va_list* ap);

// Register holding the sysv_va_list* for an implementation with N fixed integer arguments.
#define SYSV_VA_REG_1 "%rsi"
#define SYSV_VA_REG_2 "%rdx"
#define SYSV_VA_REG_3 "%rcx"
#define SYSV_VA_REG_4 "%r8"
#define SYSV_VA_REG_5 "%r9"

// Defines lx_<name> (registered as <name>) that forwards to `impl(fixed..., sysv_va_list*)`, a SYSV extern "C"
// function. nfixed is the number of fixed integer/pointer arguments.
#define SYSV_VARIADIC(name, nfixed, impl)                                                            \
    extern "C" void lx_##name();                                                                     \
    __asm__(".text\n.globl lx_" #name "\n.p2align 4\nlx_" #name ":\n"                                \
            "subq $0xd8, %rsp\n"                                                                     \
            "movq %rdi, 0x00(%rsp)\nmovq %rsi, 0x08(%rsp)\nmovq %rdx, 0x10(%rsp)\n"                  \
            "movq %rcx, 0x18(%rsp)\nmovq %r8, 0x20(%rsp)\nmovq %r9, 0x28(%rsp)\n"                    \
            "testb %al, %al\nje 1f\n"                                                                \
            "movaps %xmm0, 0x30(%rsp)\nmovaps %xmm1, 0x40(%rsp)\nmovaps %xmm2, 0x50(%rsp)\n"         \
            "movaps %xmm3, 0x60(%rsp)\nmovaps %xmm4, 0x70(%rsp)\nmovaps %xmm5, 0x80(%rsp)\n"         \
            "movaps %xmm6, 0x90(%rsp)\nmovaps %xmm7, 0xa0(%rsp)\n"                                   \
            "1:\n"                                                                                   \
            "movl $" #nfixed "*8, 0xb0(%rsp)\n"                                                      \
            "movl $48, 0xb4(%rsp)\n"                                                                 \
            "leaq 0xe0(%rsp), %rax\nmovq %rax, 0xb8(%rsp)\n"                                         \
            "movq %rsp, 0xc0(%rsp)\n"                                                                \
            "leaq 0xb0(%rsp), " SYSV_VA_REG_##nfixed "\n"                                            \
            "callq " #impl "\n"                                                                      \
            "addq $0xd8, %rsp\n"                                                                     \
            "retq\n");                                                                               \
    static ::raven::shim::Reg shim_reg_##name(#name, reinterpret_cast<void*>(&lx_##name))

namespace raven::lnx {
// printf-style formatting of a SysV argument list into a growable buffer. Linux semantics: `long` is 64-bit,
// %ls / %lc take 32-bit wchar_t.
struct FormatSink {
    virtual void put(const char* s, size_t n) = 0;
};
int format(FormatSink& out, const char* fmt, sysv_va_list* ap);
// Wide-format variant (fwprintf / swprintf): the format and %s/%ls arguments are 32-bit wchar_t; output is
// UTF-8 into the sink.
int wformat(FormatSink& out, const char32_t* fmt, sysv_va_list* ap);
// sscanf with SysV pointer arguments.
int scan(const char* input, const char* fmt, sysv_va_list* ap, size_t* consumed = nullptr);
}  // namespace raven::lnx
