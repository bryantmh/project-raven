// Button icons drawn with ImGui shapes. See glyphs.h.
#include "ui/glyphs.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace raven::ui {
namespace {
using input::Bind;
constexpr ImU32 kBody = IM_COL32(38, 42, 50, 255), kEdge = IM_COL32(150, 158, 172, 255),
                kText = IM_COL32(235, 238, 245, 255), kLit = IM_COL32(90, 170, 255, 255);

ImU32 rgb(int r, int g, int b) { return IM_COL32(r, g, b, 255); }

void text_centered(ImDrawList* dl, ImVec2 c, float size, ImU32 col, const char* s) {
    ImFont* f = ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, s);
    dl->AddText(f, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, s);
}
float text_width(float size, const char* s) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, s).x; }

// A round face button: Xbox letters in their colors, PlayStation shapes.
void face(ImDrawList* dl, ImVec2 c, float r, int button, PadStyle style) {
    dl->AddCircleFilled(c, r, kBody, 32);
    dl->AddCircle(c, r, kEdge, 32, r * 0.08f);
    if (style == PadStyle::Xbox) {
        static const char* letters[] = {"A", "B", "X", "Y"};
        static const ImU32 colors[] = {rgb(96, 200, 72), rgb(230, 70, 60), rgb(60, 140, 240), rgb(240, 200, 40)};
        text_centered(dl, c, r * 1.3f, colors[button], letters[button]);
        return;
    }
    float s = r * 0.48f, t = r * 0.13f;
    switch (button) {
        case 0:  // cross
            dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), rgb(120, 170, 255), t);
            dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), rgb(120, 170, 255), t);
            break;
        case 1:  // circle
            dl->AddCircle(c, s, rgb(255, 100, 110), 24, t);
            break;
        case 2:  // square
            dl->AddRect(ImVec2(c.x - s * 0.9f, c.y - s * 0.9f), ImVec2(c.x + s * 0.9f, c.y + s * 0.9f), rgb(240, 130, 210), 0, 0, t);
            break;
        default:  // triangle
            dl->AddTriangle(ImVec2(c.x, c.y - s), ImVec2(c.x + s, c.y + s * 0.75f), ImVec2(c.x - s, c.y + s * 0.75f),
                            rgb(80, 210, 170), t);
            break;
    }
}

// Shoulder (bumper) or trigger: a rounded tab labelled LB / RT / L1 / R2.
void shoulder(ImDrawList* dl, ImVec2 p, float w, float h, const char* label, bool trigger) {
    float top = trigger ? 0.0f : h * 0.2f;
    ImVec2 a(p.x, p.y + top), b(p.x + w, p.y + h);
    dl->AddRectFilled(a, b, kBody, h * (trigger ? 0.35f : 0.25f), trigger ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);
    dl->AddRect(a, b, kEdge, h * (trigger ? 0.35f : 0.25f), trigger ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll, h * 0.06f);
    text_centered(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), h * 0.55f, kText, label);
}

// D-pad with one arm lit (0 up, 1 down, 2 left, 3 right).
void dpad(ImDrawList* dl, ImVec2 c, float r, int dir) {
    float a = r * 0.36f;
    auto arm = [&](int d, ImVec2 p0, ImVec2 p1) {
        dl->AddRectFilled(p0, p1, d == dir ? kLit : kBody, a * 0.3f);
        dl->AddRect(p0, p1, kEdge, a * 0.3f, 0, r * 0.06f);
    };
    arm(0, ImVec2(c.x - a, c.y - r), ImVec2(c.x + a, c.y - a));
    arm(1, ImVec2(c.x - a, c.y + a), ImVec2(c.x + a, c.y + r));
    arm(2, ImVec2(c.x - r, c.y - a), ImVec2(c.x - a, c.y + a));
    arm(3, ImVec2(c.x + a, c.y - a), ImVec2(c.x + r, c.y + a));
    dl->AddRectFilled(ImVec2(c.x - a, c.y - a), ImVec2(c.x + a, c.y + a), kBody);
}

// A small pill (Start / Back / Share / Guide / Touchpad / paddles) with a short label or symbol.
void pill(ImDrawList* dl, ImVec2 p, float w, float h, const char* label) {
    ImVec2 a(p.x, p.y + h * 0.18f), b(p.x + w, p.y + h * 0.82f);
    dl->AddRectFilled(a, b, kBody, h);
    dl->AddRect(a, b, kEdge, h, 0, h * 0.06f);
    text_centered(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), h * 0.42f, kText, label);
}

