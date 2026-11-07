#pragma once
// Controller input: SDL game controllers drive the game's PC input driver (docs/input.md).
union SDL_Event;
namespace raven::input {
// Called for every real SDL event before the game sees it. Returns true if the event was consumed.
bool on_sdl_event(const SDL_Event& e);
// Called once per frame on the game thread (after the game's swap).
void frame();
// Player p's aim (0..1 across the screen); false if that player has no aim of its own.
bool aim(int player, float* x, float* y);
// The aims written to the game in the last frames, newest first; returns how many.
int aim_history(const float** xs, const float** ys);
}  // namespace raven::input
