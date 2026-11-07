// Process startup and exit, environment, errno, time, signals and the assorted system calls the game makes.
#include <windows.h>
#include <winsock2.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "linux/fd.h"
#include "linux/lnx.h"
#include "linux/sysv_va.h"
#include "log.h"

using namespace raven::lnx;

namespace raven::lnx {
static thread_local int t_errno = 0;
int& err() { return t_errno; }

int from_crt_errno(int e) {
    switch (e) {
        case EAGAIN: return L_EAGAIN;
        case ENOSYS: return L_ENOSYS;
        case ETIMEDOUT: return L_ETIMEDOUT;
        case ENOTEMPTY: return L_ENOTEMPTY;
        case ENAMETOOLONG: return L_ENAMETOOLONG;
        case EDEADLK: return L_EDEADLK;
        default: return e <= 34 ? e : L_EIO;
    }
}
int from_win32_error(unsigned long e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: return L_ENOENT;
        case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return L_EACCES;
        case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return L_EEXIST;
        case ERROR_DIR_NOT_EMPTY: return L_ENOTEMPTY;
        case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return L_ENOSPC;
        case ERROR_INVALID_HANDLE: return L_EBADF;
        case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return L_ENOMEM;
        case ERROR_DIRECTORY: return L_ENOTDIR;
        default: return L_EIO;
    }
}

int64_t mono_ns() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return int64_t(c.QuadPart / freq.QuadPart) * 1000000000 + int64_t(c.QuadPart % freq.QuadPart) * 1000000000 / freq.QuadPart;
}
int64_t wall_ns() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return int64_t(t - 116444736000000000ull) * 100;
}
}  // namespace raven::lnx

SHIM(int*, __errno_location, ()) { return &err(); }
static thread_local int t_h_errno = 0;
SHIM(int*, __h_errno_location, ()) { return &t_h_errno; }

static const char* const kErrStr[] = {
    "Success", "Operation not permitted", "No such file or directory", "No such process", "Interrupted system call",
    "Input/output error", "No such device or address", "Argument list too long", "Exec format error",
    "Bad file descriptor", "No child processes", "Resource temporarily unavailable", "Cannot allocate memory",
    "Permission denied", "Bad address", "Block device required", "Device or resource busy", "File exists",
    "Invalid cross-device link", "No such device", "Not a directory", "Is a directory", "Invalid argument",
    "Too many open files in system", "Too many open files", "Inappropriate ioctl for device", "Text file busy",
    "File too large", "No space left on device", "Illegal seek", "Read-only file system", "Too many links",
    "Broken pipe", "Numerical argument out of domain", "Numerical result out of range"};
SHIM(char*, strerror, (int e)) {
    static thread_local char buf[64];
    if (e >= 0 && e < int(sizeof kErrStr / sizeof *kErrStr)) return const_cast<char*>(kErrStr[e]);
    std::snprintf(buf, sizeof buf, "Unknown error %d", e);
    return buf;
}
SHIM(char*, strerror_r, (int e, char* buf, size_t n)) { return lx_strerror(e); }  // GNU variant returns the string

// ---- environment (the game sees a small Linux environment, not Windows')
namespace {
std::mutex g_env_mutex;
std::vector<char*> g_env;
}  // namespace
char** g_environ_ptr = nullptr;
SHIM_VAR(environ, &g_environ_ptr);
SHIM_VAR(__environ, &g_environ_ptr);

namespace raven::lnx {
void env_init(const std::vector<std::string>& vars) {
    std::lock_guard<std::mutex> lk(g_env_mutex);
    for (auto& v : vars) g_env.push_back(_strdup(v.c_str()));
    g_env.push_back(nullptr);
    g_environ_ptr = g_env.data();
}
}  // namespace raven::lnx

