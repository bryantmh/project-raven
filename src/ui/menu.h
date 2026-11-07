#pragma once
// The recomp settings menu (Dear ImGui over the game's GL context). Select/Back on a controller or F1 opens it; the
// game is paused while it is open. Settings live in app::Config and are saved on every change.
union SDL_Event;
namespace raven::ui {
void request_open();
bool is_open();
// Called by the SDL bridge for every real event; true when the menu used it (the game must not see it).
bool on_event(const SDL_Event& e);
// Called by the SDL bridge just before the game's frame is presented. Runs the menu (pausing the game) if requested.
void before_present();
}  // namespace raven::ui
