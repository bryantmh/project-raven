#pragma once
// The game's named script triggers (src/game/triggers.cpp).
namespace raven::game {
// How many times the trigger has been set so far (a change means it fired), or -1 if it doesn't exist (yet).
int trigger_set_count(const char* name);
// Fire a trigger, as a script's SETTRIGGER does (tests).
void set_trigger(const char* name);
}  // namespace raven::game
