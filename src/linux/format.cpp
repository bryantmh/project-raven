// printf / scanf over SysV argument lists. See src/linux/sysv_va.h.
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "linux/sysv_va.h"

double sysv_va_long_double(sysv_va_list* ap) {
    uintptr_t p = (reinterpret_cast<uintptr_t>(ap->overflow_arg_area) + 15) & ~uintptr_t(15);
    uint8_t raw[10];
    std::memcpy(raw, reinterpret_cast<void*>(p), 10);
    ap->overflow_arg_area = reinterpret_cast<uint8_t*>(p + 16);
    // x87 extended: 64-bit mantissa with explicit integer bit, 15-bit exponent, sign.
    uint64_t mant;
    std::memcpy(&mant, raw, 8);
    uint16_t se;
    std::memcpy(&se, raw + 8, 2);
    int exp = se & 0x7fff;
    bool neg = se & 0x8000;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else v = std::ldexp(static_cast<double>(mant), exp - 16383 - 63);
    return neg ? -v : v;
}

namespace raven::lnx {
namespace {

void utf8_put(FormatSink& out, char32_t c) {
    char b[4];
    size_t n;
    if (c < 0x80) { b[0] = char(c); n = 1; }
    else if (c < 0x800) { b[0] = char(0xC0 | (c >> 6)); b[1] = char(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) { b[0] = char(0xE0 | (c >> 12)); b[1] = char(0x80 | ((c >> 6) & 0x3F)); b[2] = char(0x80 | (c & 0x3F)); n = 3; }
    else { b[0] = char(0xF0 | (c >> 18)); b[1] = char(0x80 | ((c >> 12) & 0x3F)); b[2] = char(0x80 | ((c >> 6) & 0x3F)); b[3] = char(0x80 | (c & 0x3F)); n = 4; }
    out.put(b, n);
}

struct Spec {
    std::string flags;
    int width = -1, prec = -1;
    int len = 0;  // 0 int, 1 h, 2 hh, 3 l, 4 ll/q/j/z/t, 5 L
    char conv = 0;
};

void pad_put(FormatSink& out, const Spec& s, const char* body, size_t n) {
    bool left = s.flags.find('-') != std::string::npos;
    size_t w = s.width > 0 ? size_t(s.width) : 0;
    if (!left)
        for (size_t i = n; i < w; i++) out.put(" ", 1);
    out.put(body, n);
    if (left)
        for (size_t i = n; i < w; i++) out.put(" ", 1);
}

// One conversion. `wide_fmt` means the format string was wide (so %s means wchar_t* and %c a wchar_t).
int convert(FormatSink& out, Spec& s, sysv_va_list* ap, bool wide_fmt, int written) {
    char spec[64], buf[512];
    auto mkspec = [&](const char* lenmod) {
        std::snprintf(spec, sizeof spec, "%%%s%s%s%s%s%c", s.flags.c_str(), s.width >= 0 ? std::to_string(s.width).c_str() : "",
                      s.prec >= 0 ? "." : "", s.prec >= 0 ? std::to_string(s.prec).c_str() : "", lenmod, s.conv);
    };
    switch (s.conv) {
        case 'd': case 'i': {
            int64_t v = int64_t(sysv_va_gp(ap));
            if (s.len == 0) v = int32_t(v);
            else if (s.len == 1) v = int16_t(v);
            else if (s.len == 2) v = int8_t(v);
            mkspec("ll");
            int n = std::snprintf(buf, sizeof buf, spec, (long long)v);
            out.put(buf, n);
            return n;
        }
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v = sysv_va_gp(ap);
            if (s.len == 0) v = uint32_t(v);
            else if (s.len == 1) v = uint16_t(v);
            else if (s.len == 2) v = uint8_t(v);
            mkspec("ll");
            int n = std::snprintf(buf, sizeof buf, spec, (unsigned long long)v);
            out.put(buf, n);
            return n;
        }
        case 'p': {
            uint64_t v = sysv_va_gp(ap);
            int n = v ? std::snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)v) : std::snprintf(buf, sizeof buf, "(nil)");
            pad_put(out, s, buf, n);
            return std::max(n, s.width);
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = s.len == 5 ? sysv_va_long_double(ap) : sysv_va_fp(ap);
            mkspec("");
            int n = std::snprintf(buf, sizeof buf, spec, v);
            if (n >= (int)sizeof buf) {
                std::string big(n + 1, '\0');
                std::snprintf(big.data(), big.size(), spec, v);
                out.put(big.data(), n);
            } else {
                out.put(buf, n);
            }
            return n;
        }
        case 'c': {
            uint32_t c = uint32_t(sysv_va_gp(ap));
            if (s.len == 3 || wide_fmt) {
                char tmp[8];
                struct Tmp : FormatSink {
                    char* p; size_t n = 0;
                    void put(const char* s, size_t k) override { std::memcpy(p + n, s, k); n += k; }
                } t;
                t.p = tmp;
                utf8_put(t, c);
                pad_put(out, s, tmp, t.n);
                return std::max<int>((int)t.n, s.width);
            }
            char ch = char(c);
            pad_put(out, s, &ch, 1);
            return std::max(1, s.width);
        }
        case 's': {
            uint64_t p = sysv_va_gp(ap);
            if (s.len == 3 || wide_fmt) {
                auto w = reinterpret_cast<const char32_t*>(p);
                std::string u8;
                struct S8 : FormatSink {
                    std::string* d;
                    void put(const char* s, size_t k) override { d->append(s, k); }
                } t;
                t.d = &u8;
                if (!w) u8 = "(null)";
                else
                    for (int i = 0; w[i] && (s.prec < 0 || i < s.prec); i++) utf8_put(t, w[i]);
                pad_put(out, s, u8.data(), u8.size());
                return std::max<int>((int)u8.size(), s.width);
            }
            const char* str = p ? reinterpret_cast<const char*>(p) : "(null)";
            size_t n = s.prec >= 0 ? strnlen(str, size_t(s.prec)) : std::strlen(str);
            pad_put(out, s, str, n);
            return std::max<int>((int)n, s.width);
        }
        case 'n': {
            uint64_t p = sysv_va_gp(ap);
            if (!p) return 0;
            if (s.len == 4 || s.len == 3) *reinterpret_cast<int64_t*>(p) = written;
            else if (s.len == 1) *reinterpret_cast<int16_t*>(p) = int16_t(written);
            else if (s.len == 2) *reinterpret_cast<int8_t*>(p) = int8_t(written);
            else *reinterpret_cast<int32_t*>(p) = written;
            return 0;
        }
        case 'm': {
            const char* e = std::strerror(errno);
            out.put(e, std::strlen(e));
            return (int)std::strlen(e);
        }
        default:
            return 0;
    }
}

