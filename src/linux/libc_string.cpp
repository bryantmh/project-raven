// glibc string, memory, ctype, wide-character and number-parsing functions.
// Linux wchar_t is 32-bit, so every wcs* function here works on char32_t (Windows' wchar_t is 16-bit).
#include <mimalloc.h>

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>

#include "linux/lnx.h"
#include "linux/sysv_va.h"
#include "log.h"

using namespace raven::lnx;
using wchar32 = char32_t;

// ---- memory allocation (mimalloc: one allocator, so free() takes anything malloc/memalign/realloc returned)
SHIM(void*, malloc, (size_t n)) { return mi_malloc(n); }
SHIM(void*, calloc, (size_t a, size_t b)) { return mi_calloc(a, b); }
SHIM(void*, realloc, (void* p, size_t n)) { return mi_realloc(p, n); }
SHIM(void, free, (void* p)) { mi_free(p); }
SHIM(void*, memalign, (size_t al, size_t n)) { return mi_malloc_aligned(n, al); }
SHIM(void*, aligned_alloc, (size_t al, size_t n)) { return mi_malloc_aligned(n, al); }
SHIM(int, posix_memalign, (void** out, size_t al, size_t n)) {
    *out = mi_malloc_aligned(n, al);
    return *out ? 0 : L_ENOMEM;
}
SHIM(size_t, malloc_usable_size, (void* p)) { return mi_usable_size(p); }
SHIM(char*, strdup, (const char* s)) { return mi_strdup(s); }
SHIM(char*, strndup, (const char* s, size_t n)) { return mi_strndup(s, n); }

// ---- mem*/str* (same semantics as the CRT)
SHIM(void*, memcpy, (void* d, const void* s, size_t n)) { return std::memcpy(d, s, n); }
SHIM(void*, memmove, (void* d, const void* s, size_t n)) { return std::memmove(d, s, n); }
SHIM(void*, memset, (void* d, int c, size_t n)) { return std::memset(d, c, n); }
SHIM(int, memcmp, (const void* a, const void* b, size_t n)) { return std::memcmp(a, b, n); }
SHIM(void*, memchr, (const void* s, int c, size_t n)) { return const_cast<void*>(std::memchr(s, c, n)); }
SHIM(void*, __memcpy_chk, (void* d, const void* s, size_t n, size_t)) { return std::memcpy(d, s, n); }
SHIM(void*, __memmove_chk, (void* d, const void* s, size_t n, size_t)) { return std::memmove(d, s, n); }
SHIM(void*, __memset_chk, (void* d, int c, size_t n, size_t)) { return std::memset(d, c, n); }
SHIM(size_t, strlen, (const char* s)) { return std::strlen(s); }
SHIM(size_t, strnlen, (const char* s, size_t n)) { return ::strnlen(s, n); }
SHIM(int, strcmp, (const char* a, const char* b)) { return std::strcmp(a, b); }
SHIM(int, strncmp, (const char* a, const char* b, size_t n)) { return std::strncmp(a, b, n); }
SHIM(int, strcasecmp, (const char* a, const char* b)) { return _stricmp(a, b); }
SHIM(int, strncasecmp, (const char* a, const char* b, size_t n)) { return _strnicmp(a, b, n); }
SHIM(int, strcoll, (const char* a, const char* b)) { return std::strcmp(a, b); }
SHIM(char*, strcpy, (char* d, const char* s)) { return std::strcpy(d, s); }
SHIM(char*, strncpy, (char* d, const char* s, size_t n)) { return std::strncpy(d, s, n); }
SHIM(char*, strcat, (char* d, const char* s)) { return std::strcat(d, s); }
SHIM(char*, strncat, (char* d, const char* s, size_t n)) { return std::strncat(d, s, n); }
SHIM(char*, stpcpy, (char* d, const char* s)) {
    size_t n = std::strlen(s);
    std::memcpy(d, s, n + 1);
    return d + n;
}
SHIM(char*, __strcpy_chk, (char* d, const char* s, size_t)) { return std::strcpy(d, s); }
SHIM(char*, __strncpy_chk, (char* d, const char* s, size_t n, size_t)) { return std::strncpy(d, s, n); }
SHIM(char*, __strcat_chk, (char* d, const char* s, size_t)) { return std::strcat(d, s); }
SHIM(char*, __strncat_chk, (char* d, const char* s, size_t n, size_t)) { return std::strncat(d, s, n); }
SHIM(char*, __stpcpy_chk, (char* d, const char* s, size_t)) { return lx_stpcpy(d, s); }
SHIM(char*, strchr, (const char* s, int c)) { return const_cast<char*>(std::strchr(s, c)); }
SHIM(char*, strrchr, (const char* s, int c)) { return const_cast<char*>(std::strrchr(s, c)); }
SHIM(char*, strstr, (const char* a, const char* b)) { return const_cast<char*>(std::strstr(a, b)); }
SHIM(char*, strcasestr, (const char* h, const char* n)) {
    size_t ln = std::strlen(n);
    for (; *h; h++)
        if (!_strnicmp(h, n, ln)) return const_cast<char*>(h);
    return ln ? nullptr : const_cast<char*>(h);
}
SHIM(char*, strpbrk, (const char* s, const char* a)) { return const_cast<char*>(std::strpbrk(s, a)); }
SHIM(size_t, strspn, (const char* s, const char* a)) { return std::strspn(s, a); }
SHIM(size_t, strcspn, (const char* s, const char* a)) { return std::strcspn(s, a); }
SHIM(char*, strtok_r, (char* s, const char* d, char** save)) { return strtok_s(s, d, save); }
SHIM_ALIAS(__strtok_r, strtok_r);
static thread_local char* t_strtok;
SHIM(char*, strtok, (char* s, const char* d)) { return strtok_s(s, d, &t_strtok); }
SHIM(size_t, strxfrm, (char* d, const char* s, size_t n)) { return std::strxfrm(d, s, n); }

