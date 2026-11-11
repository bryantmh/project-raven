#pragma once
// Players, guns and ODSTs (Settings > Gameplay > Players, Controls > ODST). docs/input.md#players-and-guns
//
// Each gun slot (0-3) is one ODST: the cabinet seat decides the character. The 2-player cabinets use slots 0 and 3;
// with "Up to 4 players" the game is told it runs on the 4-player single-screen cabinet (type 3), which uses all four.
namespace raven::game {
constexpr int kMaxPlayers = 4;

// At startup (before the game runs): the cabinet type and the gun enables.
void install_players();

// Players this session supports (2 or 4; four_players is read once at startup).
int player_count();
// The gun slots this session's cabinet uses, in seat order (2 or 4 entries).
int slot_count();
int slot_at(int seat);
// The ODST each gun slot plays: "Ethan Graves", "Marcus Hudson", "Victor Ramos", "Ava Lang".
const char* odst_name(int slot);
// The ODST's color (JOIN NOW panel, HUD, crosshair): "blue", "orange", "green", "purple".
const char* odst_color(int slot);
// The gun slot player p (0-based) plays, from the ODST setting; -1 if none.
int player_slot(int player);
}  // namespace raven::game
