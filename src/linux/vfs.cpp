// Linux path -> Windows path. See README.md (game folder overlay).
//
//   /pm/g7/halo/...   the game's halo/ folder (cabinet install path; also the working directory)
//   /pm/g7/dump/...   <game>/dump (read by halo.so)
//   /tmp/...          <save>/tmp
//   anything else     <save>/root/... (so /etc, /var writes land somewhere harmless)
//
// Reads look in the overlay first (<save>/overlay/<path under the game folder>), then the game folder. Writes under
// the game folder always go to the overlay, so the game folder is never modified.
#include <windows.h>

#include <filesystem>
#include <mutex>
#include <string>

#include "app/paths.h"
#include "linux/lnx.h"
#include "log.h"

namespace fs = std::filesystem;

namespace raven::lnx {
namespace {
std::mutex g_cwd_mutex;
std::string g_cwd = "/pm/g7/halo";

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Collapse . and .. and duplicate slashes.
std::string normalize(const std::string& p) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string seg = p.substr(i, j - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        i = j + 1;
    }
    std::string out;
    for (auto& s : parts) out += "/" + s;
    return out.empty() ? "/" : out;
}

bool starts_with(const std::string& s, const char* prefix, std::string* rest) {
    size_t n = std::strlen(prefix);
    if (s.compare(0, n, prefix) != 0) return false;
    if (s.size() > n && s[n] != '/') return false;
    if (rest) *rest = s.size() > n ? s.substr(n + 1) : std::string();
    return true;
}
}  // namespace

std::string game_root() { return app::paths().game_halo_dir; }

std::string absolute_linux_path(const char* path) {
    std::string p = path ? path : "";
    if (p.empty() || p[0] != '/') {
        std::lock_guard<std::mutex> lk(g_cwd_mutex);
        p = g_cwd + "/" + p;
    }
    return normalize(p);
}

void set_cwd(const std::string& linux_abs) {
    std::lock_guard<std::mutex> lk(g_cwd_mutex);
    g_cwd = linux_abs;
}
std::string get_cwd() {
    std::lock_guard<std::mutex> lk(g_cwd_mutex);
    return g_cwd;
}

std::wstring map_path(const char* linux_path, bool for_write) {
    const auto& P = app::paths();
    std::string p = absolute_linux_path(linux_path), rest;
    fs::path base, overlay;
    if (starts_with(p, "/pm/g7/halo", &rest)) {
        base = fs::path(widen(P.game_halo_dir)) / widen(rest);
        overlay = fs::path(widen(P.save_dir)) / L"overlay" / widen(rest);
    } else if (starts_with(p, "/pm/g7/dump", &rest)) {
        base = fs::path(widen(P.game_dir)) / L"dump" / widen(rest);
        overlay = fs::path(widen(P.save_dir)) / L"overlay_dump" / widen(rest);
    } else if (starts_with(p, "/tmp", &rest)) {
        base = overlay = fs::path(widen(P.save_dir)) / L"tmp" / widen(rest);
    } else {
        base = overlay = fs::path(widen(P.save_dir)) / L"root" / widen(p.substr(1));
    }
    base = base.lexically_normal();
    overlay = overlay.lexically_normal();
    if (for_write) {
        std::error_code ec;
        fs::create_directories(overlay.parent_path(), ec);
        // Copy-on-write: a file opened for update keeps its contents.
        if (overlay != base && !fs::exists(overlay, ec) && fs::is_regular_file(base, ec))
            fs::copy_file(base, overlay, ec);
        return overlay.wstring();
    }
    std::error_code ec;
    if (overlay != base && fs::exists(overlay, ec)) return overlay.wstring();
    return base.wstring();
}

// The overlay twin of a directory under the game folder (for merging directory listings); empty if none.
std::wstring overlay_dir(const char* linux_path) {
    const auto& P = app::paths();
    std::string p = absolute_linux_path(linux_path), rest;
    if (starts_with(p, "/pm/g7/halo", &rest)) return (fs::path(widen(P.save_dir)) / L"overlay" / widen(rest)).wstring();
    return {};
}

}  // namespace raven::lnx