// ---- ctype: glibc's tables (bits from <ctype.h>: _ISbit puts bits 0-7 in the high byte)
namespace {
enum : uint16_t {
    ISupper = 0x100, ISlower = 0x200, ISalpha = 0x400, ISdigit = 0x800, ISxdigit = 0x1000, ISspace = 0x2000,
    ISprint = 0x4000, ISgraph = 0x8000, ISblank = 0x1, IScntrl = 0x2, ISpunct = 0x4, ISalnum = 0x8,
};
struct CtypeTables {
    uint16_t b[384];
    int32_t lower[384], upper[384];
    const uint16_t* bp = b + 128;
    const int32_t* lp = lower + 128;
    const int32_t* up = upper + 128;
    CtypeTables() {
        for (int i = -128; i < 256; i++) {
            int c = i & 0xff;
            uint16_t m = 0;
            if (i >= 0 && c < 128) {
                if (isupper(c)) m |= ISupper;
                if (islower(c)) m |= ISlower;
                if (isalpha(c)) m |= ISalpha;
                if (isdigit(c)) m |= ISdigit;
                if (isxdigit(c)) m |= ISxdigit;
                if (isspace(c)) m |= ISspace;
                if (isprint(c)) m |= ISprint;
                if (isgraph(c)) m |= ISgraph;
                if (c == ' ' || c == '\t') m |= ISblank;
                if (iscntrl(c)) m |= IScntrl;
                if (ispunct(c)) m |= ISpunct;
                if (isalnum(c)) m |= ISalnum;
            }
            b[i + 128] = m;
            lower[i + 128] = (i >= 'A' && i <= 'Z') ? i + 32 : i;
            upper[i + 128] = (i >= 'a' && i <= 'z') ? i - 32 : i;
        }
    }
};
CtypeTables& ctype_tables() {
    static CtypeTables t;
    return t;
}
}  // namespace
const uint16_t* raven_ctype_b() { return ctype_tables().bp; }
const int32_t* raven_ctype_tolower() { return ctype_tables().lp; }
const int32_t* raven_ctype_toupper() { return ctype_tables().up; }
SHIM(const uint16_t**, __ctype_b_loc, ()) { return &ctype_tables().bp; }
SHIM(const int32_t**, __ctype_tolower_loc, ()) { return &ctype_tables().lp; }
SHIM(const int32_t**, __ctype_toupper_loc, ()) { return &ctype_tables().up; }
SHIM(size_t, __ctype_get_mb_cur_max, ()) { return 6; }
SHIM(int, tolower, (int c)) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
SHIM(int, toupper, (int c)) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
SHIM(int, isspace, (int c)) { return c == ' ' || (c >= '\t' && c <= '\r'); }
SHIM(int, isalpha, (int c)) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
SHIM(int, isdigit, (int c)) { return c >= '0' && c <= '9'; }
SHIM(int, isalnum, (int c)) { return lx_isalpha(c) || lx_isdigit(c); }
SHIM(int, isupper, (int c)) { return c >= 'A' && c <= 'Z'; }
SHIM(int, islower, (int c)) { return c >= 'a' && c <= 'z'; }
SHIM(int, isxdigit, (int c)) { return std::isxdigit(c & 0x7f) && c < 128 && c >= 0; }
SHIM(int, isprint, (int c)) { return c >= 32 && c < 127; }
SHIM(int, ispunct, (int c)) { return c > 32 && c < 127 && !lx_isalnum(c); }