SHIM(char*, getenv, (const char* name)) {
    std::lock_guard<std::mutex> lk(g_env_mutex);
    size_t n = std::strlen(name);
    for (char* e : g_env)
        if (e && !std::strncmp(e, name, n) && e[n] == '=') {
            RTRACE("getenv %s = %s", name, e + n + 1);
            return e + n + 1;
        }
    RTRACE("getenv %s (unset)", name);
    return nullptr;
}
SHIM_ALIAS(secure_getenv, getenv);
SHIM(int, setenv, (const char* name, const char* value, int overwrite)) {
    std::lock_guard<std::mutex> lk(g_env_mutex);
    size_t n = std::strlen(name);
    std::string kv = std::string(name) + "=" + value;
    for (char*& e : g_env)
        if (e && !std::strncmp(e, name, n) && e[n] == '=') {
            if (overwrite) e = _strdup(kv.c_str());
            return 0;
        }
    g_env.back() = _strdup(kv.c_str());
    g_env.push_back(nullptr);
    g_environ_ptr = g_env.data();
    return 0;
}
SHIM(int, unsetenv, (const char* name)) {
    std::lock_guard<std::mutex> lk(g_env_mutex);
    size_t n = std::strlen(name);
    for (size_t i = 0; i + 1 < g_env.size(); i++)
        if (!std::strncmp(g_env[i], name, n) && g_env[i][n] == '=') {
            g_env.erase(g_env.begin() + i);
            break;
        }
    g_environ_ptr = g_env.data();
    return 0;
}
SHIM(int, putenv, (char* kv)) {
    const char* eq = std::strchr(kv, '=');
    if (!eq) return lx_unsetenv(kv);
    return lx_setenv(std::string(kv, size_t(eq - kv)).c_str(), eq + 1, 1);
}

// ---- startup and exit
namespace {
struct AtExit {
    SYSV void (*fn)(void*);
    void* arg;
    void* dso;
};
std::mutex g_atexit_mutex;
std::vector<AtExit> g_atexit;
std::atomic<bool> g_exiting{false};
}  // namespace

SHIM(int, __cxa_atexit, (SYSV void (*fn)(void*), void* arg, void* dso)) {
    std::lock_guard<std::mutex> lk(g_atexit_mutex);
    g_atexit.push_back({fn, arg, dso});
    return 0;
}
SHIM(int, atexit, (SYSV void (*fn)())) { return lx___cxa_atexit(reinterpret_cast<SYSV void (*)(void*)>(fn), nullptr, nullptr); }
SHIM(void, __cxa_finalize, (void* dso)) {}
SHIM(int, __cxa_thread_atexit_impl, (SYSV void (*fn)(void*), void* obj, void* dso)) { return 0; }  // thread_local dtors skipped

namespace raven::lnx { void on_game_exit(int code); }
extern "C" SYSV int lx_fflush(void*);

SHIM(void, exit, (int code)) {
    if (!g_exiting.exchange(true)) {
        RLOG("game exit(%d)", code);
        std::vector<AtExit> list;
        {
            std::lock_guard<std::mutex> lk(g_atexit_mutex);
            list.swap(g_atexit);
        }
        for (auto it = list.rbegin(); it != list.rend(); ++it) it->fn(it->arg);
        lx_fflush(nullptr);
    }
    on_game_exit(code);
}
SHIM(void, _exit, (int code)) { on_game_exit(code); }
SHIM_ALIAS(_Exit, _exit);
SHIM(void, abort, ()) { raven::log_fatal("The game called abort()."); }

using MainFn = SYSV int (*)(int, char**, char**);
using InitFn = SYSV void (*)(int, char**, char**);
SHIM(int, __libc_start_main, (MainFn main_fn, int argc, char** argv, InitFn init, SYSV void (*fini)(),
                              SYSV void (*rtld_fini)(), void* stack_end)) {
    RLOG("__libc_start_main: main %p, %d args", (void*)main_fn, argc);
    if (fini) lx_atexit(fini);
    if (init) init(argc, argv, g_environ_ptr);
    RLOG("game initializers done, calling main");
    lx_exit(main_fn(argc, argv, g_environ_ptr));
    return 0;
}