// Start / Back as the controllers draw them: a small pill with the button's symbol.
// Xbox Menu and PS5 Options: three lines. Xbox View: two overlapping windows. PS5 Create: three short rays.
enum class Symbol { Lines, Windows, Rays };
void symbol_button(ImDrawList* dl, ImVec2 p, float w, float h, Symbol s) {
    ImVec2 a(p.x, p.y + h * 0.14f), b(p.x + w, p.y + h * 0.86f);
    float r = (b.y - a.y) * 0.5f, t = h * 0.07f;
    dl->AddRectFilled(a, b, kBody, r);
    dl->AddRect(a, b, kEdge, r, 0, h * 0.06f);
    ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    float u = h * 0.16f;
    switch (s) {
        case Symbol::Lines:
            for (int i = -1; i <= 1; i++) dl->AddLine(ImVec2(c.x - u * 1.3f, c.y + i * u), ImVec2(c.x + u * 1.3f, c.y + i * u), kText, t);
            break;
        case Symbol::Windows:
            dl->AddRect(ImVec2(c.x - u * 1.5f, c.y - u * 1.1f), ImVec2(c.x + u * 0.5f, c.y + u * 0.5f), kText, u * 0.2f, 0, t);
            dl->AddRectFilled(ImVec2(c.x - u * 0.5f, c.y - u * 0.5f), ImVec2(c.x + u * 1.5f, c.y + u * 1.1f), kBody, u * 0.2f);
            dl->AddRect(ImVec2(c.x - u * 0.5f, c.y - u * 0.5f), ImVec2(c.x + u * 1.5f, c.y + u * 1.1f), kText, u * 0.2f, 0, t);
            break;
        case Symbol::Rays:
            // Three strokes fanning up from a point, as on the PS5 Create button.
            for (float ang : {-0.62f, 0.0f, 0.62f}) {
                ImVec2 d(std::sin(ang), -std::cos(ang));
                dl->AddLine(ImVec2(c.x + d.x * u * 0.45f, c.y + u * 0.9f + d.y * u * 0.45f),
                            ImVec2(c.x + d.x * u * 1.9f, c.y + u * 0.9f + d.y * u * 1.9f), kText, t);
            }
            break;
    }
}

// Keyboard keycap with the key's name.
float keycap_width(const char* name, float h) { return std::max(h, text_width(h * 0.5f, name) + h * 0.55f); }
void keycap(ImDrawList* dl, ImVec2 p, float h, const char* name) {
    float w = keycap_width(name, h);
    ImVec2 a(p.x, p.y + h * 0.04f), b(p.x + w, p.y + h * 0.96f);
    dl->AddRectFilled(ImVec2(a.x, a.y + h * 0.06f), ImVec2(b.x, b.y), rgb(20, 22, 27), h * 0.18f);
    dl->AddRectFilled(a, ImVec2(b.x, b.y - h * 0.08f), rgb(225, 228, 235), h * 0.18f);
    ImFont* f = ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(h * 0.5f, FLT_MAX, 0, name);
    dl->AddText(f, h * 0.5f, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - h * 0.08f - ts.y) * 0.5f), rgb(25, 28, 35), name);
}

// Mouse with the bound button lit (1 left, 2 middle, 3 right, 4/5 side).
void mouse(ImDrawList* dl, ImVec2 p, float h, int button) {
    float w = h * 0.68f;
    ImVec2 a(p.x, p.y), b(p.x + w, p.y + h);
    float midx = (a.x + b.x) * 0.5f, split = a.y + h * 0.45f;
    dl->AddRectFilled(a, b, kBody, w * 0.5f);
    if (button == 1) dl->AddRectFilled(a, ImVec2(midx, split), kLit, w * 0.5f, ImDrawFlags_RoundCornersTopLeft);
    if (button == 3) dl->AddRectFilled(ImVec2(midx, a.y), ImVec2(b.x, split), kLit, w * 0.5f, ImDrawFlags_RoundCornersTopRight);
    dl->AddRect(a, b, kEdge, w * 0.5f, 0, h * 0.05f);
    dl->AddLine(ImVec2(a.x, split), ImVec2(b.x, split), kEdge, h * 0.04f);
    dl->AddLine(ImVec2(midx, a.y), ImVec2(midx, split), kEdge, h * 0.04f);
    ImVec2 w0(midx - w * 0.09f, a.y + h * 0.13f), w1(midx + w * 0.09f, a.y + h * 0.32f);
    dl->AddRectFilled(w0, w1, button == 2 ? kLit : kEdge, w * 0.08f);
    if (button >= 4) text_centered(dl, ImVec2(midx, a.y + h * 0.72f), h * 0.36f, kLit, button == 4 ? "4" : "5");
}
}  // namespace

