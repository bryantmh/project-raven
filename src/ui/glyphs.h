#pragma once
// Button icons for the Controls tab, drawn with ImGui shapes (no image assets): Xbox or PlayStation face buttons,
// bumpers, triggers, sticks, D-pad, Start / Back, keyboard keycaps and mouse buttons.
#include <imgui.h>

#include "input/bindings.h"

namespace raven::ui {
enum class PadStyle { Xbox, PlayStation };
// Width the glyph takes at height h.
float glyph_width(const input::Bind& b, float h, PadStyle style);
// Draw at the top-left of a h-tall box; returns the width used.
float draw_glyph(ImDrawList* dl, ImVec2 pos, float h, const input::Bind& b, PadStyle style);
}  // namespace raven::ui