// ---- identity / system info
SHIM(int, getpid, ()) { return int(GetCurrentProcessId()); }
SHIM(int, getppid, ()) { return 1; }
SHIM(int, gettid, ()) { return int(GetCurrentThreadId()); }
SHIM(unsigned, getuid, ()) { return 0; }
SHIM_ALIAS(geteuid, getuid);
SHIM_ALIAS(getgid, getuid);
SHIM_ALIAS(getegid, getuid);
SHIM(int, getlogin_r, (char* buf, size_t n)) { std::snprintf(buf, n, "root"); return 0; }
struct lpasswd { char *pw_name, *pw_passwd; unsigned pw_uid, pw_gid; char *pw_gecos, *pw_dir, *pw_shell; };
static lpasswd g_pw{(char*)"root", (char*)"x", 0, 0, (char*)"root", (char*)"/root", (char*)"/bin/sh"};
SHIM(lpasswd*, getpwuid, (unsigned)) { return &g_pw; }
SHIM(int, getpwuid_r, (unsigned, lpasswd* pw, char*, size_t, lpasswd** out)) { *pw = g_pw; *out = pw; return 0; }
struct lutsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
SHIM(int, uname, (lutsname* u)) {
    std::memset(u, 0, sizeof *u);
    std::strcpy(u->sysname, "Linux");
    std::strcpy(u->nodename, "raven");
    std::strcpy(u->release, "4.15.0");
    std::strcpy(u->version, "#1 SMP");
    std::strcpy(u->machine, "x86_64");
    return 0;
}
SHIM(int, gethostname, (char* buf, size_t n)) { std::snprintf(buf, n, "raven"); return 0; }
SHIM(lnx_long, sysconf, (int name)) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (name) {
        case 2: return 100;                         // _SC_CLK_TCK
        case 30: return 4096;                       // _SC_PAGESIZE
        case 83: case 84: return si.dwNumberOfProcessors;  // _SC_NPROCESSORS_CONF/ONLN
        case 85: {                                  // _SC_PHYS_PAGES
            MEMORYSTATUSEX ms{sizeof ms};
            GlobalMemoryStatusEx(&ms);
            return lnx_long(ms.ullTotalPhys / 4096);
        }
        case 0: return 2097152;                     // _SC_ARG_MAX
        case 4: return 1024;                        // _SC_OPEN_MAX
        case 70: case 71: return 1024;              // getpw/getgr buffer sizes
        default: return -1;
    }
}
SHIM(int, getrlimit, (int, uint64_t* r)) { r[0] = r[1] = ~0ull; return 0; }
SHIM(int, setrlimit, (int, const uint64_t*)) { return 0; }
SHIM(int, nice, (int)) { return 0; }
SHIM(int, setpriority, (int, unsigned, int)) { return 0; }
SHIM(int, prctl, (int, uint64_t, uint64_t, uint64_t, uint64_t)) { return 0; }
SHIM(lnx_long, ptrace, (int, int, void*, void*)) { return 0; }  // the protector's anti-debug probe
SHIM(int, fork, ()) { return fail(L_ENOSYS); }
SHIM(int, execlp, (const char*, const char*)) { return fail(L_ENOSYS); }
SHIM(int, system, (const char* cmd)) {
    RLOG("game system(\"%s\") ignored", cmd ? cmd : "");
    return cmd ? 0 : 1;
}
SHIM(void*, popen, (const char* cmd, const char* mode)) {
    RLOG("game popen(\"%s\") ignored", cmd);
    fail(L_ENOSYS);
    return nullptr;
}
SHIM(int, pclose, (void*)) { return -1; }
SHIM(int, waitpid, (int, int* status, int)) { if (status) *status = 0; return fail(L_ECHILD); }
SHIM(int, kill, (int, int)) { return 0; }
// syscall(n, ...): only the ones the game uses.
SHIM(lnx_long, syscall, (lnx_long n, lnx_long a, lnx_long b, lnx_long c)) {
    switch (n) {
        case 186: return GetCurrentThreadId();       // gettid
        case 318: {                                   // getrandom
            auto p = reinterpret_cast<uint8_t*>(a);
            for (lnx_long i = 0; i < b; i++) p[i] = uint8_t(rand());
            return b;
        }
        default:
            RLOG("syscall %lld not implemented", (long long)n);
            return fail(L_ENOSYS);
    }
}
SHIM(int, getentropy, (void* p, size_t n)) {
    for (size_t i = 0; i < n; i++) static_cast<uint8_t*>(p)[i] = uint8_t(rand());
    return 0;
}
SHIM(uint32_t, arc4random, ()) { return (uint32_t(rand()) << 16) ^ uint32_t(rand()); }

