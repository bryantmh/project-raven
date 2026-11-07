#pragma once
// Controller input: SDL game controllers drive the game's PC input driver (docs/input.md).
union SDL_Event;
namespace raven::input {
// Called for every real SDL event before the game sees it. Returns true if the event was consumed.
bool on_sdl_event(const SDL_Event& e);
// Called once per frame on the game thread (after the game's swap).
void frame();
// Player p's aim (0..1 across the screen; players 0 and 1).
bool aim(int player, float* x, float* y);
// The aims written to the game in the last frames, newest first; returns how many.
int aim_history(int player, const float** xs, const float** ys);
// The controller player p uses ("" if none), and whether it is a PlayStation controller.
const char* controller_name(int player);
bool controller_is_playstation(int player);
// Close and reopen every controller (after the "keyboard & mouse is player 1" setting changes).
void reassign_controllers();
// Tests: press or release a button (SDL_GameControllerButton) on virtual controller n; false if none is attached.
bool set_virtual_button(int n, int button, bool down);
}  // namespace raven::input
