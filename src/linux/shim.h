#pragma once
// The shim table: Windows implementations of the Linux functions and variables the game and its Linux libraries
// import. Every function is SysV-ABI (docs/architecture.md#calling-convention) and registered under its Linux name.
//
//   SHIM(int, close, (int fd)) { ... }          defines lx_close and registers it as "close"
//   SHIM_VAR(stdout, &lx_stdout_var)            registers a data symbol
//   SHIM_ALIAS(__open_2, open)                  registers another name for an existing shim
//
// Linux `long` is 64-bit: use int64_t/uint64_t (lnx_long/lnx_ulong) for it.
#include <cstddef>
#include <cstdint>

#define SYSV __attribute__((sysv_abi))

using lnx_long = int64_t;
using lnx_ulong = uint64_t;

namespace raven::shim {
void register_symbol(const char* name, void* addr);
void* lookup(const char* name);
struct Reg {
    Reg(const char* n, void* p) { register_symbol(n, p); }
};
}  // namespace raven::shim

#define SHIM(ret, name, args)                                                            \
    extern "C" SYSV ret lx_##name args;                                                  \
    static ::raven::shim::Reg shim_reg_##name(#name, reinterpret_cast<void*>(&lx_##name)); \
    extern "C" SYSV ret lx_##name args

#define SHIM_VAR(name, addr) static ::raven::shim::Reg shim_var_##name(#name, (void*)(addr))
#define SHIM_ALIAS(alias, name) static ::raven::shim::Reg shim_alias_##alias(#alias, reinterpret_cast<void*>(&lx_##name))
// A shim that only logs that it was called and returns a fixed value (for functions the game can live without).
#define SHIM_STUB(ret, name, args, value)  \
    SHIM(ret, name, args) {                \
        RTRACE("stub %s", #name);           \
        return value;                       \
    }