// ---- signals (no signals are ever delivered)
SHIM(int, sigaction, (int sig, const void* act, void* old)) {
    RTRACE("sigaction(%d, handler %p)", sig, act ? *static_cast<void* const*>(act) : nullptr);
    if (old) std::memset(old, 0, 152);
    return 0;
}
SHIM(void*, signal, (int sig, void* h)) { RTRACE("signal(%d, %p)", sig, h); return nullptr; }
SHIM(int, sigemptyset, (uint64_t* s)) { std::memset(s, 0, 128); return 0; }
SHIM(int, sigfillset, (uint64_t* s)) { std::memset(s, 0xff, 128); return 0; }
SHIM(int, sigaddset, (uint64_t* s, int n)) { s[(n - 1) / 64] |= 1ull << ((n - 1) % 64); return 0; }
SHIM(int, sigdelset, (uint64_t* s, int n)) { s[(n - 1) / 64] &= ~(1ull << ((n - 1) % 64)); return 0; }
SHIM(int, sigprocmask, (int, const void*, void*)) { return 0; }
SHIM(int, sigqueue, (int, int, void*)) { return 0; }
SHIM(int, __libc_current_sigrtmin, ()) { return 34; }
SHIM(int, raise, (int sig)) { RLOG("game raise(%d)", sig); return 0; }

