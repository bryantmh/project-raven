// libm. Plain double/float functions map straight to the CRT (clang converts the calling convention).
#include <cfenv>
#include <cmath>
#include <cstring>

#include "linux/lnx.h"

#define M1(fn) SHIM(double, fn, (double x)) { return ::fn(x); }
#define M1F(fn, base) SHIM(float, fn, (float x)) { return ::fn(x); }
#define M2(fn) SHIM(double, fn, (double x, double y)) { return ::fn(x, y); }
#define M2F(fn, base) SHIM(float, fn, (float x, float y)) { return ::fn(x, y); }

M1(sin) M1(cos) M1(tan) M1(asin) M1(acos) M1(atan) M1(exp) M1(log) M1(log10) M1(log2) M1(sqrt) M1(floor) M1(ceil)
M1(trunc) M1(round) M1(rint) M1(sinh) M1(cosh) M1(tanh) M1(exp2) M1(cbrt) M1(fabs) M1(nearbyint) M1(expm1) M1(log1p)
M2(atan2) M2(pow) M2(fmod) M2(hypot) M2(fmin) M2(fmax) M2(copysign) M2(remainder)
M1F(sinf, sin) M1F(cosf, cos) M1F(tanf, tan) M1F(asinf, asin) M1F(acosf, acos) M1F(atanf, atan) M1F(expf, exp)
M1F(logf, log) M1F(log10f, log10) M1F(log2f, log2) M1F(sqrtf, sqrt) M1F(floorf, floor) M1F(ceilf, ceil)
M1F(truncf, trunc) M1F(roundf, round) M1F(rintf, rint) M1F(fabsf, fabs) M1F(exp2f, exp2) M1F(tanhf, tanh)
M2F(atan2f, atan2) M2F(powf, pow) M2F(fmodf, fmod) M2F(hypotf, hypot) M2F(fminf, fmin) M2F(fmaxf, fmax)

SHIM(lnx_long, lround, (double x)) { return std::llround(x); }
SHIM(lnx_long, lrint, (double x)) { return std::llrint(x); }
SHIM(lnx_long, lroundf, (float x)) { return std::llround(x); }
SHIM(lnx_long, lrintf, (float x)) { return std::llrint(x); }
SHIM(double, ldexp, (double x, int e)) { return std::ldexp(x, e); }
SHIM(float, ldexpf, (float x, int e)) { return std::ldexp(x, e); }
SHIM(double, scalbn, (double x, int e)) { return std::scalbn(x, e); }
SHIM(double, frexp, (double x, int* e)) { return std::frexp(x, e); }
SHIM(double, modf, (double x, double* i)) { return std::modf(x, i); }
SHIM(float, modff, (float x, float* i)) { return std::modf(x, i); }
SHIM(void, sincos, (double x, double* s, double* c)) { *s = std::sin(x); *c = std::cos(x); }
SHIM(void, sincosf, (float x, float* s, float* c)) { *s = std::sin(x); *c = std::cos(x); }

// fenv rounding modes: glibc FE_* values are the x87/SSE RC bits (0, 0x400, 0x800, 0xc00).
SHIM(int, fegetround, ()) {
    switch (std::fegetround()) {
        case FE_DOWNWARD: return 0x400;
        case FE_UPWARD: return 0x800;
        case FE_TOWARDZERO: return 0xc00;
        default: return 0;
    }
}
SHIM(int, fesetround, (int m)) {
    int w = m == 0x400 ? FE_DOWNWARD : m == 0x800 ? FE_UPWARD : m == 0xc00 ? FE_TOWARDZERO : FE_TONEAREST;
    return std::fesetround(w);
}

// frexpl: long double arrives on the stack (16 bytes) and returns in st(0). x87 instructions do the work.
extern "C" SYSV long double lx_frexpl(long double x, int* e);
__asm__(".text\n.globl lx_frexpl\nlx_frexpl:\n"
        "fldt 8(%rsp)\n"         // x
        "ftst\nfnstsw %ax\nsahf\n"
        "je 2f\n"
        "fxtract\n"              // st0 = mantissa in [1,2), st1 = exponent
        "fld1\nfchs\nfxch\nfscale\nfstp %st(1)\n"  // mantissa / 2 -> [0.5,1)
        "fxch\n"
        "subq $8, %rsp\nfistpl (%rsp)\nmovl (%rsp), %eax\naddl $1, %eax\nmovl %eax, (%rdi)\naddq $8, %rsp\n"
        "retq\n"
        "2:\nmovl $0, (%rdi)\nretq\n");
static ::raven::shim::Reg reg_frexpl("frexpl", reinterpret_cast<void*>(&lx_frexpl));
