#include "app/paths.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>

namespace raven::app {
static Paths g_paths;

std::string to_utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring to_wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

const Paths& paths() { return g_paths; }

void set_game_dir(const std::string& dir) {
    namespace fs = std::filesystem;
    fs::path p(to_wide(dir));
    // Accept either the folder holding halo/ or halo/ itself.
    if (p.filename() == L"halo" && fs::exists(p / L"game")) p = p.parent_path();
    g_paths.game_dir = to_utf8(p.wstring());
    g_paths.game_halo_dir = to_utf8((p / L"halo").wstring());
}

void init_paths() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    g_paths.exe_dir = to_utf8(std::filesystem::path(buf).parent_path().wstring());
    PWSTR appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        auto dir = std::filesystem::path(appdata) / L"ProjectRaven";
        std::filesystem::create_directories(dir);
        g_paths.save_dir = to_utf8(dir.wstring());
        CoTaskMemFree(appdata);
    }
}
}  // namespace raven::app
