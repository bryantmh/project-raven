// Script edits for presentation settings. The game's UI and flow are scripts ("programs") in encrypted zips under
// data/prod/archives; ArchiveLoad (0x5a8960) decrypts one into a buffer. We wrap it and rewrite matching files'
// text before the game parses them. Scripts load at boot, so these settings apply on restart.
#include "game/script_patches.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <string>
#include <vector>

#include "app/config.h"
#include "game/hook.h"
#include "linux/shim.h"
#include "log.h"

namespace raven::game {
namespace {
// int ArchiveLoad(const char* type, const char* path, char** buf, uint32_t* size, void* (*alloc)(size_t),
//                 void (*free)(void*), int terminate). *size includes 2 zero bytes when `terminate` is set.
using AllocFn = SYSV void* (*)(size_t);
using FreeFn = SYSV void (*)(void*);
using ArchiveLoad = SYSV int (*)(const char*, const char*, char**, uint32_t*, AllocFn, FreeFn, int);
constexpr uint64_t kArchiveLoad = 0x5a8960;
ArchiveLoad g_original = nullptr;

struct Edit {
    const char* file;     // path suffix
    const char* find;     // exact text
    const char* replace;
    bool app::Config::*enabled;
};
// Hides a script's object for good: HIDE, then sleep forever.
#define HIDE_FOREVER "#-EndHeader\nHIDE             \nSLEEP            0"
const Edit kEdits[] = {
    // "Money In: $0.00" / "FREEPLAY" under the attract, menu and game screens.
    {"attract_2d/moneyin_txt.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    {"attract_2d/MoneyIn_Cost.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    {"attract_2d/MoneyIn_Cost2.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    {"moneyin_game/MoneyInGame_Cost.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    {"moneyin_game/MoneyInGame_Cost2.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    // Xbox Live "Get Connected" screen: the boot flow still loads its scene for one frame (src/game/patches.cpp
    // ends the wait); never create the popup itself, so nothing flashes.
    {"online_popup/online_popup.txt", "trigger = SCENE_START", "trigger = RAVEN_NEVER", &app::Config::skip_xbox_screen},
    // QR scanner (webcam) notices: "QR scanners need to be configured!", "P1/P2 QR Scanner is not detected!",
    // and the in-game "webcam not found" plaques.
    {"attract_2d/qr_scanner_config.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_qr_notices},
    {"attract_2d/qr_scanner_p1.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_qr_notices},
    {"attract_2d/qr_scanner_p2.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_qr_notices},
    {"perm/webcam_notfound1.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_qr_notices},
    {"perm/webcam_notfound2.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_qr_notices},
    {"perm/webcam_notfound3.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_qr_notices},
    {"perm/webcam_notfound4.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_qr_notices},
    // Game mode / mission select countdown: the timer object removes itself before starting its 30 s timer, so
    // there is no countdown and no timeout.
    {"gamesel_time.txt", "UI_TIMER_START   30,0", "KILLPRIME        ", &app::Config::no_menu_countdown},
    {"gamesel_time_r.txt", "UI_TIMER_START   30,0", "KILLPRIME        ", &app::Config::no_menu_countdown},
};

bool ends_with(const char* s, const char* suffix) {
    size_t a = std::strlen(s), b = std::strlen(suffix);
    return a >= b && std::memcmp(s + a - b, suffix, b) == 0;
}

// RAVEN_DUMP_SCRIPTS=<folder>: save every decrypted script there (tree under data/), for finding what to edit.
void dump(const char* path, const char* buf, uint32_t size) {
    static const char* dir = std::getenv("RAVEN_DUMP_SCRIPTS");
    if (!dir) return;
    const char* rel = std::strstr(path, "/data/");
    if (!rel) return;
    std::filesystem::path out = std::filesystem::path(dir) / (rel + 1);
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    if (FILE* f = _wfopen(out.c_str(), L"wb")) {
        size_t n = 0;
        while (n < size && buf[n]) n++;
        std::fwrite(buf, 1, n, f);
        std::fclose(f);
    }
}

SYSV int h_archive_load(const char* type, const char* path, char** buf, uint32_t* size, AllocFn alloc, FreeFn free,
                        int terminate) {
    bool fresh = buf && !*buf;
    int r = g_original(type, path, buf, size, alloc, free, terminate);
    RTRACE("archive %s: %s (%u bytes) = %d", type ? type : "?", path ? path : "?", size ? *size : 0, r);
    if (r != 0 || !fresh || !path || !*buf || !size) return r;
    dump(path, *buf, *size);
    std::string text;
    bool changed = false;
    for (auto& e : kEdits) {
        if (!(app::config().*e.enabled) || !ends_with(path, e.file)) continue;
        if (!changed) text.assign(*buf, std::strlen(*buf) < *size ? std::strlen(*buf) : *size);
        size_t at = text.find(e.find);
        if (at == std::string::npos) {
            RLOG("script patch: \"%s\" not found in %s", e.find, path);
            continue;
        }
        text.replace(at, std::strlen(e.find), e.replace);
        changed = true;
    }
    if (!changed) return r;
    uint32_t pad = terminate ? 2 : 0;
    auto out = static_cast<char*>(alloc(text.size() + pad));
    std::memcpy(out, text.data(), text.size());
    std::memset(out + text.size(), 0, pad);
    free(*buf);
    *buf = out;
    *size = uint32_t(text.size() + pad);
    RLOG("script patch: edited %s", path);
    return r;
}
}  // namespace

void install_script_patches() {
    // push r15; push r14; mov r15, rcx; push r13; push r12; mov ecx, 0x80 (16 bytes, position-independent)
    g_original = reinterpret_cast<ArchiveLoad>(make_trampoline(kArchiveLoad, 16));
    hook_jump(kArchiveLoad, reinterpret_cast<const void*>(&h_archive_load));
}
}  // namespace raven::game
