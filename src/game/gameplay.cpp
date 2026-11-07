// Gameplay settings (Settings > Gameplay). docs/functions.md#operator-adjustments
//
// Difficulty: the arcade scales the damage a player takes with how long they've been playing, relative to
// minGameTime (135 s): 0.3x at first, 1x at 135 s, then up to 5x by about 170 s, which ends a credit. Until then
// health also can't drop far below a floor that falls from 1000 to 0 over that time: damage past it is multiplied by
// GAMETIME_DAMAGE_MOD. Player damage (0x419760) reads all of these from the player_health variable table with
// GetFloat (0x9247a0) on every hit. For a fixed difficulty we answer the four curve multipliers with one value
// (a flat curve) and GAMETIME_DAMAGE_MOD with 1 (no floor), so it applies at once.
//
// Subtitles and game volume are operator adjustments: written to the save overlay at startup, and set in the
// running game (Adj_SetValue) when changed in the menu. The subtitle line scripts are edited to keep checking the
// adjustment while a movie plays (src/game/script_patches.cpp); the volume also goes to the game's master volume
// setter.
#include "game/gameplay.h"

#include <cstdint>
#include <cstdlib>

#include "app/config.h"
#include "game/adjustments.h"
#include "game/hook.h"
#include "linux/shim.h"
#include "log.h"

namespace raven::game {
namespace {
// float GetFloat(VarTable* table, const char* key, float default)
using GetFloat = SYSV float (*)(void*, const char*, float);
constexpr uint64_t kGetFloat = 0x9247a0;
GetFloat g_get_float = nullptr;

// The keys' string addresses, as player damage passes them (values the game sets in brackets).
constexpr uint64_t kCurveKeys[] = {
    0x1138966,  // GAMETIME_DAMAGE_START (0.3)
    0x113897c,  // GAMETIME_DAMAGE_LOW (1.0)
    0x1138990,  // GAMETIME_DAMAGE_HIGH (2.5)
    0x11389a5,  // GAMETIME_DAMAGE_KILL (5.0)
};
constexpr uint64_t kFloorKey = 0x1138940;  // GAMETIME_DAMAGE_MOD
// Config::difficulty 1-4: Easy, Normal, Heroic, Legendary.
constexpr float kDamage[] = {0.5f, 1.0f, 2.0f, 3.0f};

SYSV float h_get_float(void* table, const char* key, float def) {
    int d = app::config().difficulty;
    if (d >= 1 && d <= 4) {
        auto k = reinterpret_cast<uint64_t>(key);
        for (uint64_t ck : kCurveKeys)
            if (k == ck) return kDamage[d - 1];
        if (k == kFloorKey) return 1.0f;
    }
    return g_get_float(table, key, def);
}

// RAVEN_DAMAGE_LOG=1 (tests): log every hit a player takes. PlayerDamaged(slot, damage) (0x419760); players at
// 0x1c0c7c0, stride 0x250, health at +0x14 (1000 when full).
using PlayerDamaged = SYSV int64_t (*)(int, int);
PlayerDamaged g_damaged = nullptr;
SYSV int64_t h_damaged(int slot, int damage) {
    auto health = [&] { return *reinterpret_cast<int32_t*>(0x1c0c7d4 + uint64_t(slot) * 0x250); };
    int before = slot >= 0 && slot <= 3 ? health() : 0;
    int64_t r = g_damaged(slot, damage);
    if (slot >= 0 && slot <= 3) RLOG("damage: player slot %d hit for %d, health %d -> %d", slot, damage, before, health());
    return r;
}

// int Adj_SetValue(const char* group, const char* name, int value): clamps to the definition's range.
using AdjSetValue = SYSV int (*)(const char*, const char*, int);
const auto adj_set_value = reinterpret_cast<AdjSetValue>(0x53f860);
// void SetGameVolume(float 0-100)
using SetVolume = SYSV void (*)(float);
const auto set_game_volume = reinterpret_cast<SetVolume>(0x59ed20);
}  // namespace

void install_gameplay() {
    auto& c = app::config();
    set_adjustment("gameOptions", "subtitles", c.subtitles ? 1 : 0);
    set_adjustment("sound", "gameVolume", c.game_volume);
    // push rbp; push rbx; sub rsp, 0x28; test rdi, rdi; movss [rsp+0xc], xmm0 (15 bytes, position-independent)
    g_get_float = reinterpret_cast<GetFloat>(make_trampoline(kGetFloat, 15));
    hook_jump(kGetFloat, reinterpret_cast<const void*>(&h_get_float));
    if (std::getenv("RAVEN_DAMAGE_LOG")) {
        // push r15; push r14; push r13; push r12; push rbp; push rbx; sub rsp, 0x78 (14 bytes)
        g_damaged = reinterpret_cast<PlayerDamaged>(make_trampoline(0x419760, 14));
        hook_jump(0x419760, reinterpret_cast<const void*>(&h_damaged));
    }
}

void apply_subtitles() { adj_set_value("gameOptions", "subtitles", app::config().subtitles ? 1 : 0); }

void apply_game_volume() {
    int v = app::config().game_volume;
    adj_set_value("sound", "gameVolume", v);
    set_game_volume(float(v));
}
}  // namespace raven::game
