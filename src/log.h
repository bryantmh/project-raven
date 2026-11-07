#pragma once
// Logging for the loader and the shim layer. Everything goes to raven.log next to the save folder and, when a
// console is attached, to stderr. log_fatal shows a message box and exits.
#include <cstdarg>

namespace raven {
void log_init(const char* path);
void log(const char* fmt, ...);
void logv(const char* fmt, va_list ap);
[[noreturn]] void log_fatal(const char* fmt, ...);
// Verbose shim tracing (RAVEN_TRACE=1): one line per traced call.
extern bool g_trace;
}  // namespace raven

#define RLOG(...) ::raven::log(__VA_ARGS__)
#define RTRACE(...) do { if (::raven::g_trace) ::raven::log(__VA_ARGS__); } while (0)
