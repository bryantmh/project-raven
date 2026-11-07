#include "log.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace raven {
bool g_trace = false;
static FILE* g_log = nullptr;
static std::mutex g_log_mutex;

void log_init(const char* path) {
    g_log = std::fopen(path, "w");
    const char* t = std::getenv("RAVEN_TRACE");
    g_trace = t && *t == '1';
}

void logv(const char* fmt, va_list ap) {
    char buf[4096];
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf - 1) n = sizeof buf - 2;
    if (buf[n - 1] != '\n') { buf[n++] = '\n'; buf[n] = 0; }
    std::lock_guard<std::mutex> lk(g_log_mutex);
    if (g_log) { std::fputs(buf, g_log); std::fflush(g_log); }
    std::fputs(buf, stderr);
}

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv(fmt, ap);
    va_end(ap);
}

void log_fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log("FATAL: %s", buf);
    // RAVEN_HEADLESS=1 (automated test runs): log only, no dialog.
    const char* headless = std::getenv("RAVEN_HEADLESS");
    if (!(headless && *headless == '1')) MessageBoxA(nullptr, buf, "Project Raven", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}
}  // namespace raven
