// getcontext / setcontext / swapcontext / makecontext with glibc's x86-64 ucontext_t layout. The game's process
// ("proc") system runs its tasks as ucontext coroutines (ProcInit 0x5a9c90).
//
// Layout (glibc sysdeps/unix/sysv/linux/x86_64): uc_link +8, uc_stack {ss_sp +16, ss_size +32}, gregs +40
// (REG_R8..REG_CR2, 8 bytes each), fpregs pointer +224, __reserved1[8] +232, uc_sigmask +296, __fpregs_mem +424
// (mxcsr at +448).
//
// Windows checks some stack pointers against the TEB's StackBase/StackLimit, so like Windows fibers, every switch
// also switches those. Each context keeps its stack bounds in mcontext.__reserved1 (+232 base, +240 limit), which
// glibc leaves unused: getcontext/swapcontext store the current TEB bounds, makecontext the coroutine stack's.
#include <cstdint>

#include "linux/shim.h"
#include "linux/sysv_va.h"
#include "log.h"

// Offsets
#define oR8 "40"
#define oR9 "48"
#define oR12 "72"
#define oR13 "80"
#define oR14 "88"
#define oR15 "96"
#define oRDI "104"
#define oRSI "112"
#define oRBP "120"
#define oRBX "128"
#define oRDX "136"
#define oRCX "152"
#define oRSP "160"
#define oRIP "168"
#define oFPREGS "224"
#define oSTKBASE "232"
#define oSTKLIMIT "240"
#define oFPMEM "424"
#define oMXCSR "448"

extern "C" SYSV int lx_getcontext(void* ucp);
extern "C" SYSV int lx_setcontext(const void* ucp);
extern "C" SYSV int lx_swapcontext(void* oucp, const void* ucp);
extern "C" void raven_start_context();

// Save the caller's state into the context in %rdi (shared by getcontext and swapcontext).
#define SAVE_CONTEXT                                                                                          \
    "movq %rbx, " oRBX "(%rdi)\nmovq %rbp, " oRBP "(%rdi)\nmovq %r12, " oR12 "(%rdi)\n"                      \
    "movq %r13, " oR13 "(%rdi)\nmovq %r14, " oR14 "(%rdi)\nmovq %r15, " oR15 "(%rdi)\n"                      \
    "movq %rdi, " oRDI "(%rdi)\nmovq %rsi, " oRSI "(%rdi)\nmovq %rdx, " oRDX "(%rdi)\n"                      \
    "movq %rcx, " oRCX "(%rdi)\nmovq %r8, " oR8 "(%rdi)\nmovq %r9, " oR9 "(%rdi)\n"                          \
    "movq (%rsp), %rcx\nmovq %rcx, " oRIP "(%rdi)\n"                                                          \
    "leaq 8(%rsp), %rcx\nmovq %rcx, " oRSP "(%rdi)\n"                                                         \
    "leaq " oFPMEM "(%rdi), %rcx\nmovq %rcx, " oFPREGS "(%rdi)\n"                                             \
    "fnstenv (%rcx)\nfldenv (%rcx)\nstmxcsr " oMXCSR "(%rdi)\n"                                               \
    "movq %gs:0x8, %rcx\nmovq %rcx, " oSTKBASE "(%rdi)\n"                                                     \
    "movq %gs:0x10, %rcx\nmovq %rcx, " oSTKLIMIT "(%rdi)\n"

