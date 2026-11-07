#pragma once
namespace raven::game {
// Keep the 2D UI unstretched on wide windows (src/game/ui_aspect.cpp).
void install_ui_aspect();
// For glUniformMatrix4fv: writes an adjusted copy of a 2D element's matrix to `out` and returns true, or false.
// In the menus (game::in_mission() false) the UI is laid out as a centered 16:9 area (both UI layout modes), and the menus'
// crosshair is laid out with it so it stays on what the game highlights. Returns how much that squeezes the aim on
// screen (> 1) and on which axis (0 = x, 1 = y), or 1 when nothing is squeezed (gameplay, stretched UI, 16:9 window).
float menu_aim_squeeze(int* axis);
bool ui_adjust_matrix(int location, int count, unsigned char transpose, const float* m, float* out);
}  // namespace raven::game
