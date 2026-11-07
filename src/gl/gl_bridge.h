#pragma once
#include <windows.h>
#include <GL/gl.h>
#include <GL/glu.h>

namespace raven::gl {
// Fill the thunk table from the current context (call once a context is current).
void load_entry_points();
// The driver's entry point for a GL command (Microsoft ABI), or null.
void* real(const char* name);
// Redirect the game's window framebuffer to one of w x h (call once the context is current).
void create_game_framebuffer(int w, int h);
// The frame's shape in the window: 0 fills the window, else letterbox/pillarbox to that aspect ratio.
void set_view_aspect(float aspect);
// Change the game framebuffer's size (its contents are undefined until the next frame).
void resize_game_framebuffer(int w, int h);
// Scale the game's frame into the window's back buffer (letterboxed), leaving framebuffer 0 bound for the swap.
void present_game_framebuffer(int win_w, int win_h);
// After the swap: bind the game's framebuffer again where the game had "framebuffer 0".
void rebind_game_framebuffer();
// Where the game's frame sits in a win_w x win_h window.
void game_rect_in(int win_w, int win_h, int* x, int* y, int* w, int* h);
// The 2D texture the game last bound on texture unit 0.
unsigned bound_texture0();
// The SysV thunk for a GL command, or null if the driver lacks it.
void* thunk_for(const char* name);
}  // namespace raven::gl
