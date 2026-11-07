#pragma once
// Where things live. All UTF-8.
#include <string>

namespace raven::app {
struct Paths {
    std::string exe_dir;        // folder of raven.exe
    std::string runtime_dir;    // the unpacked runtime files: linux/, windows/, assets/ (src/app/runtime_pack.cpp)
    std::string save_dir;       // %APPDATA%\ProjectRaven: config.json, raven.log, overlay/, tmp/
    std::string game_dir;       // the folder the user pointed at (holds halo/ and dump/)
    std::string game_halo_dir;  // game_dir\halo
};
const Paths& paths();
void set_game_dir(const std::string& dir);  // also derives game_halo_dir
void init_paths();
void set_runtime_dir(const std::string& dir);
std::string to_utf8(const std::wstring& w);
std::wstring to_wide(const std::string& s);
}  // namespace raven::app