template <typename Ch>
int format_impl(FormatSink& out, const Ch* fmt, sysv_va_list* ap, bool wide) {
    int total = 0;
    for (const Ch* f = fmt; *f;) {
        if (*f != '%') {
            if (wide) {
                struct Count : FormatSink {
                    FormatSink* o; int n = 0;
                    void put(const char* s, size_t k) override { o->put(s, k); n += int(k); }
                } c;
                c.o = &out;
                utf8_put(c, char32_t(*f));
                total += c.n;
            } else {
                const Ch* start = f;
                while (*f && *f != '%') f++;
                out.put(reinterpret_cast<const char*>(start), size_t(f - start));
                total += int(f - start);
                continue;
            }
            f++;
            continue;
        }
        f++;
        if (*f == '%') { out.put("%", 1); total++; f++; continue; }
        Spec s;
        // positional arguments (%1$d) are not used by the game
        while (*f == '-' || *f == '+' || *f == ' ' || *f == '#' || *f == '0' || *f == '\'') {
            if (*f != '\'') s.flags += char(*f);
            f++;
        }
        if (*f == '*') { s.width = int(int32_t(sysv_va_gp(ap))); if (s.width < 0) { s.flags += '-'; s.width = -s.width; } f++; }
        else if (*f >= '0' && *f <= '9') { s.width = 0; while (*f >= '0' && *f <= '9') s.width = s.width * 10 + (*f++ - '0'); }
        if (*f == '.') {
            f++;
            s.prec = 0;
            if (*f == '*') { s.prec = int(int32_t(sysv_va_gp(ap))); f++; }
            else while (*f >= '0' && *f <= '9') s.prec = s.prec * 10 + (*f++ - '0');
        }
        for (;;) {
            if (*f == 'h') { s.len = s.len == 1 ? 2 : 1; f++; }
            else if (*f == 'l') { s.len = s.len == 3 ? 4 : 3; f++; }
            else if (*f == 'q' || *f == 'j' || *f == 'z' || *f == 't') { s.len = 4; f++; }
            else if (*f == 'L') { s.len = 5; f++; }
            else break;
        }
        if (!*f) break;
        s.conv = char(*f++);
        if (s.len == 3 && s.conv != 'c' && s.conv != 's') s.len = 4;  // Linux long is 64-bit
        total += convert(out, s, ap, wide, total);
    }
    return total;
}

}  // namespace

