// Script edits for presentation and gameplay settings. The game's UI and flow are scripts ("programs") in encrypted
// files: zips under data/prod/archives, decrypted by ArchiveLoad (0x5a8960), and files listed in eFileMaps, decrypted
// by 0x774b00. We wrap both and rewrite matching files' text before the game parses them. Scripts load at boot or when
// their scene starts, so most of these settings apply on restart.
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
#include "game/players.h"
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
    const char* file;     // path suffix, or "*text": any path containing text (and no log when `find` is missing)
    const char* find;     // exact text
    const char* replace;
    bool app::Config::*enabled;  // nullptr: always
};
// Ends a script's program as soon as it starts (event handlers can't bring it back, unlike HIDE_FOREVER).
#define KILL_AT_START "#-EndHeader\nKILLPRIME        "
// Hides a script's object for good: HIDE, then sleep forever.
#define HIDE_FOREVER "#-EndHeader\nHIDE             \nSLEEP            0"
const Edit kEdits[] = {
    // "Money In: $0.00" / "FREEPLAY" under the attract, menu and game screens.
    {"attract_2d/moneyin_txt.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    {"attract_2d/MoneyIn_Cost.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    {"attract_2d/MoneyIn_Cost2.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    {"moneyin_game/MoneyInGame_Cost.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    {"moneyin_game/MoneyInGame_Cost2.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    // The 4-player cabinet's, one per seat.
    {"moneyin_game/MoneyInGame_Cost1.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    {"moneyin_game/MoneyInGame_Cost3.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
    {"moneyin_game/MoneyInGame_Cost4.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::hide_credit_text},
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
    // Mission movies: "Press RELOAD to skip" (a texture); Project Raven draws "Press <Start icon> to skip" instead
    // (src/game/movie_skip.cpp).
    {"intro_movie/skip_text.txt", "#-EndHeader", HIDE_FOREVER, &app::Config::start_skips_movies},
    // Subtitles in mission movies (Settings > Gameplay), switchable while a movie plays. Each movie's subtitle
    // program checks the adjustment once and skips the subtitle scene when it's off: always play the scene.
    {"*_movie_subtitles.txt", "BR_ON_ADJUSTMENT gameOptions,subtitles,SHOW,0", "GOTO             SHOW", nullptr},
    // Each subtitle line fades in and then sleeps until its "done" event: instead, check the adjustment every 6
    // frames and show or hide the line (src/game/gameplay.cpp sets it live). BR_ON_ADJUSTMENT jumps when it's on.
    {"*/movie_subtitles/", "LABEL            TEXT\nTIMED_COLOR_SET  0,0,0,0,0,0,1,1,300,0,0,0,0,0,0\nSLEEP            0\n",
     "LABEL            TEXT\n"
     "BR_ON_ADJUSTMENT gameOptions,subtitles,RVN_SUB_ON,0\n"
     "LABEL            RVN_SUB_OFF\n"
     "SLEEP            6\n"
     "BR_ON_ADJUSTMENT gameOptions,subtitles,RVN_SUB_ON,0\n"
     "GOTO             RVN_SUB_OFF\n"
     "LABEL            RVN_SUB_ON\n"
     "TIMED_COLOR_SET  0,0,0,0,0,0,1,1,300,0,0,0,0,0,0\n"
     "LABEL            RVN_SUB_SHOWN\n"
     "SLEEP            6\n"
     "BR_ON_ADJUSTMENT gameOptions,subtitles,RVN_SUB_SHOWN,0\n"
     "TIMED_COLOR_SET  0,0,0,0,0,0,1,0,0,0,0,0,0,0,0\n"
     "GOTO             RVN_SUB_OFF\n",
     nullptr},
};

bool ends_with(const char* s, const char* suffix) {
    size_t a = std::strlen(s), b = std::strlen(suffix);
    return a >= b && std::memcmp(s + a - b, suffix, b) == 0;
}

// "Hide the other ODSTs' JOIN NOW panels" (hide_p2_join): each seat's "JOIN NOW / Press START" panel (scripts number
// the seats 1-4, the gun slots 0-3), with the ODST's plaque and join movie, stays on screen while nobody plays it.
// Hide every seat but player 1's ODST. Display only: joining is handled in code (0x4269e0).
bool hidden_join_panel(const char* path) {
    if (!app::config().hide_p2_join) return false;
    static const char* const kFiles[] = {"moneyin_game/CashPlay_P%d_Msg.txt", "moneyin_game/join_plaq_p%d.txt",
                                         "moneyin_game/join_in_p%d_movie.txt", "moneyin_game/NoStart_P%d_Msg.txt"};
    for (int slot = 0; slot < kMaxPlayers; slot++) {
        if (slot == player_slot(0)) continue;
        for (const char* f : kFiles) {
            char name[64];
            std::snprintf(name, sizeof name, f, slot + 1);
            if (ends_with(path, name)) return true;
        }
    }
    return false;
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

std::string crlf(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch == '\n') out += '\r';
        out += ch;
    }
    return out;
}

// Dump and edit a freshly loaded file (both loaders).
void after_load(const char* path, char** buf, uint32_t* size, AllocFn alloc, FreeFn free, int terminate) {
    dump(path, *buf, *size);
    std::string text;
    bool changed = false;
    if (hidden_join_panel(path)) {
        text.assign(*buf, std::strlen(*buf) < *size ? std::strlen(*buf) : *size);
        size_t at = text.find("#-EndHeader");
        if (at != std::string::npos) text.replace(at, std::strlen("#-EndHeader"), KILL_AT_START), changed = true;
    }
    for (auto& e : kEdits) {
        if (e.enabled && !(app::config().*e.enabled)) continue;
        bool any = e.file[0] == '*';
        if (any ? !std::strstr(path, e.file + 1) : !ends_with(path, e.file)) continue;
        if (!changed) text.assign(*buf, std::strlen(*buf) < *size ? std::strlen(*buf) : *size);
        // Multi-line edits are written with \n; some scripts have \r\n line ends.
        std::string find = e.find, replace = e.replace;
        size_t at = text.find(find);
        if (at == std::string::npos && find.find('\n') != std::string::npos) {
            find = crlf(find), replace = crlf(replace);
            at = text.find(find);
        }
        if (at == std::string::npos) {
            if (!any) RLOG("script patch: \"%s\" not found in %s", e.find, path);
            continue;
        }
        text.replace(at, find.size(), replace);
        changed = true;
    }
    if (!changed) return;
    uint32_t pad = terminate ? 2 : 0;
    auto out = static_cast<char*>(alloc(text.size() + pad));
    std::memcpy(out, text.data(), text.size());
    std::memset(out + text.size(), 0, pad);
    free(*buf);
    *buf = out;
    *size = uint32_t(text.size() + pad);
    RLOG("script patch: edited %s", path);
}

SYSV int h_archive_load(const char* type, const char* path, char** buf, uint32_t* size, AllocFn alloc, FreeFn free,
                        int terminate) {
    bool fresh = buf && !*buf;
    int r = g_original(type, path, buf, size, alloc, free, terminate);
    RTRACE("archive %s: %s (%u bytes) = %d", type ? type : "?", path ? path : "?", size ? *size : 0, r);
    if (r == 0 && fresh && path && *buf && size) after_load(path, buf, size, alloc, free, terminate);
    return r;
}

// The other encrypted-file loader (0x774b00, used by FileLoad 0x4e65f0 for files listed in eFileMaps, such as the
// operator adjustment definitions in data/prod/adj, and some scripts such as the movie subtitle lines). Same arguments
// as ArchiveLoad except the first, an int.
using FileMapLoad = SYSV int (*)(int, const char*, char**, uint32_t*, AllocFn, FreeFn, int);
constexpr uint64_t kFileMapLoad = 0x774b00;
FileMapLoad g_filemap_original = nullptr;

SYSV int h_filemap_load(int kind, const char* path, char** buf, uint32_t* size, AllocFn alloc, FreeFn free,
                        int terminate) {
    bool fresh = buf && !*buf;
    int r = g_filemap_original(kind, path, buf, size, alloc, free, terminate);
    if (r == 0 && fresh && path && *buf && size && *size) after_load(path, buf, size, alloc, free, terminate);
    return r;
}

// The RTDT text parser (0x910c00): int (void* parser, const char* text). Dumps every text it parses to rtdt/NNNNN.txt
// (the data type headers, e.g. the AdjDef struct). Wrapped only for RAVEN_DUMP_SCRIPTS.
using RtdtParse = SYSV int (*)(void*, const char*);
RtdtParse g_parse_original = nullptr;
SYSV int h_rtdt_parse(void* parser, const char* text) {
    static int n = 0;
    if (const char* dir = std::getenv("RAVEN_DUMP_SCRIPTS"); dir && text) {
        std::filesystem::path out = std::filesystem::path(dir) / "rtdt";
        std::error_code ec;
        std::filesystem::create_directories(out, ec);
        char name[32];
        std::snprintf(name, sizeof name, "%05d.txt", n++);
        if (FILE* f = _wfopen((out / name).c_str(), L"wb")) {
            std::fwrite(text, 1, std::strlen(text), f);
            std::fclose(f);
        }
    }
    return g_parse_original(parser, text);
}

// RTDT object loader (0x4e6800): int (const char* kind, const char* path, const char* type, void* out). Logs every
// AdjDef it loads (struct in the type headers: name[64], version, 6 flag bytes, then int32 fields from +76): the
// operator adjustment definitions, whose files are encrypted. Wrapped only for RAVEN_DUMP_SCRIPTS.
using RtdtLoad = SYSV int (*)(const char*, const char*, const char*, void*);
RtdtLoad g_load_original = nullptr;
SYSV int h_rtdt_load(const char* kind, const char* path, const char* type, void* out) {
    int r = g_load_original(kind, path, type, out);
    if (r < 0 || !type || !out || std::strcmp(type, "AdjDef") != 0) return r;
    auto b = static_cast<const uint8_t*>(out);
    auto i = [&](int off) { int32_t v; std::memcpy(&v, b + off, 4); return v; };
    RLOG("adjdef %s | name=%.64s ver=%d flags=%d%d%d%d%d%d admin=%d def=%d min=%d max=%d step=%d div=%d "
         "time=%d date=%d cur=%d frac=%d real=%d pct=%d prio=%d lut=%.64s snd=%.64s",
         path ? path : "?", (const char*)b, i(64), b[68], b[69], b[70], b[71], b[72], b[73], i(76), i(80), i(84),
         i(88), i(92), i(96), i(100), i(104), i(108), i(112), i(116), i(120), i(124), (const char*)b + 128,
         (const char*)b + 192);
    return r;
}
}  // namespace

void install_script_patches() {
    // push r15; push r14; mov r10, rsi; push r13; push r12; mov r13, rcx; push rbp; push rbx (16 bytes)
    g_filemap_original = reinterpret_cast<FileMapLoad>(make_trampoline(kFileMapLoad, 16));
    hook_jump(kFileMapLoad, reinterpret_cast<const void*>(&h_filemap_load));
    if (std::getenv("RAVEN_DUMP_SCRIPTS")) {
        // push r15..rbx; mov rbx, rsi; sub rsp, 0x88 (20 bytes)
        g_parse_original = reinterpret_cast<RtdtParse>(make_trampoline(0x910c00, 20));
        hook_jump(0x910c00, reinterpret_cast<const void*>(&h_rtdt_parse));
        // push r14; push r13; mov r14, rdi; push r12; push rbp; mov r13, rsi; push rbx; mov rsi, rdi (17 bytes)
        g_load_original = reinterpret_cast<RtdtLoad>(make_trampoline(0x4e6800, 17));
        hook_jump(0x4e6800, reinterpret_cast<const void*>(&h_rtdt_load));
    }
    // push r15; push r14; mov r15, rcx; push r13; push r12; mov ecx, 0x80 (16 bytes, position-independent)
    g_original = reinterpret_cast<ArchiveLoad>(make_trampoline(kArchiveLoad, 16));
    hook_jump(kArchiveLoad, reinterpret_cast<const void*>(&h_archive_load));
}
}  // namespace raven::game