// ---- time
SHIM(int64_t, time, (int64_t* t)) {
    int64_t s = wall_ns() / 1000000000;
    if (t) *t = s;
    return s;
}
SHIM(int, gettimeofday, (ltimeval* tv, void*)) {
    int64_t ns = wall_ns();
    tv->tv_sec = ns / 1000000000;
    tv->tv_usec = (ns / 1000) % 1000000;
    return 0;
}
SHIM(int, settimeofday, (const ltimeval*, const void*)) { return 0; }
SHIM(int, clock_gettime, (int clk, ltimespec* ts)) {
    int64_t ns;
    if (clk == 0 || clk == 5 || clk == 8 || clk == 11) ns = wall_ns();  // REALTIME(_COARSE/_ALARM), TAI
    else if (clk == 2 || clk == 3) {  // process / thread CPU time
        FILETIME a, b, k, u;
        if (clk == 2) GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
        else GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u);
        ns = int64_t(((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime) + ((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime)) * 100;
    } else ns = mono_ns();
    ts->tv_sec = ns / 1000000000;
    ts->tv_nsec = ns % 1000000000;
    return 0;
}
SHIM(int, clock_getres, (int, ltimespec* ts)) {
    if (ts) { ts->tv_sec = 0; ts->tv_nsec = 100; }
    return 0;
}
SHIM(int, timespec_get, (ltimespec* ts, int base)) { lx_clock_gettime(0, ts); return base; }
SHIM(lnx_long, clock, ()) {
    ltimespec ts;
    lx_clock_gettime(2, &ts);
    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

namespace raven::lnx {
// Sleep with sub-millisecond accuracy: a high-resolution waitable timer, then a short spin.
void precise_sleep_ns(int64_t ns) {
    if (ns <= 0) { SwitchToThread(); return; }
    int64_t end = mono_ns() + ns;
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (ns > 1500000 && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -int64_t((ns - 1000000) / 100);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
    }
    while (mono_ns() < end) {
        if (end - mono_ns() > 200000) Sleep(0);
        else YieldProcessor();
    }
}
}  // namespace raven::lnx

SHIM(int, nanosleep, (const ltimespec* req, ltimespec* rem)) {
    precise_sleep_ns(req->tv_sec * 1000000000ll + req->tv_nsec);
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}
SHIM(int, clock_nanosleep, (int clk, int flags, const ltimespec* req, ltimespec* rem)) {
    int64_t ns = req->tv_sec * 1000000000ll + req->tv_nsec;
    if (flags & 1) ns -= (clk == 0 ? wall_ns() : mono_ns());  // TIMER_ABSTIME
    precise_sleep_ns(ns);
    return 0;
}
SHIM(int, usleep, (unsigned us)) { precise_sleep_ns(int64_t(us) * 1000); return 0; }
SHIM(unsigned, sleep, (unsigned s)) { Sleep(s * 1000); return 0; }

static void to_ltm(const std::tm& t, ltm* out, int64_t gmtoff, const char* zone) {
    out->tm_sec = t.tm_sec; out->tm_min = t.tm_min; out->tm_hour = t.tm_hour; out->tm_mday = t.tm_mday;
    out->tm_mon = t.tm_mon; out->tm_year = t.tm_year; out->tm_wday = t.tm_wday; out->tm_yday = t.tm_yday;
    out->tm_isdst = t.tm_isdst; out->pad = 0; out->tm_gmtoff = gmtoff; out->tm_zone = zone;
}
static std::tm from_ltm(const ltm* in) {
    std::tm t{};
    t.tm_sec = in->tm_sec; t.tm_min = in->tm_min; t.tm_hour = in->tm_hour; t.tm_mday = in->tm_mday;
    t.tm_mon = in->tm_mon; t.tm_year = in->tm_year; t.tm_wday = in->tm_wday; t.tm_yday = in->tm_yday;
    t.tm_isdst = in->tm_isdst;
    return t;
}
SHIM(ltm*, localtime_r, (const int64_t* t, ltm* out)) {
    std::tm tm{};
    __time64_t tt = *t;
    _localtime64_s(&tm, &tt);
    long tz = 0;
    _get_timezone(&tz);
    to_ltm(tm, out, -int64_t(tz) + (tm.tm_isdst ? 3600 : 0), "LOC");
    return out;
}
SHIM(ltm*, gmtime_r, (const int64_t* t, ltm* out)) {
    std::tm tm{};
    __time64_t tt = *t;
    _gmtime64_s(&tm, &tt);
    to_ltm(tm, out, 0, "GMT");
    return out;
}
static thread_local ltm t_tm;
SHIM(ltm*, localtime, (const int64_t* t)) { return lx_localtime_r(t, &t_tm); }
SHIM(ltm*, gmtime, (const int64_t* t)) { return lx_gmtime_r(t, &t_tm); }
SHIM(int64_t, mktime, (ltm* t)) {
    std::tm tm = from_ltm(t);
    int64_t r = _mktime64(&tm);
    to_ltm(tm, t, t->tm_gmtoff, t->tm_zone);
    return r;
}
SHIM(int64_t, timegm, (ltm* t)) {
    std::tm tm = from_ltm(t);
    return _mkgmtime64(&tm);
}
SHIM(void, tzset, ()) { _tzset(); }
SHIM(size_t, strftime, (char* s, size_t n, const char* fmt, const ltm* t)) {
    std::tm tm = from_ltm(t);
    // %e and %k are glibc-only; map to the closest CRT conversions.
    std::string f;
    for (const char* p = fmt; *p; p++) {
        if (p[0] == '%' && p[1] == 'e') { f += "%#d"; p++; }
        else if (p[0] == '%' && p[1] == 'k') { f += "%#H"; p++; }
        else if (p[0] == '%' && p[1] == 'z') { f += "+0000"; p++; }
        else f += *p;
    }
    return std::strftime(s, n, f.c_str(), &tm);
}
SHIM(size_t, __strftime_l, (char* s, size_t n, const char* fmt, const ltm* t, void*)) { return lx_strftime(s, n, fmt, t); }
SHIM(size_t, __wcsftime_l, (char32_t* s, size_t n, const char32_t* fmt, const ltm* t, void*)) {
    std::string f;
    for (; *fmt; fmt++) f += char(*fmt);
    char buf[512];
    size_t k = lx_strftime(buf, sizeof buf, f.c_str(), t);
    if (k + 1 > n) return 0;
    for (size_t i = 0; i < k; i++) s[i] = (unsigned char)buf[i];
    s[k] = 0;
    return k;
}
SHIM(char*, asctime_r, (const ltm* t, char* buf)) {
    std::tm tm = from_ltm(t);
    asctime_s(buf, 26, &tm);
    return buf;
}

// ---- setjmp / longjmp (SysV callee-saved registers; glibc's pointer mangling is internal so not needed)
extern "C" SYSV int lx__setjmp(void* buf);
extern "C" SYSV void lx_longjmp(void* buf, int v);
__asm__(".text\n.globl lx__setjmp\n.p2align 4\nlx__setjmp:\n"
        "movq %rbx, 0(%rdi)\nmovq %rbp, 8(%rdi)\nmovq %r12, 16(%rdi)\nmovq %r13, 24(%rdi)\n"
        "movq %r14, 32(%rdi)\nmovq %r15, 40(%rdi)\nleaq 8(%rsp), %rdx\nmovq %rdx, 48(%rdi)\n"
        "movq (%rsp), %rdx\nmovq %rdx, 56(%rdi)\nmovl $0, 64(%rdi)\nxorl %eax, %eax\nretq\n"
        ".globl lx_longjmp\n.p2align 4\nlx_longjmp:\n"
        "movl %esi, %eax\ntestl %eax, %eax\njnz 1f\nincl %eax\n1:\n"
        "movq 0(%rdi), %rbx\nmovq 8(%rdi), %rbp\nmovq 16(%rdi), %r12\nmovq 24(%rdi), %r13\n"
        "movq 32(%rdi), %r14\nmovq 40(%rdi), %r15\nmovq 48(%rdi), %rsp\njmpq *56(%rdi)\n");
static ::raven::shim::Reg reg_setjmp("_setjmp", reinterpret_cast<void*>(&lx__setjmp));
static ::raven::shim::Reg reg_setjmp2("setjmp", reinterpret_cast<void*>(&lx__setjmp));
static ::raven::shim::Reg reg_sigsetjmp("__sigsetjmp", reinterpret_cast<void*>(&lx__setjmp));
static ::raven::shim::Reg reg_longjmp("longjmp", reinterpret_cast<void*>(&lx_longjmp));
static ::raven::shim::Reg reg_longjmp2("_longjmp", reinterpret_cast<void*>(&lx_longjmp));
static ::raven::shim::Reg reg_longjmp3("siglongjmp", reinterpret_cast<void*>(&lx_longjmp));
static ::raven::shim::Reg reg_longjmp4("__longjmp_chk", reinterpret_cast<void*>(&lx_longjmp));

// ---- misc
SHIM(int, backtrace, (void** buf, int n)) { return 0; }
SHIM(char**, backtrace_symbols, (void* const*, int)) { return nullptr; }
SHIM(void, backtrace_symbols_fd, (void* const*, int, int)) {}
SHIM(char*, gettext, (const char* s)) { return const_cast<char*>(s); }
SHIM(char*, dgettext, (const char*, const char* s)) { return const_cast<char*>(s); }
SHIM(char*, dcgettext, (const char*, const char* s, int)) { return const_cast<char*>(s); }
SHIM(char*, bindtextdomain, (const char*, const char* d)) { return const_cast<char*>(d); }
SHIM(char*, bind_textdomain_codeset, (const char*, const char* c)) { return const_cast<char*>(c); }
SHIM(char*, textdomain, (const char* d)) { return const_cast<char*>(d); }
SHIM(lnx_ulong, gnu_dev_makedev, (unsigned ma, unsigned mi)) { return (lnx_ulong(ma) << 8) | mi; }
SHIM(unsigned, gnu_dev_major, (lnx_ulong d)) { return unsigned((d >> 8) & 0xfff); }
SHIM(unsigned, gnu_dev_minor, (lnx_ulong d)) { return unsigned(d & 0xff); }
SHIM(int, ttyname_r, (int, char* buf, size_t n)) { return L_ENOTTY; }
SHIM(int, isatty, (int fd)) { return fd <= 2; }
// utmp (login records): none
SHIM(void, setutent, ()) {}
SHIM(void, endutent, ()) {}
SHIM(int, getutent_r, (void*, void** out)) { *out = nullptr; return -1; }
// mount table: none
SHIM(void*, setmntent, (const char*, const char*)) { return nullptr; }
SHIM(void*, getmntent, (void*)) { return nullptr; }
SHIM(int, endmntent, (void*)) { return 1; }
// SysV shared memory (the protector's): unsupported
SHIM(int, shmget, (int, size_t, int)) { return fail(L_ENOSYS); }
SHIM(void*, shmat, (int, const void*, int)) { fail(L_ENOSYS); return reinterpret_cast<void*>(-1); }
SHIM(int, shmdt, (const void*)) { return 0; }
SHIM(int, shmctl, (int, int, void*)) { return 0; }
// terminal I/O (serial ports are emulated devices; these only need to succeed)
SHIM(int, tcgetattr, (int, void* t)) { std::memset(t, 0, 60); return 0; }
SHIM(int, tcsetattr, (int, int, const void*)) { return 0; }
SHIM(int, tcflush, (int, int)) { return 0; }
SHIM(int, tcdrain, (int)) { return 0; }
SHIM(int, cfsetispeed, (void*, unsigned)) { return 0; }
SHIM(int, cfsetospeed, (void*, unsigned)) { return 0; }
SHIM(unsigned, cfgetispeed, (const void*)) { return 0; }
SHIM(int, timerfd_create, (int, int)) { return fail(L_ENOSYS); }
SHIM(int, timerfd_settime, (int, int, const void*, void*)) { return fail(L_ENOSYS); }
SHIM(int64_t, sendfile, (int out, int in, int64_t* off, size_t n)) { return fail(L_ENOSYS); }
SHIM(int, mincore, (void*, size_t len, uint8_t* vec)) {
    std::memset(vec, 1, (len + 4095) / 4096);
    return 0;
}
