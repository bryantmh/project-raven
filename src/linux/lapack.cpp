// LAPACK for the game (gun calibration fits, physics solves) over OpenBLAS (runtime/windows/libopenblas.dll,
// fetched by tools/fetch_runtime.py). LAPACK's Fortran interface passes every argument by pointer, so each SysV
// wrapper just forwards its pointers; character arguments get Fortran's hidden length (1) appended, which the
// OpenBLAS C kernels ignore and its Fortran ones expect.
#include <windows.h>

#include <filesystem>

#include "app/paths.h"
#include "linux/shim.h"
#include "log.h"

namespace {
HMODULE blas() {
    static HMODULE h = [] {
        auto p = std::filesystem::path(raven::app::to_wide(raven::app::paths().runtime_dir)) / L"runtime" / L"windows" / L"libopenblas.dll";
        HMODULE m = LoadLibraryW(p.c_str());
        if (!m) raven::log_fatal("The game needs LAPACK: libopenblas.dll is missing from the unpacked runtime files.");
        return m;
    }();
    return h;
}
template <typename F>
F get(const char* name) {
    auto f = reinterpret_cast<F>(GetProcAddress(blas(), name));
    if (!f) raven::log_fatal("libopenblas.dll lacks %s", name);
    return f;
}
using P = void*;
}  // namespace

// N pointer arguments, then H hidden character lengths.
#define LAPACK(name, N, H)                                                                                   \
    SHIM(void, name, (P a0, P a1, P a2, P a3, P a4, P a5, P a6, P a7, P a8, P a9, P a10, P a11, P a12, P a13, \
                      P a14, P a15, P a16, P a17, P a18, P a19, P a20)) {                                    \
        using F = void(__stdcall*)(P, P, P, P, P, P, P, P, P, P, P, P, P, P, P, P, P, P, P, P, P, size_t,    \
                                   size_t, size_t);                                                          \
        static auto f = get<F>(#name);                                                                      \
        P a[21] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20}; \
        /* Hidden lengths go right after the N real arguments; the rest are unused stack slots. */          \
        P b[24] = {};                                                                                        \
        for (int i = 0; i < N; i++) b[i] = a[i];                                                             \
        for (int i = 0; i < H; i++) b[N + i] = reinterpret_cast<P>(size_t(1));                               \
        f(b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],     \
          b[15], b[16], b[17], b[18], b[19], b[20], size_t(b[21]), size_t(b[22]), size_t(b[23]));            \
    }

// The SysV caller only passes N arguments; the wrapper reads 21 (the extras are whatever is in the registers and
// on the stack), and only the first N + H reach the callee meaningfully.
LAPACK(sgesv_, 8, 0)
LAPACK(dgesv_, 8, 0)
LAPACK(sgetrf_, 6, 0)
LAPACK(dgetrf_, 6, 0)
LAPACK(sgetri_, 7, 0)
LAPACK(dgetri_, 7, 0)
LAPACK(spotrf_, 5, 1)
LAPACK(dpotrf_, 5, 1)
LAPACK(spotri_, 5, 1)
LAPACK(dpotri_, 5, 1)
LAPACK(spotrs_, 8, 1)
LAPACK(dpotrs_, 8, 1)
LAPACK(ssyevr_, 21, 3)
LAPACK(dsyevr_, 21, 3)
LAPACK(sgesdd_, 14, 1)
LAPACK(dgesdd_, 14, 1)
LAPACK(sgels_, 11, 1)
LAPACK(dgels_, 11, 1)
LAPACK(sgelsd_, 14, 0)
LAPACK(dgelsd_, 14, 0)
