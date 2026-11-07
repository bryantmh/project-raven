// Locales: only the "C" locale exists. libstdc++'s GNU locale model reads glibc's __locale_struct directly
// (ctype tables), so newlocale returns objects with that layout.
#include <cstring>

#include "linux/lnx.h"
#include "log.h"

using namespace raven::lnx;

const uint16_t* raven_ctype_b();
const int32_t* raven_ctype_tolower();
const int32_t* raven_ctype_toupper();

struct LLocale {
    void* locales[13];
    const uint16_t* ctype_b;
    const int32_t* ctype_tolower;
    const int32_t* ctype_toupper;
    const char* names[13];
};
static_assert(sizeof(LLocale) == 232);

static LLocale* c_locale() {
    static LLocale l = [] {
        LLocale x{};
        x.ctype_b = raven_ctype_b();
        x.ctype_tolower = raven_ctype_tolower();
        x.ctype_toupper = raven_ctype_toupper();
        for (auto& n : x.names) n = "C";
        return x;
    }();
    return &l;
}
static thread_local LLocale* t_locale = nullptr;

SHIM(LLocale*, newlocale, (int mask, const char* name, LLocale* base)) {
    auto l = static_cast<LLocale*>(lx_malloc(sizeof(LLocale)));
    *l = *c_locale();
    return l;
}
SHIM_ALIAS(__newlocale, newlocale);
SHIM(LLocale*, duplocale, (LLocale* l)) {
    auto n = static_cast<LLocale*>(lx_malloc(sizeof(LLocale)));
    *n = (l && l != reinterpret_cast<LLocale*>(-1)) ? *l : *c_locale();
    return n;
}
SHIM_ALIAS(__duplocale, duplocale);
SHIM(void, freelocale, (LLocale* l)) {
    if (l && l != c_locale()) lx_free(l);
}
SHIM_ALIAS(__freelocale, freelocale);
SHIM(LLocale*, uselocale, (LLocale* l)) {
    LLocale* old = t_locale ? t_locale : reinterpret_cast<LLocale*>(-1);  // LC_GLOBAL_LOCALE
    if (l) t_locale = l == reinterpret_cast<LLocale*>(-1) ? nullptr : l;
    return old;
}
SHIM_ALIAS(__uselocale, uselocale);
SHIM(char*, setlocale, (int, const char* name)) { return const_cast<char*>("C"); }

SHIM(char*, nl_langinfo, (int item)) {
    switch (item) {
        case 0x10000: return const_cast<char*>(".");   // RADIXCHAR / DECIMAL_POINT
        case 0x10001: return const_cast<char*>("");    // THOUSEP
        case 0x10002: return const_cast<char*>("");    // GROUPING
        case 14: return const_cast<char*>("UTF-8");    // CODESET
        default: return const_cast<char*>("");
    }
}
SHIM(char*, __nl_langinfo_l, (int item, LLocale*)) { return lx_nl_langinfo(item); }
SHIM_ALIAS(nl_langinfo_l, __nl_langinfo_l);
SHIM(int, __strcoll_l, (const char* a, const char* b, LLocale*)) { return std::strcmp(a, b); }
SHIM(size_t, __strxfrm_l, (char* d, const char* s, size_t n, LLocale*)) {
    size_t l = std::strlen(s);
    if (l < n) std::memcpy(d, s, l + 1);
    return l;
}
struct llconv;
SHIM(void*, localeconv, ()) {
    static char dot[] = ".", empty[] = "";
    static char* conv[10] = {dot, empty, empty, empty, empty, empty, empty, empty, empty, empty};
    static uint8_t buf[96];
    std::memcpy(buf, conv, sizeof conv);
    std::memset(buf + 80, 127, 16);
    return buf;
}

SHIM(void*, iconv_open, (const char* to, const char* from)) {
    RLOG("iconv_open(%s, %s) unsupported", to, from);
    fail(L_EINVAL);
    return reinterpret_cast<void*>(-1);
}
SHIM(size_t, iconv, (void*, char**, size_t*, char**, size_t*)) { fail(L_EINVAL); return size_t(-1); }
SHIM(int, iconv_close, (void*)) { return 0; }