// ---- numbers
SHIM(int, atoi, (const char* s)) { return std::atoi(s); }
SHIM(lnx_long, atol, (const char* s)) { return std::atoll(s); }
SHIM(long long, atoll, (const char* s)) { return std::atoll(s); }
SHIM(double, atof, (const char* s)) { return std::atof(s); }
#define STRTO(name, type, crt)                                         \
    SHIM(type, name, (const char* s, char** e, int b)) {               \
        errno = 0;                                                     \
        type r = (type)crt(s, e, b);                                   \
        if (errno == ERANGE) err() = L_ERANGE;                         \
        return r;                                                      \
    }
STRTO(strtol, lnx_long, std::strtoll)
STRTO(strtoul, lnx_ulong, std::strtoull)
STRTO(strtoll, long long, std::strtoll)
STRTO(strtoull, unsigned long long, std::strtoull)
SHIM(double, strtod, (const char* s, char** e)) { return std::strtod(s, e); }
SHIM(float, strtof, (const char* s, char** e)) { return std::strtof(s, e); }
SHIM(double, __strtod_l, (const char* s, char** e, void*)) { return std::strtod(s, e); }
SHIM(float, __strtof_l, (const char* s, char** e, void*)) { return std::strtof(s, e); }
// long double is 80-bit on Linux; the game only passes these through libstdc++'s number parsing. Return the value
// in the low 8 bytes as an x87 conversion would: build the 80-bit pattern from the double.
static void to_x87(double v, void* out) {
    uint8_t* o = static_cast<uint8_t*>(out);
    std::memset(o, 0, 16);
    if (v == 0) { if (std::signbit(v)) o[9] = 0x80; return; }
    int e;
    double m = std::frexp(std::fabs(v), &e);  // m in [0.5,1)
    uint64_t mant = uint64_t(std::ldexp(m, 64));
    uint16_t se = uint16_t(e - 1 + 16383) | (v < 0 ? 0x8000 : 0);
    std::memcpy(o, &mant, 8);
    std::memcpy(o + 8, &se, 2);
}
// long double returns in st(0) under SysV: done with inline asm by loading the 80-bit value.
extern "C" SYSV long double lx_strtold(const char* s, char** e);
__asm__(".text\n.globl lx_strtold\nlx_strtold:\n"
        "subq $40, %rsp\n"
        "leaq 16(%rsp), %rdx\n"           // out buffer as 3rd arg of helper (sysv)
        "callq raven_strtold_helper\n"
        "fldt 16(%rsp)\n"
        "addq $40, %rsp\nretq\n");
