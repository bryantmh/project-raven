// stdio on top of the fd layer. FILE is opaque to the game (it only uses the functions), so LFILE is our own.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "linux/fd.h"
#include "linux/lnx.h"
#include "linux/sysv_va.h"
#include "log.h"

using namespace raven::lnx;
extern "C" SYSV int64_t lx_lseek(int, int64_t, int);
extern "C" SYSV int lx_open(const char*, int, int);
extern "C" SYSV int lx_close(int);
extern "C" SYSV size_t lx_wcrtomb(char*, char32_t, void*);
extern "C" SYSV char* lx_strerror(int);

struct LFILE {
    int fd = -1;
    bool readable = false, writable = false, eof = false, error = false;
    std::string rbuf;     // read buffer
    size_t rpos = 0;
    std::string wbuf;     // write buffer
    int unget = -1;
    bool line_buffered = false;
    std::recursive_mutex m;
};

static LFILE* make_std(int fd, bool w) {
    auto f = new LFILE;
    f->fd = fd;
    f->readable = !w;
    f->writable = w;
    f->line_buffered = true;
    return f;
}
static LFILE* g_stdin = make_std(0, false);
static LFILE* g_stdout = make_std(1, true);
static LFILE* g_stderr = make_std(2, true);
SHIM_VAR(stdin, &g_stdin);
SHIM_VAR(stdout, &g_stdout);
SHIM_VAR(stderr, &g_stderr);
// The game holds its own copies of stdin/stdout/stderr (COPY relocations), filled from these at link time.

static int flush_locked(LFILE* f) {
    if (!f->wbuf.empty()) {
        size_t off = 0;
        while (off < f->wbuf.size()) {
            int64_t r = fd_write(f->fd, f->wbuf.data() + off, f->wbuf.size() - off);
            if (r <= 0) { f->error = true; f->wbuf.clear(); return -1; }
            off += size_t(r);
        }
        f->wbuf.clear();
    }
    return 0;
}

static size_t write_locked(LFILE* f, const void* p, size_t n) {
    if (!f->writable) { f->error = true; return 0; }
    if (f->readable && f->rpos < f->rbuf.size()) {  // switching from reading: drop read-ahead, reposition
        lx_lseek(f->fd, -int64_t(f->rbuf.size() - f->rpos), 1);
        f->rbuf.clear();
        f->rpos = 0;
    }
    f->wbuf.append(static_cast<const char*>(p), n);
    if (f->wbuf.size() >= 65536 || (f->line_buffered && std::memchr(p, '\n', n))) flush_locked(f);
    return n;
}

static bool fill_locked(LFILE* f) {
    if (f->rpos < f->rbuf.size()) return true;
    flush_locked(f);
    f->rbuf.resize(65536);
    int64_t r = fd_read(f->fd, f->rbuf.data(), f->rbuf.size());
    if (r <= 0) {
        f->rbuf.clear();
        f->rpos = 0;
        if (r == 0) f->eof = true; else f->error = true;
        return false;
    }
    f->rbuf.resize(size_t(r));
    f->rpos = 0;
    return true;
}

static int getc_locked(LFILE* f) {
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (!fill_locked(f)) return -1;
    return (unsigned char)f->rbuf[f->rpos++];
}

static int parse_mode(const char* mode, bool* r, bool* w) {
    int flags = 0;
    bool plus = std::strchr(mode, '+') != nullptr;
    switch (mode[0]) {
        case 'r': flags = plus ? L_O_RDWR : L_O_RDONLY; break;
        case 'w': flags = (plus ? L_O_RDWR : L_O_WRONLY) | L_O_CREAT | L_O_TRUNC; break;
        case 'a': flags = (plus ? L_O_RDWR : L_O_WRONLY) | L_O_CREAT | L_O_APPEND; break;
        default: return -1;
    }
    *r = mode[0] == 'r' || plus;
    *w = mode[0] != 'r' || plus;
    return flags;
}

