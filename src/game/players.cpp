// Players, guns and ODSTs. docs/input.md#players-and-guns
//
// The cabinet type comes from the dongle at startup: ReadCabinetType (0x639950, ctx, uint8_t* out) reads byte 0x29
// of its memory and main (0x437a20) stores it as SysInfoGameCabinetType, which every cabinet check reads (0x5864b0
// caches it). Types 4 and 8 are the 2-player cabinets (slots 0 and 3), 3 and 9 the 4-player ones (single screen and
// dual screen); docs/functions.md#cabinet-type. For "Up to 4 players" we answer the read with 3.
#include "game/players.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "app/config.h"
#include "game/adjustments.h"
#include "game/hook.h"
#include "linux/shim.h"
#include "log.h"

namespace raven::game {
namespace {
constexpr uint64_t kReadCabinetType = 0x639950;
constexpr int kFourPlayerCabinet = 3;  // "Super Deluxe": 4 guns, one 16:9 screen
using ReadCabinetType = SYSV int (*)(void*, uint8_t*);
ReadCabinetType g_read_cabinet_type = nullptr;
bool g_four = false;

SYSV int h_read_cabinet_type(void* ctx, uint8_t* out) {
    int r = g_read_cabinet_type(ctx, out);
    RLOG("players: dongle cabinet type %d (read %d), reporting %d", out ? *out : -1, r, kFourPlayerCabinet);
    if (out) *out = kFourPlayerCabinet;
    return 0;
}

constexpr int kSlots2[] = {0, 3};
constexpr int kSlots4[] = {0, 1, 2, 3};
const char* const kOdsts[kMaxPlayers] = {"Ethan Graves", "Marcus Hudson", "Victor Ramos", "Ava Lang"};
}  // namespace

void install_players() {
    g_four = app::config().four_players;
    // Every gun enabled (operator adjustments hardware/p<n>gun; GunEnabled 0x4178d0 reads them). Off, a player
    // can't join: the game shows GUN<n>_DISABLED instead.
    for (int n = 1; n <= kMaxPlayers; n++) set_adjustment("hardware", "p" + std::to_string(n) + "gun", 1);
    if (!g_four) return;
    // push rbp; push rbx; mov rbp, rsi; mov eax, 3; sub rsp, 8 (14 bytes, position-independent)
    auto p = reinterpret_cast<const uint8_t*>(kReadCabinetType);
    if (p[0] != 0x55 || p[1] != 0x53 || p[10] != 0x48 || p[11] != 0x83) {
        RLOG("players: unexpected bytes at %#llx, staying on the 2-player cabinet", (unsigned long long)kReadCabinetType);
        g_four = false;
        return;
    }
    // Type 3 has IR camera guns: the I/O setup (0x436230) gives types 3 and 8 the IR tracker driver (I/O flag 8,
    // driver 0x16f8400, whose camera manager crashes with no cameras). Set it up like the user's mounted-gun
    // type 4 instead: `jle 0x436630` (types below 4 = 3) at 0x436415 becomes `jle 0x4365b8` (type 4's branch).
    auto io = reinterpret_cast<uint8_t*>(0x436415);
    const uint8_t want[6] = {0x0F, 0x8E, 0x15, 0x02, 0x00, 0x00};
    if (std::memcmp(io, want, 6) != 0) {
        RLOG("players: unexpected bytes at 0x436415, staying on the 2-player cabinet");
        g_four = false;
        return;
    }
    // The 4-player cabinet has two RIO I/O boards (P1/P2 and P3/P4). The board check (0x4adac0) halts boot on
    // "missing board(s)" when it expects more than one (0x6213f0); with one it skips the count, as on the 2-player
    // cabinet. `jbe` at 0x4adbc1 becomes `jmp`: always skip it.
    auto rio = reinterpret_cast<uint8_t*>(0x4adbc1);
    if (rio[0] != 0x76 || rio[1] != 0x0B) {
        RLOG("players: unexpected bytes at 0x4adbc1, staying on the 2-player cabinet");
        g_four = false;
        return;
    }
    io[2] = 0x9D, io[3] = 0x01;  // rel32 0x19d: 0x43641b + 0x19d = 0x4365b8
    rio[0] = 0xEB;
    g_read_cabinet_type = reinterpret_cast<ReadCabinetType>(make_trampoline(kReadCabinetType, 14));
    hook_jump(kReadCabinetType, reinterpret_cast<const void*>(&h_read_cabinet_type));
    RLOG("players: up to 4 players (cabinet type %d)", kFourPlayerCabinet);
}

int player_count() { return g_four ? 4 : 2; }
int slot_count() { return player_count(); }
int slot_at(int seat) {
    if (seat < 0 || seat >= slot_count()) return -1;
    return g_four ? kSlots4[seat] : kSlots2[seat];
}

const char* odst_name(int slot) { return slot >= 0 && slot < kMaxPlayers ? kOdsts[slot] : "?"; }
const char* odst_color(int slot) {
    static const char* const kColors[kMaxPlayers] = {"blue", "orange", "green", "purple"};
    return slot >= 0 && slot < kMaxPlayers ? kColors[slot] : "";
}

int player_slot(int player) {
    if (player < 0 || player >= player_count()) return -1;
    // Players pick in order: their chosen ODST if this cabinet has that slot and nobody before took it, else the
    // first free one.
    bool taken[kMaxPlayers] = {};
    int slot = -1;
    for (int p = 0; p <= player; p++) {
        int want = app::config().odst[p];
        bool ok = false;
        for (int s = 0; s < slot_count(); s++) ok |= slot_at(s) == want;
        if (!ok || want < 0 || want >= kMaxPlayers || taken[want]) {
            want = -1;
            for (int s = 0; s < slot_count() && want < 0; s++)
                if (!taken[slot_at(s)]) want = slot_at(s);
        }
        taken[want] = true;
        slot = want;
    }
    return slot;
}
}  // namespace raven::game