extern "C" SYSV void raven_strtold_helper(const char* s, char** e, void* out) { to_x87(std::strtod(s, e), out); }
static ::raven::shim::Reg reg_strtold("strtold", reinterpret_cast<void*>(&lx_strtold));
static ::raven::shim::Reg reg_strtold_l("strtold_l", reinterpret_cast<void*>(&lx_strtold));
static ::raven::shim::Reg reg___strtold_l("__strtold_l", reinterpret_cast<void*>(&lx_strtold));

static unsigned g_rand_state = 1;
SHIM(int, rand, ()) {
    g_rand_state = g_rand_state * 1103515245 + 12345;
    return int((g_rand_state / 65536) % 32768) | int(((g_rand_state >> 3) & 0x7fff) << 15);
}
SHIM(void, srand, (unsigned s)) { g_rand_state = s; }
SHIM(int, rand_r, (unsigned* s)) {
    *s = *s * 1103515245 + 12345;
    return int((*s >> 1) & 0x7fffffff);
}
SHIM(lnx_long, random, ()) { return lx_rand() & 0x7fffffff; }
SHIM(void, srandom, (unsigned s)) { g_rand_state = s; }
SHIM(int, abs, (int v)) { return v < 0 ? -v : v; }
SHIM(lnx_long, labs, (lnx_long v)) { return v < 0 ? -v : v; }

struct SysvCompare {
    SYSV int (*fn)(const void*, const void*);
};
static thread_local SysvCompare t_cmp;
SHIM(void, qsort, (void* base, size_t n, size_t sz, SYSV int (*cmp)(const void*, const void*))) {
    SysvCompare saved = t_cmp;
    t_cmp.fn = cmp;
    std::qsort(base, n, sz, [](const void* a, const void* b) { return t_cmp.fn(a, b); });
    t_cmp = saved;
}
SHIM(void*, bsearch, (const void* key, const void* base, size_t n, size_t sz, SYSV int (*cmp)(const void*, const void*))) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const void* p = static_cast<const char*>(base) + mid * sz;
        int c = cmp(key, p);
        if (c == 0) return const_cast<void*>(p);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return nullptr;
}

