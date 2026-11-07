#pragma once
namespace raven::game {
// Make 3D cameras follow the window aspect live (src/game/view_aspect.cpp).
void install_view_aspect();
// True in a mission (the last main scene started is level_<n>), false in the menus (attract, game and mission
// select). From the game's scene manager.
bool in_mission();
// True once the game's scene manager has started a scene (its tables, triggers included, are set up by then).
bool scenes_started();
}  // namespace raven::game
