#pragma once
#include <string>
namespace raven::game {
// Write an operator adjustment into the save overlay (the game reads it at startup).
void set_adjustment(const std::string& group, const std::string& name, int value);
}