SHIM(LFILE*, fopen, (const char* path, const char* mode)) {
    bool r, w;
    int flags = parse_mode(mode, &r, &w);
    if (flags < 0) { err() = L_EINVAL; return nullptr; }
    int fd = lx_open(path, flags, 0644);
    if (fd < 0) return nullptr;
    auto f = new LFILE;
    f->fd = fd;
    f->readable = r;
    f->writable = w;
    return f;
}
SHIM_ALIAS(fopen64, fopen);
SHIM(LFILE*, fdopen, (int fd, const char* mode)) {
    if (!fd_get(fd)) { err() = L_EBADF; return nullptr; }
    bool r, w;
    parse_mode(mode, &r, &w);
    auto f = new LFILE;
    f->fd = fd;
    f->readable = r;
    f->writable = w;
    return f;
}
SHIM(LFILE*, freopen, (const char* path, const char* mode, LFILE* f)) { return f; }
SHIM(int, fclose, (LFILE* f)) {
    int r;
    {
        std::lock_guard<std::recursive_mutex> lk(f->m);
        r = flush_locked(f);
        if (f->fd > 2) lx_close(f->fd);
    }
    if (f != g_stdin && f != g_stdout && f != g_stderr) delete f;
    return r;
}
SHIM(int, fflush, (LFILE* f)) {
    if (!f) {
        for (LFILE* s : {g_stdout, g_stderr}) { std::lock_guard<std::recursive_mutex> lk(s->m); flush_locked(s); }
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lk(f->m);
    return flush_locked(f);
}
SHIM(size_t, fread, (void* p, size_t sz, size_t n, LFILE* f)) {
    if (!sz || !n) return 0;
    std::lock_guard<std::recursive_mutex> lk(f->m);
    size_t want = sz * n, got = 0;
    auto out = static_cast<char*>(p);
    if (f->unget >= 0 && want) { out[got++] = char(f->unget); f->unget = -1; }
    while (got < want) {
        if (f->rpos < f->rbuf.size()) {
            size_t k = std::min(want - got, f->rbuf.size() - f->rpos);
            std::memcpy(out + got, f->rbuf.data() + f->rpos, k);
            f->rpos += k;
            got += k;
            continue;
        }
        flush_locked(f);
        if (want - got >= 65536) {  // large reads bypass the buffer
            int64_t r = fd_read(f->fd, out + got, want - got);
            if (r <= 0) { if (r == 0) f->eof = true; else f->error = true; break; }
            got += size_t(r);
            continue;
        }
        if (!fill_locked(f)) break;
    }
    return got / sz;
}
SHIM(size_t, __fread_chk, (void* p, size_t, size_t sz, size_t n, LFILE* f)) { return lx_fread(p, sz, n, f); }
SHIM(size_t, fwrite, (const void* p, size_t sz, size_t n, LFILE* f)) {
    if (!sz || !n) return 0;
    std::lock_guard<std::recursive_mutex> lk(f->m);
    return write_locked(f, p, sz * n) / sz;
}
SHIM(int, fgetc, (LFILE* f)) {
    std::lock_guard<std::recursive_mutex> lk(f->m);
    return getc_locked(f);
}
SHIM_ALIAS(getc, fgetc);
SHIM_ALIAS(_IO_getc, fgetc);
SHIM(int, getchar, ()) { return lx_fgetc(g_stdin); }
SHIM(int, ungetc, (int c, LFILE* f)) {
    if (c < 0) return -1;
    std::lock_guard<std::recursive_mutex> lk(f->m);
    f->unget = c & 0xff;
    f->eof = false;
    return c & 0xff;
}
SHIM(char*, fgets, (char* s, int n, LFILE* f)) {
    std::lock_guard<std::recursive_mutex> lk(f->m);
    int i = 0;
    while (i < n - 1) {
        int c = getc_locked(f);
        if (c < 0) break;
        s[i++] = char(c);
        if (c == '\n') break;
    }
    if (i == 0) return nullptr;
    s[i] = 0;
    return s;
}
SHIM(int64_t, getline, (char** line, size_t* cap, LFILE* f)) {
    std::lock_guard<std::recursive_mutex> lk(f->m);
    std::string s;
    int c;
    while ((c = getc_locked(f)) >= 0) {
        s += char(c);
        if (c == '\n') break;
    }
    if (s.empty()) return -1;
    if (!*line || *cap < s.size() + 1) {
        *cap = s.size() + 1;
        lx_free(*line);
        *line = static_cast<char*>(lx_malloc(*cap));
    }
    std::memcpy(*line, s.c_str(), s.size() + 1);
    return int64_t(s.size());
}
SHIM(int, fputc, (int c, LFILE* f)) {
    char ch = char(c);
    std::lock_guard<std::recursive_mutex> lk(f->m);
    return write_locked(f, &ch, 1) == 1 ? (c & 0xff) : -1;
}
SHIM_ALIAS(putc, fputc);
SHIM(int, putchar, (int c)) { return lx_fputc(c, g_stdout); }
SHIM(int, fputs, (const char* s, LFILE* f)) {
    std::lock_guard<std::recursive_mutex> lk(f->m);
    write_locked(f, s, std::strlen(s));
    return 1;
}
SHIM(int, puts, (const char* s)) {
    std::lock_guard<std::recursive_mutex> lk(g_stdout->m);
    write_locked(g_stdout, s, std::strlen(s));
    write_locked(g_stdout, "\n", 1);
    return 1;
}
SHIM(int, fseek, (LFILE* f, int64_t off, int whence)) {
    std::lock_guard<std::recursive_mutex> lk(f->m);
    flush_locked(f);
    if (whence == 1) off -= int64_t(f->rbuf.size() - f->rpos) + (f->unget >= 0 ? 1 : 0);
    f->rbuf.clear();
    f->rpos = 0;
    f->unget = -1;
    f->eof = false;
    return lx_lseek(f->fd, off, whence) < 0 ? -1 : 0;
}
SHIM_ALIAS(fseeko, fseek);
SHIM_ALIAS(fseeko64, fseek);
SHIM(int64_t, ftell, (LFILE* f)) {
    std::lock_guard<std::recursive_mutex> lk(f->m);
    int64_t p = lx_lseek(f->fd, 0, 1);
    if (p < 0) return -1;
    return p - int64_t(f->rbuf.size() - f->rpos) + int64_t(f->wbuf.size()) - (f->unget >= 0 ? 1 : 0);
}
SHIM_ALIAS(ftello, ftell);
SHIM_ALIAS(ftello64, ftell);
SHIM(void, rewind, (LFILE* f)) { lx_fseek(f, 0, 0); f->error = false; }
struct lfpos { int64_t pos; uint8_t state[8]; };
SHIM(int, fgetpos, (LFILE* f, lfpos* p)) { p->pos = lx_ftell(f); return p->pos < 0 ? -1 : 0; }
SHIM(int, fsetpos, (LFILE* f, const lfpos* p)) { return lx_fseek(f, p->pos, 0); }
SHIM(int, feof, (LFILE* f)) { return f->eof; }
SHIM(int, ferror, (LFILE* f)) { return f->error; }
SHIM(void, clearerr, (LFILE* f)) { f->eof = f->error = false; }
SHIM(int, fileno, (LFILE* f)) { return f->fd; }
SHIM(int, setvbuf, (LFILE* f, char*, int mode, size_t)) { f->line_buffered = mode != 0; return 0; }
SHIM(void, setbuf, (LFILE*, char*)) {}
SHIM(void, flockfile, (LFILE* f)) { f->m.lock(); }
SHIM(void, funlockfile, (LFILE* f)) { f->m.unlock(); }

// ---- printf family
namespace {
struct FileSink : FormatSink {
    LFILE* f;
    void put(const char* s, size_t n) override { write_locked(f, s, n); }
};
struct BufSink : FormatSink {
    char* buf;
    size_t cap, n = 0;
    void put(const char* s, size_t k) override {
        if (cap && n < cap - 1) std::memcpy(buf + n, s, std::min(k, cap - 1 - n));
        n += k;
    }
    void finish() { if (cap) buf[std::min(n, cap - 1)] = 0; }
};
struct StrSink : FormatSink {
    std::string s;
    void put(const char* p, size_t k) override { s.append(p, k); }
};
}  // namespace

extern "C" SYSV int raven_vfprintf(LFILE* f, const char* fmt, sysv_va_list* ap) {
    std::lock_guard<std::recursive_mutex> lk(f->m);
    FileSink s;
    s.f = f;
    return format(s, fmt, ap);
}
extern "C" SYSV int raven_vsnprintf(char* buf, size_t n, const char* fmt, sysv_va_list* ap) {
    BufSink s;
    s.buf = buf;
    s.cap = n;
    format(s, fmt, ap);
    s.finish();
    return int(s.n);
}
extern "C" SYSV int raven_vsprintf(char* buf, const char* fmt, sysv_va_list* ap) { return raven_vsnprintf(buf, SIZE_MAX / 2, fmt, ap); }
extern "C" SYSV int raven_vprintf(const char* fmt, sysv_va_list* ap) { return raven_vfprintf(g_stdout, fmt, ap); }
extern "C" SYSV int raven_vasprintf(char** out, const char* fmt, sysv_va_list* ap) {
    StrSink s;
    format(s, fmt, ap);
    *out = static_cast<char*>(lx_malloc(s.s.size() + 1));
    std::memcpy(*out, s.s.c_str(), s.s.size() + 1);
    return int(s.s.size());
}
// _chk variants: (dest, flag, size, fmt, ...) / (fp, flag, fmt, ...)
extern "C" SYSV int raven_snprintf_chk(char* buf, size_t n, int, size_t, const char* fmt, sysv_va_list* ap) { return raven_vsnprintf(buf, n, fmt, ap); }
extern "C" SYSV int raven_sprintf_chk(char* buf, int, size_t, const char* fmt, sysv_va_list* ap) { return raven_vsprintf(buf, fmt, ap); }
extern "C" SYSV int raven_fprintf_chk(LFILE* f, int, const char* fmt, sysv_va_list* ap) { return raven_vfprintf(f, fmt, ap); }
extern "C" SYSV int raven_printf_chk(int, const char* fmt, sysv_va_list* ap) { return raven_vfprintf(g_stdout, fmt, ap); }
extern "C" SYSV int raven_asprintf_chk(char** out, int, const char* fmt, sysv_va_list* ap) { return raven_vasprintf(out, fmt, ap); }

SYSV_VARIADIC(printf, 1, raven_vprintf);
SYSV_VARIADIC(fprintf, 2, raven_vfprintf);
SYSV_VARIADIC(sprintf, 2, raven_vsprintf);
SYSV_VARIADIC(snprintf, 3, raven_vsnprintf);
SYSV_VARIADIC(asprintf, 2, raven_vasprintf);
SYSV_VARIADIC(__snprintf_chk, 5, raven_snprintf_chk);
SYSV_VARIADIC(__sprintf_chk, 4, raven_sprintf_chk);
SYSV_VARIADIC(__fprintf_chk, 3, raven_fprintf_chk);
SYSV_VARIADIC(__printf_chk, 2, raven_printf_chk);
SYSV_VARIADIC(__asprintf_chk, 3, raven_asprintf_chk);

SHIM(int, vfprintf, (LFILE* f, const char* fmt, sysv_va_list* ap)) { return raven_vfprintf(f, fmt, ap); }
SHIM(int, vprintf, (const char* fmt, sysv_va_list* ap)) { return raven_vprintf(fmt, ap); }
SHIM(int, vsnprintf, (char* b, size_t n, const char* fmt, sysv_va_list* ap)) { return raven_vsnprintf(b, n, fmt, ap); }
SHIM(int, vsprintf, (char* b, const char* fmt, sysv_va_list* ap)) { return raven_vsprintf(b, fmt, ap); }
SHIM(int, vasprintf, (char** o, const char* fmt, sysv_va_list* ap)) { return raven_vasprintf(o, fmt, ap); }
SHIM(int, __vsnprintf_chk, (char* b, size_t n, int, size_t, const char* fmt, sysv_va_list* ap)) { return raven_vsnprintf(b, n, fmt, ap); }
SHIM(int, __vsprintf_chk, (char* b, int, size_t, const char* fmt, sysv_va_list* ap)) { return raven_vsprintf(b, fmt, ap); }
SHIM(int, __vfprintf_chk, (LFILE* f, int, const char* fmt, sysv_va_list* ap)) { return raven_vfprintf(f, fmt, ap); }

// Wide printf: fwprintf / swprintf with 32-bit wchar_t formats.
extern "C" SYSV int raven_vfwprintf(LFILE* f, const char32_t* fmt, sysv_va_list* ap) {
    std::lock_guard<std::recursive_mutex> lk(f->m);
    FileSink s;
    s.f = f;
    return wformat(s, fmt, ap);
}
extern "C" SYSV int raven_vswprintf(char32_t* buf, size_t n, const char32_t* fmt, sysv_va_list* ap) {
    StrSink s;
    wformat(s, fmt, ap);
    // back to UTF-32
    size_t o = 0;
    for (size_t i = 0; i < s.s.size() && o + 1 < n;) {
        auto u = reinterpret_cast<const unsigned char*>(s.s.data() + i);
        uint32_t c = u[0];
        size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (len > 1) { c &= (0x7F >> len); for (size_t k = 1; k < len; k++) c = (c << 6) | (u[k] & 0x3F); }
        buf[o++] = c;
        i += len;
    }
    if (n) buf[o] = 0;
    return int(o);
}
extern "C" SYSV int raven_fwprintf_chk(LFILE* f, int, const char32_t* fmt, sysv_va_list* ap) { return raven_vfwprintf(f, fmt, ap); }
extern "C" SYSV int raven_swprintf_chk(char32_t* b, size_t n, int, size_t, const char32_t* fmt, sysv_va_list* ap) { return raven_vswprintf(b, n, fmt, ap); }
SYSV_VARIADIC(fwprintf, 2, raven_vfwprintf);
SYSV_VARIADIC(swprintf, 3, raven_vswprintf);
SYSV_VARIADIC(__fwprintf_chk, 3, raven_fwprintf_chk);
SYSV_VARIADIC(__swprintf_chk, 5, raven_swprintf_chk);
SHIM(int, vswprintf, (char32_t* b, size_t n, const char32_t* fmt, sysv_va_list* ap)) { return raven_vswprintf(b, n, fmt, ap); }
SHIM(uint32_t, getwc, (LFILE* f)) {
    int c = lx_fgetc(f);
    return c < 0 ? 0xFFFFFFFFu : uint32_t(c);
}
SHIM(uint32_t, putwc, (uint32_t c, LFILE* f)) {
    char buf[4];
    size_t k = lx_wcrtomb(buf, char32_t(c), nullptr);
    lx_fwrite(buf, 1, k, f);
    return c;
}
SHIM(uint32_t, ungetwc, (uint32_t c, LFILE* f)) { return lx_ungetc(int(c), f) < 0 ? 0xFFFFFFFFu : c; }

// ---- scanf family
extern "C" SYSV int raven_vsscanf(const char* s, const char* fmt, sysv_va_list* ap) { return scan(s, fmt, ap); }
extern "C" SYSV int raven_vfscanf(LFILE* f, const char* fmt, sysv_va_list* ap) {
    // Read a line at a time: the game's fscanf use is line-oriented config parsing.
    std::lock_guard<std::recursive_mutex> lk(f->m);
    std::string line;
    int c;
    while ((c = getc_locked(f)) >= 0) {
        line += char(c);
        if (c == '\n') break;
    }
    if (line.empty()) return -1;
    size_t used = 0;
    int r = scan(line.c_str(), fmt, ap, &used);
    // Push back what the conversion didn't consume.
    if (used < line.size()) {
        std::string rest = line.substr(used);
        f->rbuf = rest + f->rbuf.substr(f->rpos);
        f->rpos = 0;
        f->eof = false;
    }
    return r;
}
SYSV_VARIADIC(sscanf, 2, raven_vsscanf);
SYSV_VARIADIC(__isoc99_sscanf, 2, raven_vsscanf);
SYSV_VARIADIC(fscanf, 2, raven_vfscanf);
SYSV_VARIADIC(__isoc99_fscanf, 2, raven_vfscanf);
SHIM(int, vsscanf, (const char* s, const char* fmt, sysv_va_list* ap)) { return raven_vsscanf(s, fmt, ap); }
SHIM_ALIAS(__isoc99_vsscanf, vsscanf);
extern "C" SYSV int raven_vswscanf(const char32_t* s, const char32_t* fmt, sysv_va_list* ap) {
    std::string a, b;
    for (; *s; s++) a += *s < 128 ? char(*s) : '?';
    for (; *fmt; fmt++) b += *fmt < 128 ? char(*fmt) : '?';
    return scan(a.c_str(), b.c_str(), ap);
}
SYSV_VARIADIC(swscanf, 2, raven_vswscanf);
SYSV_VARIADIC(__isoc99_swscanf, 2, raven_vswscanf);

SHIM(void, perror, (const char* s)) {
    RLOG("[game] %s: %s", s ? s : "", lx_strerror(err()));
}
