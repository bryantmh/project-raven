// 2D UI on wide screens (Settings > Display > UI layout). The game lays its 2D UI out on a 1920x1080 canvas and
// draws each element with one matrix, ortho camera x element transform, sent with glUniformMatrix4fv (its x scale
// is the element's scale / 960, its x translation the element's center in clip space). On a wider window that
// canvas is stretched. We fix each element as its matrix goes to GL (src/gl/gl_bridge.cpp):
//   1  16:9: undo the stretch about the screen center, so the canvas shows as a centered 16:9 area.
//   2  corners: undo the stretch about each element's own center, and move elements in the outer 20% of the
//      canvas out to the real screen edges (so HUD corners stay in the corners), the middle ones toward the center.
// 3D (perspective) and full-screen passes (unit quads scaled by 2) are left alone.
#include "game/ui_aspect.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app/config.h"
#include "gl/gl_bridge.h"
#include "input/input.h"
#include "log.h"
#include "sdl/sdl_bridge.h"
#include "game/view_aspect.h"

namespace raven::game {
namespace {
int g_mode = 0;
float g_k = 1.0f;  // window aspect / (16/9)

// The current viewport's aspect over 16:9 (the UI pass draws to the full window; shadow and other passes don't).
// The game's current aspect (window shape) over 16:9, for full-frame passes; 1 (leave alone) for other passes.
float viewport_k() {
    static auto get_int = reinterpret_cast<void(__stdcall*)(unsigned, int*)>(gl::real("glGetIntegerv"));
    int vp[4] = {}, gw = 0, gh = 0;
    get_int(0x0BA2 /*GL_VIEWPORT*/, vp);
    sdl::game_size(&gw, &gh);
    if (vp[2] != gw || vp[3] != gh) return 1.0f;
    return *reinterpret_cast<const float*>(0x1cc1fac) / (16.0f / 9.0f);
}
}  // namespace

float menu_aim_squeeze(int* axis) {
    *axis = 0;
    if (!app::config().ui_layout || in_mission()) return 1.0f;
    float k = *reinterpret_cast<const float*>(0x1cc1fac) / (16.0f / 9.0f);
    if (std::fabs(k - 1.0f) < 0.01f || k <= 0) return 1.0f;
    *axis = k > 1 ? 0 : 1;
    return k > 1 ? k : 1.0f / k;
}

void install_ui_aspect() {
    g_mode = app::config().ui_layout;
    if (g_mode) RLOG("ui layout: %s", g_mode == 1 ? "16:9" : "corners");
}

namespace {
// A crosshair is recognized by its texture and its shape (y scale / x scale), not its size: the game pulses
// crosshairs while firing, so the size changes every frame.
struct Element {
    unsigned tex;
    float shape;
    float x0, y0;       // where first seen
    float travel = 0;   // furthest from there: crosshairs move with the aim, static HUD doesn't
    int hits = 0;       // consecutive draws on the recent aim path while it moves
    bool crosshair = false;
};
std::vector<Element> g_elements;

bool is_crosshair(const float* m) {
    unsigned tex = gl::bound_texture0();
    if (!tex || m[0] == 0) return false;
    float shape = m[5] / m[0];
    Element* e = nullptr;
    for (auto& k : g_elements)
        if (k.tex == tex && std::fabs(k.shape - shape) < 0.01f * std::fabs(shape)) e = &k;
    if (!e) {
        if (g_elements.size() > 1024) g_elements.clear();
        g_elements.push_back({tex, shape, m[12], m[13]});
        e = &g_elements.back();
    }
    if (e->crosshair) return true;
    e->travel = std::max(e->travel, std::fabs(m[12] - e->x0) + std::fabs(m[13] - e->y0));

    const float *hx, *hy;
    int n = input::aim_history(&hx, &hy);
    if (n < 16) return false;
    auto learn = [&](const char* how) {
        e->crosshair = true;
        RLOG("ui layout: crosshair texture %u (shape %.3f), %s", tex, shape, how);
        return true;
    };
    bool resting = true;
    for (int i = 1; i < n; i++) resting &= hx[i] == hx[0] && hy[i] == hy[0];
    if (resting) {
        // Aim at rest away from the screen center (where static UI sits), an element that has moved before and now
        // sits on it (the game's smoothing settles within ~1/1000 of the screen).
        float cx = 2 * hx[0] - 1, cy = 1 - 2 * hy[0];
        if ((std::fabs(cx) > 0.05f || std::fabs(cy) > 0.05f) && e->travel > 0.02f &&
            std::fabs(m[12] - cx) < 1e-3f && std::fabs(m[13] - cy) < 1e-3f)
            return learn("at rest");
        return false;
    }
    // Aim moving: an element that stays on the recent aim path (within the game's smoothing) for 5 draws while
    // having travelled with it.
    float best = 1e9f;
    for (int i = 0; i < 10 && i < n; i++)
        best = std::min(best, std::fabs(m[12] - (2 * hx[i] - 1)) + std::fabs(m[13] - (1 - 2 * hy[i])));
    if (best >= 0.003f) e->hits = 0;
    else if (++e->hits >= 5 && e->travel > 0.02f) return learn("following the aim");
    return false;
}
}  // namespace

bool adjust(unsigned char transpose, const float* m, float* out);

// RAVEN_XHAIRDEBUG=1 logs every element drawn near the aim: where the game put it and where it ends up.
bool ui_adjust_matrix(int, int, unsigned char transpose, const float* m, float* out) {
    bool changed = adjust(transpose, m, out);
    static bool debug = std::getenv("RAVEN_XHAIRDEBUG") != nullptr;
    if (debug && !transpose) {
        const float *hx, *hy;
        int n = input::aim_history(&hx, &hy);
        if (n > 0 && std::fabs(m[12] - (2 * hx[0] - 1)) < 0.15f && std::fabs(m[13] - (1 - 2 * hy[0])) < 0.15f)
            RLOG("xhair: tex %u s %.6f game %.4f %.4f -> drawn %.4f %.4f | aim %.4f %.4f", gl::bound_texture0(), m[0],
                 m[12], m[13], changed ? out[12] : m[12], changed ? out[13] : m[13], 2 * hx[0] - 1, 1 - 2 * hy[0]);
    }
    return changed;
}

bool adjust(unsigned char transpose, const float* m, float* out) {
    g_mode = app::config().ui_layout;  // live: the settings menu can change it mid-game
    if (!g_mode || transpose) return false;
    // Column-major. An element matrix: affine (no perspective row), pixel-scaled (|x scale| ~ 1/960, far from the
    // 2.0 of full-screen unit quads), with a y scale too.
    if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) return false;
    float sx = std::sqrt(m[0] * m[0] + m[1] * m[1]);
    if (sx == 0 || sx > 0.05f || std::fabs(m[5]) + std::fabs(m[4]) == 0) return false;
    float k = viewport_k();
    if (std::fabs(k - 1.0f) < 0.01f) return false;
    // In gameplay crosshairs follow the guns' aim, which the game maps across the whole 3D view: they must stay where
    // the game draws them, on what the shot hits (is_crosshair says why they're recognized by texture). In the 2D
    // menus the game hit-tests the aim against its unadjusted layout, so there the crosshair is laid out like every
    // other element: drawn over the same item the game highlights. The menus use the centered 16:9 layout in both
    // modes: Corners moves elements past 60% of the width outward, which would make a menu crosshair jump there and
    // pull menu items apart from their labels.
    bool menu = !in_mission();
    if (is_crosshair(m) && !menu) return false;
    int mode = menu ? 1 : g_mode;
    // Wider than 16:9 the canvas is stretched sideways: fix x. Narrower (k < 1) it is stretched vertically: fix y.
    // The same rule either way, on that axis (row 0 or row 1 of the column-major matrix).
    int row = k > 1 ? 0 : 1;
    float f = k > 1 ? k : 1.0f / k;
    std::memcpy(out, m, sizeof(float) * 16);
    for (int i : {0, 4, 8}) out[i + row] = m[i + row] / f;
    float t = m[12 + row];
    if (mode == 1) {
        out[12 + row] = t / f;
    } else {
        // Element centers in the outer 20% of the canvas keep their distance to the nearest screen edge; the rest
        // keep their distance to the center. Both measured on the unstretched (16:9) canvas.
        float a = std::fabs(t);
        float edge = 0.6f;
        float moved = a > edge ? 1.0f - (1.0f - a) / f : a / f;
        out[12 + row] = t < 0 ? -moved : moved;
    }
    return true;
}
}  // namespace raven::game
