#pragma once
#include <cstdint>
struct SDL_Window;
union SDL_Event;
namespace raven::sdl {
void install();  // hook the game's SDL entry points (before the game starts)
SDL_Window* window();
void game_size(int* w, int* h);       // the game's render size
void present();                       // swap and pump events (for screens we draw ourselves)
void present_menu_frame();            // swap only (the settings menu polls its own events)
void apply_display_mode();
void reapply_view_aspect();           // after the Aspect ratio setting changes (applies on the next frame)             // windowed / borderless / exclusive, from the config (at once)
void window_to_game(int x, int y, float* gx, float* gy);  // window point -> 0..1 across the game's frame
void request_shots(int n);             // save the next n frames (RAVEN_SHOT; the test script's "shot:N")
void push_event(const SDL_Event& e);  // queue an event for the game's PC input driver
}  // namespace raven::sdl
