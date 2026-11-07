#include "app/config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "app/paths.h"
#include "log.h"

namespace raven::app {
namespace {
Config g_config;

// Minimal flat-object JSON: {"key": "string" | number | true | false, ...}
std::map<std::string, std::string> parse_flat_json(const std::string& s) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    auto skip = [&] { while (i < s.size() && (isspace((unsigned char)s[i]) || s[i] == ',' || s[i] == '{' || s[i] == '}')) i++; };
    auto str = [&]() {
        std::string r;
        i++;  // opening quote
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char c = s[++i];
                r += c == 'n' ? '\n' : c == 't' ? '\t' : c;
            } else r += s[i];
            i++;
        }
        i++;
        return r;
    };
    for (;;) {
        skip();
        if (i >= s.size() || s[i] != '"') break;
        std::string k = str();
        while (i < s.size() && (isspace((unsigned char)s[i]) || s[i] == ':')) i++;
        std::string v;
        if (i < s.size() && s[i] == '"') v = str();
        else {
            while (i < s.size() && s[i] != ',' && s[i] != '}' && !isspace((unsigned char)s[i])) v += s[i++];
        }
        out[k] = v;
    }
    return out;
}
std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

// Every setting, for load and save. Strings, bools and ints.
struct Field {
    const char* key;
    std::string Config::*s = nullptr;
    bool Config::*b = nullptr;
    int Config::*i = nullptr;
};
const Field kFields[] = {
    {"version", nullptr, nullptr, &Config::version},
    {"game_folder", &Config::game_folder},
    {"fmod_dll", &Config::fmod_dll},
    {"free_play", nullptr, &Config::free_play},
    {"window_width", nullptr, nullptr, &Config::window_width},
    {"window_height", nullptr, nullptr, &Config::window_height},
    {"display_mode", nullptr, nullptr, &Config::display_mode},
    {"render_width", nullptr, nullptr, &Config::render_width},
    {"render_height", nullptr, nullptr, &Config::render_height},
    {"aspect", nullptr, nullptr, &Config::aspect},
    {"ui_layout", nullptr, nullptr, &Config::ui_layout},
    {"vsync", nullptr, &Config::vsync},
    {"fps_limit", nullptr, nullptr, &Config::fps_limit},
    {"hide_credit_text", nullptr, &Config::hide_credit_text},
    {"no_menu_countdown", nullptr, &Config::no_menu_countdown},
    {"skip_xbox_screen", nullptr, &Config::skip_xbox_screen},
    {"hide_qr_notices", nullptr, &Config::hide_qr_notices},
    {"boot_image", nullptr, &Config::boot_image},
    {"start_skips_movies", nullptr, &Config::start_skips_movies},
    {"movies_fill", nullptr, &Config::movies_fill},
    {"hide_p2_join", nullptr, &Config::hide_p2_join},
    {"rumble", nullptr, nullptr, &Config::rumble},
    {"aim_speed", nullptr, nullptr, &Config::aim_speed},
    {"aim_deadzone", nullptr, nullptr, &Config::aim_deadzone},
    {"kbm_own_player", nullptr, &Config::kbm_own_player},
    {"button_icons", nullptr, nullptr, &Config::button_icons},
};
}  // namespace

Config& config() { return g_config; }

void load_config() {
    std::ifstream f(to_wide(paths().save_dir + "\\config.json"));
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    auto kv = parse_flat_json(ss.str());
    int file_version = kv.count("version") ? std::atoi(kv["version"].c_str()) : 1;
    for (auto& fd : kFields) {
        auto it = kv.find(fd.key);
        if (it == kv.end()) continue;
        if (fd.s) g_config.*fd.s = it->second;
        else if (fd.b) g_config.*fd.b = it->second == "true";
        else if (fd.i) g_config.*fd.i = std::atoi(it->second.c_str());
    }
    for (auto& [k, v] : kv)
        if (k.rfind("pad_", 0) == 0 || k.rfind("key_", 0) == 0) g_config.binds[k] = v;
    // Migrations. 1 -> 2: presentation/display settings added (defaults apply). 2 -> 3: the UI layout default
    // changed from stretched to corners (version 2 only offered stretched as the safe choice).
    if (file_version < 3) g_config.ui_layout = 2;
    // 3 -> 4: "fullscreen" (borderless) became display_mode.
    if (file_version < 4) g_config.display_mode = kv["fullscreen"] == "true" ? 1 : 0;
    if (file_version < 4) RLOG("config: migrated version %d -> 4", file_version);
    g_config.version = 4;
    // RAVEN_TEST_RES=<w>x<h>: automated runs play windowed at this size and resolution (lighter on the machine).
    if (const char* r = std::getenv("RAVEN_TEST_RES")) {
        int w = 0, h = 0;
        if (std::sscanf(r, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            g_config.display_mode = 0;
            g_config.window_width = g_config.render_width = w;
            g_config.window_height = g_config.render_height = h;
            RLOG("config: test resolution %dx%d, windowed", w, h);
        }
    }
}

void save_config() {
    std::ofstream f(to_wide(paths().save_dir + "\\config.json"));
    f << "{\n";
    for (size_t n = 0; n < std::size(kFields); n++) {
        auto& fd = kFields[n];
        f << "  \"" << fd.key << "\": ";
        if (fd.s) f << '"' << escape(g_config.*fd.s) << '"';
        else if (fd.b) f << (g_config.*fd.b ? "true" : "false");
        else f << g_config.*fd.i;
        f << (n + 1 < std::size(kFields) || !g_config.binds.empty() ? ",\n" : "\n");
    }
    size_t m = 0;
    for (auto& [k, v] : g_config.binds)
        f << "  \"" << escape(k) << "\": \"" << escape(v) << '"' << (++m < g_config.binds.size() ? ",\n" : "\n");
    f << "}\n";
}
}  // namespace raven::app