float glyph_width(const Bind& b, float h, PadStyle style) {
    switch (b.kind) {
        case Bind::PadButton:
            if (b.code <= SDL_CONTROLLER_BUTTON_Y || b.code == SDL_CONTROLLER_BUTTON_LEFTSTICK ||
                b.code == SDL_CONTROLLER_BUTTON_RIGHTSTICK || (b.code >= SDL_CONTROLLER_BUTTON_DPAD_UP && b.code <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
                return h;
            if (b.code == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || b.code == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) return h * 1.5f;
            if (b.code == SDL_CONTROLLER_BUTTON_START || b.code == SDL_CONTROLLER_BUTTON_BACK) return h * 1.2f;
            return h * 1.7f;
        case Bind::PadTrigger: return h * 1.3f;
        case Bind::Key: return keycap_width(SDL_GetScancodeName(SDL_Scancode(b.code)), h);
        case Bind::Mouse: return h * 0.68f;
        default: return 0;
    }
}

float draw_glyph(ImDrawList* dl, ImVec2 p, float h, const Bind& b, PadStyle style) {
    bool ps = style == PadStyle::PlayStation;
    float w = glyph_width(b, h, style);
    ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
    switch (b.kind) {
        case Bind::PadButton:
            switch (b.code) {
                case SDL_CONTROLLER_BUTTON_A:
                case SDL_CONTROLLER_BUTTON_B:
                case SDL_CONTROLLER_BUTTON_X:
                case SDL_CONTROLLER_BUTTON_Y: face(dl, c, h * 0.46f, b.code, style); break;
                case SDL_CONTROLLER_BUTTON_LEFTSTICK:
                case SDL_CONTROLLER_BUTTON_RIGHTSTICK: {
                    dl->AddCircleFilled(c, h * 0.46f, kBody, 32);
                    dl->AddCircle(c, h * 0.46f, kEdge, 32, h * 0.05f);
                    dl->AddCircle(c, h * 0.3f, kEdge, 32, h * 0.04f);
                    bool left = b.code == SDL_CONTROLLER_BUTTON_LEFTSTICK;
                    text_centered(dl, c, h * 0.42f, kText, ps ? (left ? "L3" : "R3") : (left ? "LS" : "RS"));
                    break;
                }
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: shoulder(dl, p, w, h, ps ? "L1" : "LB", false); break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: shoulder(dl, p, w, h, ps ? "R1" : "RB", false); break;
                case SDL_CONTROLLER_BUTTON_DPAD_UP:
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dpad(dl, c, h * 0.46f, b.code - SDL_CONTROLLER_BUTTON_DPAD_UP); break;
                case SDL_CONTROLLER_BUTTON_START: symbol_button(dl, p, w, h, Symbol::Lines); break;
                case SDL_CONTROLLER_BUTTON_BACK: symbol_button(dl, p, w, h, ps ? Symbol::Rays : Symbol::Windows); break;
                case SDL_CONTROLLER_BUTTON_GUIDE: pill(dl, p, w, h, ps ? "PS" : "GUIDE"); break;
                case SDL_CONTROLLER_BUTTON_MISC1: pill(dl, p, w, h, ps ? "MUTE" : "SHARE"); break;
                case SDL_CONTROLLER_BUTTON_TOUCHPAD: pill(dl, p, w, h, "TOUCH"); break;
                default: {
                    std::string label = "P" + std::to_string(b.code - SDL_CONTROLLER_BUTTON_PADDLE1 + 1);
                    pill(dl, p, w, h, label.c_str());
                }
            }
            break;
        case Bind::PadTrigger: shoulder(dl, p, w, h, ps ? (b.code ? "R2" : "L2") : (b.code ? "RT" : "LT"), true); break;
        case Bind::Key: keycap(dl, p, h, SDL_GetScancodeName(SDL_Scancode(b.code))); break;
        case Bind::Mouse: mouse(dl, p, h, b.code); break;
        default: break;
    }
    return w;
}
}  // namespace raven::ui