// ---- wide strings (32-bit wchar_t)
SHIM(size_t, wcslen, (const wchar32* s)) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
SHIM(wchar32*, wcscpy, (wchar32* d, const wchar32* s)) {
    size_t i = 0;
    while ((d[i] = s[i])) i++;
    return d;
}
SHIM(wchar32*, wcsncpy, (wchar32* d, const wchar32* s, size_t n)) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
SHIM(wchar32*, wcscat, (wchar32* d, const wchar32* s)) {
    lx_wcscpy(d + lx_wcslen(d), s);
    return d;
}
SHIM(wchar32*, wcsncat, (wchar32* d, const wchar32* s, size_t n)) {
    wchar32* e = d + lx_wcslen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) e[i] = s[i];
    e[i] = 0;
    return d;
}
SHIM(int, wcscmp, (const wchar32* a, const wchar32* b)) {
    while (*a && *a == *b) a++, b++;
    return *a < *b ? -1 : *a > *b;
}
SHIM(int, wcsncmp, (const wchar32* a, const wchar32* b, size_t n)) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
    return 0;
}
SHIM(int, wcscasecmp, (const wchar32* a, const wchar32* b)) {
    for (;; a++, b++) {
        wchar32 x = *a < 0x10000 ? towlower(wchar_t(*a)) : *a, y = *b < 0x10000 ? towlower(wchar_t(*b)) : *b;
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
SHIM(int, wcscoll, (const wchar32* a, const wchar32* b)) { return lx_wcscmp(a, b); }
SHIM(int, __wcscoll_l, (const wchar32* a, const wchar32* b, void*)) { return lx_wcscmp(a, b); }
SHIM(size_t, __wcsxfrm_l, (wchar32* d, const wchar32* s, size_t n, void*)) {
    size_t l = lx_wcslen(s);
    if (l < n) lx_wcscpy(d, s);
    return l;
}
SHIM(wchar32*, wcschr, (const wchar32* s, wchar32 c)) {
    for (;; s++) {
        if (*s == c) return const_cast<wchar32*>(s);
        if (!*s) return nullptr;
    }
}
SHIM(wchar32*, wcsstr, (const wchar32* h, const wchar32* n)) {
    size_t ln = lx_wcslen(n);
    for (; *h; h++)
        if (!lx_wcsncmp(h, n, ln)) return const_cast<wchar32*>(h);
    return ln ? nullptr : const_cast<wchar32*>(h);
}
SHIM(wchar32*, wcstok, (wchar32* s, const wchar32* delim, wchar32** save)) {
    if (!s) s = *save;
    auto isd = [&](wchar32 c) { return lx_wcschr(delim, c) && c; };
    while (*s && isd(*s)) s++;
    if (!*s) { *save = s; return nullptr; }
    wchar32* tok = s;
    while (*s && !isd(*s)) s++;
    if (*s) *s++ = 0;
    *save = s;
    return tok;
}
SHIM(wchar32*, wmemcpy, (wchar32* d, const wchar32* s, size_t n)) { return static_cast<wchar32*>(std::memcpy(d, s, n * 4)); }
SHIM(wchar32*, wmemmove, (wchar32* d, const wchar32* s, size_t n)) { return static_cast<wchar32*>(std::memmove(d, s, n * 4)); }
SHIM(wchar32*, wmemset, (wchar32* d, wchar32 c, size_t n)) {
    for (size_t i = 0; i < n; i++) d[i] = c;
    return d;
}
SHIM(int, wmemcmp, (const wchar32* a, const wchar32* b, size_t n)) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
SHIM(wchar32*, wmemchr, (const wchar32* s, wchar32 c, size_t n)) {
    for (size_t i = 0; i < n; i++)
        if (s[i] == c) return const_cast<wchar32*>(s + i);
    return nullptr;
}
static std::string narrow32(const wchar32* s, wchar32** end_out = nullptr) {
    std::string o;
    const wchar32* p = s;
    while (*p && *p < 128) o += char(*p++);
    return o;
}
SHIM(lnx_long, wcstol, (const wchar32* s, wchar32** e, int b)) {
    std::string n = narrow32(s);
    char* ce;
    long long r = std::strtoll(n.c_str(), &ce, b);
    if (e) *e = const_cast<wchar32*>(s + (ce - n.c_str()));
    return r;
}
SHIM(lnx_ulong, wcstoul, (const wchar32* s, wchar32** e, int b)) {
    std::string n = narrow32(s);
    char* ce;
    unsigned long long r = std::strtoull(n.c_str(), &ce, b);
    if (e) *e = const_cast<wchar32*>(s + (ce - n.c_str()));
    return r;
}
SHIM_ALIAS(wcstoll, wcstol);
SHIM_ALIAS(wcstoull, wcstoul);
SHIM(double, wcstod, (const wchar32* s, wchar32** e)) {
    std::string n = narrow32(s);
    char* ce;
    double r = std::strtod(n.c_str(), &ce);
    if (e) *e = const_cast<wchar32*>(s + (ce - n.c_str()));
    return r;
}
SHIM(float, wcstof, (const wchar32* s, wchar32** e)) { return float(lx_wcstod(s, e)); }

// UTF-8 <-> UTF-32 multibyte conversions (the game's locale is UTF-8 or C)
struct lmbstate { int count; uint32_t value; };
SHIM(size_t, mbrtowc, (wchar32* pwc, const char* s, size_t n, lmbstate* st)) {
    if (!s) return 0;
    if (!n) return size_t(-2);
    auto u = reinterpret_cast<const unsigned char*>(s);
    uint32_t c = u[0];
    size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (len > n) return size_t(-2);
    if (len > 1) {
        c &= (0x7F >> len);
        for (size_t i = 1; i < len; i++) c = (c << 6) | (u[i] & 0x3F);
    }
    if (pwc) *pwc = c;
    return c ? len : 0;
}
SHIM(size_t, wcrtomb, (char* s, wchar32 c, lmbstate*)) {
    if (!s) return 1;
    if (c < 0x80) { s[0] = char(c); return 1; }
    if (c < 0x800) { s[0] = char(0xC0 | (c >> 6)); s[1] = char(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { s[0] = char(0xE0 | (c >> 12)); s[1] = char(0x80 | ((c >> 6) & 0x3F)); s[2] = char(0x80 | (c & 0x3F)); return 3; }
    s[0] = char(0xF0 | (c >> 18)); s[1] = char(0x80 | ((c >> 12) & 0x3F)); s[2] = char(0x80 | ((c >> 6) & 0x3F)); s[3] = char(0x80 | (c & 0x3F));
    return 4;
}
SHIM(int, mbtowc, (wchar32* pwc, const char* s, size_t n)) {
    if (!s) return 0;
    size_t r = lx_mbrtowc(pwc, s, n, nullptr);
    return r > 4 ? -1 : int(r);
}
SHIM(int, wctomb, (char* s, wchar32 c)) { return s ? int(lx_wcrtomb(s, c, nullptr)) : 0; }
SHIM(size_t, mbsrtowcs, (wchar32* d, const char** src, size_t n, lmbstate* st)) {
    const char* s = *src;
    size_t i = 0;
    while (!d || i < n) {
        wchar32 c;
        size_t k = lx_mbrtowc(&c, s, 4, st);
        if (k > 4) return size_t(-1);
        if (d) d[i] = c;
        if (!c) { if (d) *src = nullptr; return i; }
        s += k;
        i++;
    }
    *src = s;
    return i;
}
SHIM(size_t, mbsnrtowcs, (wchar32* d, const char** src, size_t nms, size_t n, lmbstate* st)) {
    const char* s = *src;
    const char* end = s + nms;
    size_t i = 0;
    while ((!d || i < n) && s < end) {
        wchar32 c;
        size_t k = lx_mbrtowc(&c, s, size_t(end - s), st);
        if (k == size_t(-2)) break;
        if (k > 4) return size_t(-1);
        if (d) d[i] = c;
        if (!c) { if (d) *src = nullptr; return i; }
        s += k;
        i++;
    }
    if (d) *src = s;
    return i;
}
SHIM(size_t, mbstowcs, (wchar32* d, const char* s, size_t n)) { return lx_mbsrtowcs(d, &s, n, nullptr); }
SHIM(size_t, wcsnrtombs, (char* d, const wchar32** src, size_t nwc, size_t n, lmbstate*)) {
    const wchar32* s = *src;
    size_t o = 0;
    for (size_t i = 0; i < nwc; i++) {
        char buf[4];
        size_t k = lx_wcrtomb(buf, s[i], nullptr);
        if (d) {
            if (o + k > n) { *src = s + i; return o; }
            std::memcpy(d + o, buf, k);
        }
        if (!s[i]) { if (d) *src = nullptr; return o; }
        o += k;
    }
    if (d) *src = s + nwc;
    return o;
}
SHIM(size_t, wcstombs, (char* d, const wchar32* s, size_t n)) {
    size_t o = 0;
    for (; *s; s++) {
        char buf[4];
        size_t k = lx_wcrtomb(buf, *s, nullptr);
        if (d) {
            if (o + k > n) break;
            std::memcpy(d + o, buf, k);
        }
        o += k;
    }
    if (d && o < n) d[o] = 0;
    return o;
}
SHIM(uint32_t, btowc, (int c)) { return c >= 0 && c < 128 ? uint32_t(c) : 0xFFFFFFFFu; }
SHIM(int, wctob, (uint32_t c)) { return c < 128 ? int(c) : -1; }
SHIM(int, iswalpha, (uint32_t c)) { return c < 0x10000 && iswalpha(wint_t(c)); }
SHIM(int, iswdigit, (uint32_t c)) { return c >= '0' && c <= '9'; }
SHIM(int, iswspace, (uint32_t c)) { return c < 0x10000 && iswspace(wint_t(c)); }
SHIM(uint32_t, towlower, (uint32_t c)) { return c < 0x10000 ? towlower(wint_t(c)) : c; }
SHIM(uint32_t, towupper, (uint32_t c)) { return c < 0x10000 ? towupper(wint_t(c)) : c; }
SHIM(uint32_t, __towlower_l, (uint32_t c, void*)) { return lx_towlower(c); }
SHIM(uint32_t, __towupper_l, (uint32_t c, void*)) { return lx_towupper(c); }
// wctype_t handles: glibc returns a pointer to a class table; we return the glibc bit mask as the handle.
SHIM(lnx_ulong, __wctype_l, (const char* name, void*)) {
    static const struct { const char* n; uint16_t m; } k[] = {
        {"upper", ISupper}, {"lower", ISlower}, {"alpha", ISalpha}, {"digit", ISdigit}, {"xdigit", ISxdigit},
        {"space", ISspace}, {"print", ISprint}, {"graph", ISgraph}, {"blank", ISblank}, {"cntrl", IScntrl},
        {"punct", ISpunct}, {"alnum", ISalnum}};
    for (auto& e : k)
        if (!std::strcmp(e.n, name)) return e.m;
    return 0;
}
SHIM(int, __iswctype_l, (uint32_t c, lnx_ulong desc, void*)) {
    if (c < 128) return (ctype_tables().bp[c] & desc) != 0;
    if (c >= 0x10000) return 0;
    wint_t w = wint_t(c);
    uint16_t m = 0;
    if (iswupper(w)) m |= ISupper;
    if (iswlower(w)) m |= ISlower;
    if (iswalpha(w)) m |= ISalpha | ISalnum;
    if (iswspace(w)) m |= ISspace;
    if (iswprint(w)) m |= ISprint;
    if (iswgraph(w)) m |= ISgraph;
    if (iswpunct(w)) m |= ISpunct;
    return (m & desc) != 0;
}

SHIM(void, __stack_chk_fail, ()) { raven::log_fatal("Stack corruption detected in the game (__stack_chk_fail)."); }
SHIM(void, __assert_fail, (const char* expr, const char* file, unsigned line, const char* fn)) {
    raven::log_fatal("Game assertion failed: %s (%s:%u, %s)", expr, file, line, fn);
}
SHIM(int, __fpclassify, (double v)) {
    switch (std::fpclassify(v)) {
        case FP_NAN: return 0;
        case FP_INFINITE: return 1;
        case FP_ZERO: return 2;
        case FP_SUBNORMAL: return 3;
        default: return 4;
    }
}
SHIM(int, __isinf, (double v)) { return std::isinf(v) ? (v < 0 ? -1 : 1) : 0; }
SHIM(int, __isnan, (double v)) { return std::isnan(v); }
SHIM(wchar32*, __wcscpy_chk, (wchar32* d, const wchar32* s, size_t)) { return lx_wcscpy(d, s); }
SHIM(wchar32*, __wcscat_chk, (wchar32* d, const wchar32* s, size_t)) { return lx_wcscat(d, s); }
SHIM(wchar32*, __wcsncat_chk, (wchar32* d, const wchar32* s, size_t n, size_t)) { return lx_wcsncat(d, s, n); }
SHIM(wchar32*, __wcsncpy_chk, (wchar32* d, const wchar32* s, size_t n, size_t)) { return lx_wcsncpy(d, s, n); }
