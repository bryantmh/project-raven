#pragma once
// Gameplay settings: difficulty, subtitles, game volume. docs/functions.md#operator-adjustments
namespace raven::game {
// At startup (before the game runs): the stored adjustments and the difficulty hook.
void install_gameplay();
// From the settings menu (game thread): apply at once.
void apply_subtitles();
void apply_game_volume();
}  // namespace raven::game
