// raven.exe: loads the Linux arcade build of Halo: Fireteam Raven and runs it. docs/architecture.md
#include <windows.h>
#include <shobjidl.h>
#include <timeapi.h>

#include <filesystem>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/paths.h"
#include "app/runtime_pack.h"
#include "input/bindings.h"
#include "elf/elf_loader.h"
#include "game/boot.h"
#include "game/patches.h"
#include "linux/process_hooks.h"
#include "log.h"
#include "sdl/sdl_bridge.h"

using namespace raven;
namespace raven { void install_crash_handler(); void sample_thread(HANDLE t, const char* label); }

namespace {
constexpr uint64_t kGameLo = 0x400000, kGameHi = 0x2400000;

// Reserve the game's address range before anything else in the process can land there.
struct EarlyReserve {
    void* p;
    EarlyReserve() { p = VirtualAlloc(reinterpret_cast<void*>(kGameLo), kGameHi - kGameLo, MEM_RESERVE, PAGE_NOACCESS); }
};
#pragma clang diagnostic ignored "-Wglobal-constructors"
__attribute__((init_priority(101))) EarlyReserve g_reserve;

std::string pick_folder() {
    std::string out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS);
    dlg->SetTitle(L"Select your Halo: Fireteam Raven folder (the one containing halo\\)");
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = app::to_utf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

// The game folder holds halo\ (the game) and dump\ (the cabinet's data files, read at boot as /pm/g7/dump; without
// them the game crashes while setting up its tables).
bool game_folder_ok(const std::string& dir) {
    namespace fs = std::filesystem;
    if (dir.empty()) return false;
    app::set_game_dir(dir);
    return fs::exists(app::to_wide(app::paths().game_halo_dir + "\\game")) &&
           fs::is_directory(app::to_wide(app::paths().game_dir + "\\dump"));
}

elf::Image* g_game = nullptr;

// Enter the game's _start on this thread's stack, laid out the way the Linux kernel leaves it.
extern "C" [[noreturn]] void raven_enter_start(uint64_t entry, uint64_t* sp);
__asm__(".text\n.globl raven_enter_start\nraven_enter_start:\n"
        "movq %rdx, %rsp\nxorl %edx, %edx\nxorl %ebp, %ebp\njmpq *%rcx\n");

DWORD WINAPI game_thread(void*) {
    static std::vector<std::string> args;
    auto& cfg = app::config();
    // Render size: the configured resolution, else the window (or the display when fullscreen). Aspect: the
    // game's -a<w>x<h> option (0x435560 -> 0x1749f8c), the render size's own shape unless 16:9 is forced.
    int rw = cfg.render_width, rh = cfg.render_height;
    if (rw <= 0 || rh <= 0) {
        // The display's size: the frame is scaled into the window, so this stays sharp at any window size.
        rw = GetSystemMetrics(SM_CXSCREEN);
        rh = GetSystemMetrics(SM_CYSCREEN);
    }
    args = {"/pm/g7/halo/game2", "-f" + std::to_string(rw) + "x" + std::to_string(rh), "-n"};
    args.push_back("-a" + std::to_string(rw) + "x" + std::to_string(rh));  // the live view aspect replaces it
    if (const char* extra = std::getenv("RAVEN_GAME_ARGS")) {  // testing: extra game options, space separated
        std::string s = extra;
        for (size_t i = 0; i < s.size();) {
            size_t j = s.find(' ', i);
            if (j == std::string::npos) j = s.size();
            if (j > i) args.push_back(s.substr(i, j - i));
            i = j + 1;
        }
    }
    {
        std::string line;
        for (auto& a : args) line += a + " ";
        RLOG("game args: %s", line.c_str());
    }
    static uint8_t random_bytes[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    // Frame on this thread's stack: argc, argv[], NULL, envp[] (empty: environ is set separately), NULL, auxv.
    uint64_t* frame = static_cast<uint64_t*>(_alloca(4096));
    frame = reinterpret_cast<uint64_t*>((reinterpret_cast<uintptr_t>(frame) + 15) & ~uintptr_t(15));
    size_t i = 0;
    frame[i++] = args.size();
    for (auto& a : args) frame[i++] = reinterpret_cast<uint64_t>(a.c_str());
    frame[i++] = 0;
    frame[i++] = 0;  // envp terminator
    frame[i++] = 6; frame[i++] = 4096;                                         // AT_PAGESZ
    frame[i++] = 25; frame[i++] = reinterpret_cast<uint64_t>(random_bytes);   // AT_RANDOM
    frame[i++] = 0; frame[i++] = 0;                                           // AT_NULL
    RLOG("entering game _start at %#llx", g_game->file.size() ? 0x414600ull : 0ull);
    raven_enter_start(0x414600, frame);
}
}  // namespace

namespace raven::lnx {
void on_game_exit(int code) {
    RLOG("game exited with code %d", code);
    ExitProcess(UINT(code));
}
}  // namespace raven::lnx

int main(int argc, char** argv) {
    // Real pixels everywhere (display size, window size): without this, Windows reports a scaled-down desktop on
    // high-DPI displays (3440x1440 at 125% looks like 2752x1152) and upscales the window.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!g_reserve.p) {
        // Something already took part of the range; try once more now that the CRT is up.
        g_reserve.p = VirtualAlloc(reinterpret_cast<void*>(kGameLo), kGameHi - kGameLo, MEM_RESERVE, PAGE_NOACCESS);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    install_crash_handler();
    app::init_paths();
    log_init((app::paths().save_dir + "\\raven.log").c_str());
    RLOG("Project Raven starting; save folder %s", app::paths().save_dir.c_str());
    if (!g_reserve.p) log_fatal("The game's memory range (0x400000-0x2400000) is already in use.");
    if (!app::unpack_runtime()) log_fatal("Project Raven couldn't unpack its runtime files into %%LOCALAPPDATA%%\\ProjectRaven.");

    app::load_config();
    input::load_binds();
    for (int i = 1; i + 1 < argc; i++)
        if (std::string(argv[i]) == "--game") app::config().game_folder = argv[++i];
    if (!game_folder_ok(app::config().game_folder)) {
        std::string dir = pick_folder();
        if (!game_folder_ok(dir))
            log_fatal("Select the Halo: Fireteam Raven folder: the one that contains both the halo and dump folders.");
        app::config().game_folder = app::paths().game_dir;
    }
    app::save_config();
    RLOG("game folder %s", app::paths().game_dir.c_str());

    timeBeginPeriod(1);
    const auto& P = app::paths();
    g_game = elf::load(P.game_halo_dir + "\\game", "game", true);
    elf::Image* halo_so = elf::load(P.game_halo_dir + "\\halo.so", "halo.so", false);
    elf::Image* libstdcxx = elf::load(P.runtime_dir + "\\runtime\\linux\\libstdc++.so.6", "libstdc++.so.6", false);
    elf::Image* libgcc = elf::load(P.runtime_dir + "\\runtime\\linux\\libgcc_s.so.1", "libgcc_s.so.1", false);

    elf::link_all();
    game::bind_embedded_imports(*g_game);

    DWORD slot = TlsAlloc();
    game::patch_stack_protector(*g_game, slot);
    game::patch_stack_protector(*halo_so, slot);
    sdl::install();
    game::apply_patches();

    lnx::env_init({"PATH=/usr/local/bin:/bin:/usr/bin", "HOME=/pm/g7/halo/env/home", "DISPLAY=:0", "LANG=C",
                   "SDL_VIDEODRIVER=dummy", "SDL_AUDIODRIVER=dummy"});
    elf::run_initializers(libgcc);
    elf::run_initializers(libstdcxx);
    elf::run_initializers(halo_so);
    RLOG("library initializers done");

    // Fully committed (see pthread_create in src/linux/pthread.cpp).
    HANDLE t = CreateThread(nullptr, 64u << 20, game_thread, nullptr, 0, nullptr);
    // RAVEN_SAMPLE=<seconds>: log where the game thread is every few seconds (diagnosing hangs).
    if (const char* smp = std::getenv("RAVEN_SAMPLE")) {
        int every = std::max(1, std::atoi(smp));
        while (WaitForSingleObject(t, every * 1000) == WAIT_TIMEOUT) {
            sample_thread(t, "game");
            RLOG("sample: frame limit %llu, last %llu", *reinterpret_cast<unsigned long long*>(0x1cc2730), *reinterpret_cast<unsigned long long*>(0x177e7f0));
        }
    }
    WaitForSingleObject(t, INFINITE);
    return 0;
}

namespace raven::app {
// The folder dialog, for the settings menu's "Change game folder".
std::string pick_folder_dialog() { return pick_folder(); }
}  // namespace raven::app