// Load the context in %rdx and jump into it.
#define LOAD_CONTEXT                                                                                          \
    "movq " oFPREGS "(%rdx), %rcx\nfldenv (%rcx)\nldmxcsr " oMXCSR "(%rdx)\n"                                 \
    "movq " oSTKBASE "(%rdx), %rcx\ntestq %rcx, %rcx\njz 2f\nmovq %rcx, %gs:0x8\n"                            \
    "movq " oSTKLIMIT "(%rdx), %rcx\nmovq %rcx, %gs:0x10\n2:\n"                                               \
    "movq " oRSP "(%rdx), %rsp\nmovq " oRBX "(%rdx), %rbx\nmovq " oRBP "(%rdx), %rbp\n"                       \
    "movq " oR12 "(%rdx), %r12\nmovq " oR13 "(%rdx), %r13\nmovq " oR14 "(%rdx), %r14\nmovq " oR15 "(%rdx), %r15\n" \
    "movq " oRIP "(%rdx), %rcx\npushq %rcx\n"                                                                 \
    "movq " oRDI "(%rdx), %rdi\nmovq " oRSI "(%rdx), %rsi\nmovq " oRCX "(%rdx), %rcx\n"                       \
    "movq " oR8 "(%rdx), %r8\nmovq " oR9 "(%rdx), %r9\nmovq " oRDX "(%rdx), %rdx\n"                           \
    "xorl %eax, %eax\nretq\n"

__asm__(".text\n"
        ".globl lx_getcontext\n.p2align 4\nlx_getcontext:\n" SAVE_CONTEXT "xorl %eax, %eax\nretq\n"
        ".globl lx_setcontext\n.p2align 4\nlx_setcontext:\nmovq %rdi, %rdx\n" LOAD_CONTEXT
        ".globl lx_swapcontext\n.p2align 4\nlx_swapcontext:\n" SAVE_CONTEXT
        // swapcontext returns 0 when resumed: the saved RIP/RSP already point back at our caller
        "movq %rsi, %rdx\n" LOAD_CONTEXT
        // Returned from a makecontext function: %rbx points at uc_link on the coroutine stack.
        ".globl raven_start_context\n.p2align 4\nraven_start_context:\n"
        "movq %rbx, %rsp\npopq %rdi\ntestq %rdi, %rdi\njz 3f\ncallq lx_setcontext\n"
        "3:\nxorl %edi, %edi\ncallq raven_context_exit\nud2\n");

extern "C" SYSV void raven_context_exit(int) { raven::log_fatal("A game coroutine returned with no uc_link."); }

extern "C" SYSV void raven_makecontext(uint8_t* ucp, void (*func)(), int argc, sysv_va_list* ap) {
    auto u64 = [&](int off) -> uint64_t& { return *reinterpret_cast<uint64_t*>(ucp + off); };
    uint64_t ss_sp = u64(16), ss_size = u64(32), uc_link = u64(8);
    auto sp = reinterpret_cast<uint64_t*>(ss_sp + ss_size);
    int extra = argc > 6 ? argc - 6 : 0;
    sp -= 1 + extra;
    sp = reinterpret_cast<uint64_t*>((reinterpret_cast<uintptr_t>(sp) & ~uintptr_t(15)) - 8);
    int idx_link = extra + 1;
    u64(168) = reinterpret_cast<uint64_t>(func);            // RIP
    u64(128) = reinterpret_cast<uint64_t>(&sp[idx_link]);   // RBX -> uc_link slot
    u64(160) = reinterpret_cast<uint64_t>(sp);              // RSP
    sp[0] = reinterpret_cast<uint64_t>(&raven_start_context);
    sp[idx_link] = uc_link;
    static const int regs[6] = {104, 112, 136, 152, 40, 48};  // RDI RSI RDX RCX R8 R9
    for (int i = 0; i < argc; i++) {
        uint64_t v = uint32_t(sysv_va_gp(ap));
        if (i < 6) u64(regs[i]) = v;
        else sp[i - 5] = v;
    }
    u64(232) = ss_sp + ss_size;  // TEB StackBase / StackLimit for this coroutine
    u64(240) = ss_sp;
}
SYSV_VARIADIC(makecontext, 3, raven_makecontext);
static ::raven::shim::Reg reg_getcontext("getcontext", reinterpret_cast<void*>(&lx_getcontext));
static ::raven::shim::Reg reg_setcontext("setcontext", reinterpret_cast<void*>(&lx_setcontext));
static ::raven::shim::Reg reg_swapcontext("swapcontext", reinterpret_cast<void*>(&lx_swapcontext));
