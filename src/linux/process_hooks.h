#pragma once
// Hooks between the shim layer and the rest of the loader.
#include <cstdint>
#include <string>
#include <vector>

namespace raven::lnx {
void env_init(const std::vector<std::string>& vars);  // the game's initial environment
void on_game_exit(int code);                           // exit()/_exit() land here (src/main.cpp)
void precise_sleep_ns(int64_t ns);
}  // namespace raven::lnx