int format(FormatSink& out, const char* fmt, sysv_va_list* ap) { return format_impl(out, fmt, ap, false); }
int wformat(FormatSink& out, const char32_t* fmt, sysv_va_list* ap) { return format_impl(out, fmt, ap, true); }

// sscanf: each conversion is handed to the CRT's sscanf with the right Windows length modifier, using %n to learn
// how much it consumed.
int scan(const char* input, const char* fmt, sysv_va_list* ap, size_t* consumed) {
    const char* in = input;
    int assigned = 0;
    bool any_input = false;
    for (const char* f = fmt; *f;) {
        if (std::isspace((unsigned char)*f)) {
            while (std::isspace((unsigned char)*in)) in++;
            f++;
            continue;
        }
        if (*f != '%' || f[1] == '%') {
            if (*f == '%') f++;
            if (*in != *f) break;
            in++, f++;
            continue;
        }
        f++;
        bool suppress = false;
        if (*f == '*') { suppress = true; f++; }
        std::string width;
        while (*f >= '0' && *f <= '9') width += *f++;
        int len = 0;
        for (;;) {
            if (*f == 'h') { len = len == 1 ? 2 : 1; f++; }
            else if (*f == 'l') { len = len == 3 ? 4 : 3; f++; }
            else if (*f == 'q' || *f == 'j' || *f == 'z' || *f == 't') { len = 4; f++; }
            else if (*f == 'L') { len = 5; f++; }
            else break;
        }
        char conv = *f++;
        std::string set;
        if (conv == '[') {
            set = "[";
            if (*f == '^') set += *f++;
            if (*f == ']') set += *f++;
            while (*f && *f != ']') set += *f++;
            if (*f) set += *f++;
        }
        if (conv == 'n') {
            uint64_t p = sysv_va_gp(ap);
            if (!suppress) *reinterpret_cast<int32_t*>(p) = int(in - input);
            continue;
        }
        if (!*in) break;
        std::string spec = "%";
        if (suppress) spec += '*';
        spec += width;
        const char* mod = "";
        bool is_float = conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G' || conv == 'a';
        bool is_int = conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o';
        if (is_int) mod = len == 1 ? "h" : len == 2 ? "hh" : (len == 3 || len == 4) ? "ll" : "";
        else if (is_float) mod = (len == 3) ? "l" : "";
        spec += mod;
        spec += conv == '[' ? set : std::string(1, conv);
        spec += "%n";
        int n = -1, r;
        if (suppress) {
            r = std::sscanf(in, spec.c_str(), &n);
        } else {
            uint64_t p = sysv_va_gp(ap);
            if (is_float && len == 5) {
                double tmp;
                std::string s2 = spec;
                s2.replace(s2.find('f') != std::string::npos ? s2.rfind(conv) : 0, 1, "lf");
                r = std::sscanf(in, s2.c_str(), &tmp, &n);
                if (r == 1) {
                    long double* ld = reinterpret_cast<long double*>(p);  // 80-bit on Linux: write as double bits
                    std::memset(ld, 0, 16);
                    std::memcpy(ld, &tmp, 8);
                }
            } else {
                r = std::sscanf(in, spec.c_str(), reinterpret_cast<void*>(p), &n);
            }
            if (r == 1) assigned++;
        }
        if (n < 0) break;
        any_input = true;
        in += n;
    }
    if (consumed) *consumed = size_t(in - input);
    if (!any_input && assigned == 0 && !*in) return -1;
    return assigned;
}

}  // namespace raven::lnx
