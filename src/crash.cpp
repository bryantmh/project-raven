// Crash reporting: a vectored handler that logs where a fatal exception happened in terms of the loaded ELF images
// (game+offset, libstdc+++offset), the registers and a frame-pointer backtrace, then exits.
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>

#include "elf/elf_loader.h"
#include "log.h"

namespace raven {
namespace {

void describe(uint64_t a, char* out, size_t n) {
    if (auto im = elf::find_image(a)) {
        std::snprintf(out, n, "%#llx (%s+%#llx)", (unsigned long long)a, im->name.c_str(),
                      (unsigned long long)(a - (im->executable ? 0 : im->lo)));
        return;
    }
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(a), &mod)) {
        char name[MAX_PATH];
        GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\');
        char sym[256] = "";
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        auto si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), a, &disp, si)) std::snprintf(sym, sizeof sym, " %s+%#llx", si->Name, disp);
        std::snprintf(out, n, "%#llx (%s+%#llx%s)", (unsigned long long)a, base ? base + 1 : name,
                      (unsigned long long)(a - uint64_t(mod)), sym);
        return;
    }
    std::snprintf(out, n, "%#llx", (unsigned long long)a);
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_BREAKPOINT && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    static LONG once = 0;
    if (InterlockedExchange(&once, 1)) { Sleep(INFINITE); }
    static bool sym_init = SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    (void)sym_init;
    CONTEXT* c = ep->ContextRecord;
    char where[512];
    describe(c->Rip, where, sizeof where);
    log("CRASH: exception %#lx at %s on thread %lu", code, where, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION)
        log("  %s address %#llx", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    log("  rax %016llx rbx %016llx rcx %016llx rdx %016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    log("  rsi %016llx rdi %016llx rbp %016llx rsp %016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    log("  r8  %016llx r9  %016llx r10 %016llx r11 %016llx", c->R8, c->R9, c->R10, c->R11);
    log("  r12 %016llx r13 %016llx r14 %016llx r15 %016llx", c->R12, c->R13, c->R14, c->R15);
    // Return addresses found on the stack that point into code (works without frame pointers).
    auto sp = reinterpret_cast<const uint64_t*>(c->Rsp);
    int shown = 0;
    for (int i = 0; i < 2048 && shown < 24; i++) {
        uint64_t v;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, 8, nullptr)) break;
        auto im = elf::find_image(v);
        bool code_ptr = false;
        if (im) code_ptr = true;
        else {
            HMODULE mod;
            code_ptr = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                          reinterpret_cast<LPCWSTR>(v), &mod) != 0;
        }
        if (!code_ptr) continue;
        describe(v, where, sizeof where);
        log("  stack[%#x] %s", i * 8, where);
        shown++;
    }
    log_fatal("Project Raven crashed (exception %#lx). Details are in raven.log.", code);
}
}  // namespace

void install_crash_handler() { AddVectoredExceptionHandler(1, on_exception); }

// Log where a thread is right now (RIP and return addresses on its stack). Used by the RAVEN_SAMPLE watchdog.
void sample_thread(HANDLE t, const char* label) {
    if (SuspendThread(t) == DWORD(-1)) return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    GetThreadContext(t, &c);
    char where[512];
    describe(c.Rip, where, sizeof where);
    log("SAMPLE %s: rip %s", label, where);
    auto sp = reinterpret_cast<const uint64_t*>(c.Rsp);
    int shown = 0;
    for (int i = 0; i < 4096 && shown < 12; i++) {
        uint64_t v;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, 8, nullptr)) break;
        if (auto im = elf::find_image(v); im && im->executable && v < 0x1600000) {
            describe(v, where, sizeof where);
            log("  stack[%#x] %s", i * 8, where);
            shown++;
        }
    }
    ResumeThread(t);
}
}  // namespace raven
