// 2D UI on wide screens (Settings > Display > UI layout). The game lays its 2D UI out on a 1920x1080 canvas and
// draws each element with one matrix, ortho camera x element transform, sent with glUniformMatrix4fv (its x scale
// is the element's scale / 960, its x translation the element's center in clip space). On a wider window that
// canvas is stretched. We fix each element as its matrix goes to GL (src/gl/gl_bridge.cpp):
//   1  16:9: undo the stretch about the screen center, so the canvas shows as a centered 16:9 area.
//   2  corners: undo the stretch about each element's own center, and move elements in the outer 20% of the
//      canvas (the outer quarters on the 4-player cabinet, one ODST's HUD each) out to the real screen edges (so HUD
//      corners stay in the corners), the middle ones toward the center.
// 3D (perspective) and full-screen passes (unit quads scaled by 2) are left alone.
#include "game/ui_aspect.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "app/config.h"
#include "gl/gl_bridge.h"
#include "input/input.h"
#include "log.h"
#include "sdl/sdl_bridge.h"
#include "game/players.h"
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
// A crosshair is recognized by its texture and the part of it the mesh shows, not its size: the game pulses
// crosshairs while firing, so the size changes every frame. The HUD shares one texture atlas between the crosshair
// and its icons (gun, grenade, bullets), so the texture alone isn't enough. Each piece is a quad drawn from client
// memory whose vertex pointer (attribute 0: position xyz, then texture uv) is already set when its matrix arrives;
// the quad's uv rectangle names the sprite, the same in every buffer, level and menu.
uintptr_t current_mesh() {
    static auto get = reinterpret_cast<void(__stdcall*)(unsigned, unsigned, void**)>(gl::real("glGetVertexAttribPointerv"));
    void* p = nullptr;
    if (get) get(0, 0x8645 /*GL_VERTEX_ATTRIB_ARRAY_POINTER*/, &p);
    return reinterpret_cast<uintptr_t>(p);
}
// The vertices the current draw uses (set by ui_before_draw from the draw call): [lo, hi].
int g_draw_lo = 0, g_draw_hi = 3;

// The mesh's horizontal extent on screen (clip space), from the positions of the vertices this draw uses; false if
// unknown. Positions come from client memory, or, for meshes in a vertex buffer, are read back once per buffer,
// offset and range (those meshes don't change).
bool current_extent_x(const float* m, float* x0, float* x1, bool* canvas_quad, float* y0 = nullptr, float* y1 = nullptr) {
    static auto geti = reinterpret_cast<void(__stdcall*)(unsigned, unsigned, int*)>(gl::real("glGetVertexAttribiv"));
    static auto get_sub = reinterpret_cast<void(__stdcall*)(unsigned, intptr_t, intptr_t, void*)>(gl::real("glGetBufferSubData"));
    static auto bind = reinterpret_cast<void(__stdcall*)(unsigned, unsigned)>(gl::real("glBindBuffer"));
    static std::unordered_map<uint64_t, std::vector<float>> buffered;
    uintptr_t p = current_mesh();
    int stride = 0, buffer = 0;
    if (!geti) return false;
    geti(0, 0x8624 /*STRIDE*/, &stride);
    geti(0, 0x889F /*BUFFER_BINDING*/, &buffer);
    if (stride == 0) stride = 12;
    int lo = g_draw_lo, n = g_draw_hi - g_draw_lo + 1;
    if (stride < 12 || stride > 256 || lo < 0 || n < 1 || n > 1024) return false;
    std::vector<float> pos;
    if (buffer) {
        uint64_t key = (uint64_t(buffer) << 44) ^ (uint64_t(p) << 20) ^ (uint64_t(lo) << 10) ^ uint64_t(n) ^ (uint64_t(stride) << 56);
        auto it = buffered.find(key);
        if (it == buffered.end()) {
            if (!get_sub || !bind || buffered.size() > 4096) return false;
            std::vector<uint8_t> raw(size_t(stride) * (n - 1) + 12);
            bind(0x8F36 /*COPY_READ_BUFFER*/, unsigned(buffer));
            get_sub(0x8F36, intptr_t(p + uintptr_t(lo) * stride), intptr_t(raw.size()), raw.data());
            bind(0x8F36, 0);
            std::vector<float> v(size_t(n) * 3);
            for (int i = 0; i < n; i++) std::memcpy(&v[size_t(i) * 3], &raw[size_t(i) * stride], 12);
            it = buffered.emplace(key, std::move(v)).first;
        }
        pos = it->second;
    } else {
        if (p < 0x10000) return false;
        pos.resize(size_t(n) * 3);
        for (int i = 0; i < n; i++) std::memcpy(&pos[size_t(i) * 3], reinterpret_cast<const void*>(p + uintptr_t(lo + i) * stride), 12);
    }
    *x0 = 1e9f, *x1 = -1e9f;
    float ylo = 1e9f, yhi = -1e9f;
    int corners = 0;
    for (int i = 0; i < n; i++) {
        const float* v = &pos[size_t(i) * 3];
        float x = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12];
        float y = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13];
        *x0 = std::min(*x0, x), *x1 = std::max(*x1, x);
        ylo = std::min(ylo, y), yhi = std::max(yhi, y);
        corners += std::fabs(std::fabs(x) - 1) < 0.002f && std::fabs(std::fabs(y) - 1) < 0.002f;
    }
    if (canvas_quad) *canvas_quad = n == 4 && corners == 4;
    if (y0) *y0 = ylo;
    if (y1) *y1 = yhi;
    return true;
}

// The vertex range an indexed draw uses: from its indices (client memory or the element buffer; cached per buffer).
void index_range(int count, unsigned type, const void* indices, int* lo, int* hi) {
    static auto geti = reinterpret_cast<void(__stdcall*)(unsigned, int*)>(gl::real("glGetIntegerv"));
    static auto get_sub = reinterpret_cast<void(__stdcall*)(unsigned, intptr_t, intptr_t, void*)>(gl::real("glGetBufferSubData"));
    static std::unordered_map<uint64_t, std::pair<int, int>> cache;
    int size = type == 0x1401 ? 1 : type == 0x1403 ? 2 : 4;  // UNSIGNED_BYTE / SHORT / INT
    *lo = 0, *hi = 3;
    if (count <= 0 || count > 4096) return;
    int ebo = 0;
    geti(0x8895 /*ELEMENT_ARRAY_BUFFER_BINDING*/, &ebo);
    std::vector<uint8_t> raw(size_t(count) * size);
    uint64_t key = (uint64_t(ebo) << 40) ^ (uint64_t(reinterpret_cast<uintptr_t>(indices)) << 12) ^ uint64_t(count) ^ (uint64_t(size) << 60);
    if (ebo) {
        auto it = cache.find(key);
        if (it != cache.end()) {
            *lo = it->second.first, *hi = it->second.second;
            return;
        }
        if (!get_sub) return;
        get_sub(0x8893 /*ELEMENT_ARRAY_BUFFER*/, intptr_t(reinterpret_cast<uintptr_t>(indices)), intptr_t(raw.size()), raw.data());
    } else {
        if (reinterpret_cast<uintptr_t>(indices) < 0x10000) return;
        std::memcpy(raw.data(), indices, raw.size());
    }
    int a = INT32_MAX, b = 0;
    for (int i = 0; i < count; i++) {
        uint32_t v = size == 1 ? raw[size_t(i)] : size == 2 ? uint32_t(reinterpret_cast<const uint16_t*>(raw.data())[i])
                                                           : reinterpret_cast<const uint32_t*>(raw.data())[i];
        a = std::min(a, int(v)), b = std::max(b, int(v));
    }
    *lo = a, *hi = b;
    if (ebo && cache.size() < 8192) cache[key] = {a, b};
}

// The sprite's uv rectangle, packed into a key (0 if unknown): from the first enabled 2-component vertex attribute
// in client memory (the texture coordinates; interleaved after the position or in an array of their own).
uint64_t current_sprite() {
    static auto geti = reinterpret_cast<void(__stdcall*)(unsigned, unsigned, int*)>(gl::real("glGetVertexAttribiv"));
    static auto getp = reinterpret_cast<void(__stdcall*)(unsigned, unsigned, void**)>(gl::real("glGetVertexAttribPointerv"));
    if (!geti || !getp) return 0;
    for (unsigned attr = 1; attr < 6; attr++) {
        int enabled = 0, size = 0, stride = 0, buffer = 0, type = 0;
        geti(attr, 0x8622 /*ENABLED*/, &enabled);
        if (!enabled) continue;
        geti(attr, 0x8623 /*SIZE*/, &size);
        geti(attr, 0x8625 /*TYPE*/, &type);
        geti(attr, 0x8624 /*STRIDE*/, &stride);
        geti(attr, 0x889F /*BUFFER_BINDING*/, &buffer);
        void* ptr = nullptr;
        getp(attr, 0x8645 /*POINTER*/, &ptr);
        auto p = reinterpret_cast<uintptr_t>(ptr);
        if (size != 2 || type != 0x1406 /*FLOAT*/ || buffer || p < 0x10000) continue;
        if (stride == 0) stride = 8;
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (int i = 0; i < 4; i++) {
            auto uv = reinterpret_cast<const float*>(p + uintptr_t(i) * stride);
            u0 = std::min(u0, uv[0]), u1 = std::max(u1, uv[0]), v0 = std::min(v0, uv[1]), v1 = std::max(v1, uv[1]);
        }
        auto q = [](float f) { return uint64_t(uint16_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 65535))); };
        return q(u0) | q(v0) << 16 | q(u1) << 32 | q(v1) << 48;
    }
    return 0;
}
struct Element {
    unsigned tex;
    uint64_t sprite;
    float x0, y0;       // where first seen
    float travel = 0;   // furthest from there: crosshairs move with the aim, static HUD doesn't
    int hits[4] = {};   // per player: consecutive draws on the recent aim path while it moves
    bool crosshair = false;
};
std::vector<Element> g_elements;

bool is_crosshair(const float* m) {
    unsigned tex = gl::bound_texture0();
    if (!tex || m[0] == 0) return false;
    uint64_t sprite = current_sprite();
    if (!sprite) return false;
    Element* e = nullptr;
    for (auto& k : g_elements)
        if (k.tex == tex && k.sprite == sprite) e = &k;
    if (!e) {
        if (g_elements.size() > 1024) g_elements.clear();
        g_elements.push_back({tex, sprite, m[12], m[13]});
        e = &g_elements.back();
    }
    if (e->crosshair) return true;
    e->travel = std::max(e->travel, std::fabs(m[12] - e->x0) + std::fabs(m[13] - e->y0));

    auto learn = [&](const char* how, int player) {
        e->crosshair = true;
        RLOG("ui layout: crosshair texture %u sprite %016llx, %s (player %d)", tex, (unsigned long long)sprite, how, player + 1);
        return true;
    };
    // Each player's gun has its own aim (and crosshair).
    for (int player = 0; player < game::kMaxPlayers; player++) {
    const float *hx, *hy;
    int n = input::aim_history(player, &hx, &hy);
    if (n < 16) continue;
    bool resting = true;
    for (int i = 1; i < n; i++) resting &= hx[i] == hx[0] && hy[i] == hy[0];
    if (resting) {
        // Aim at rest away from the screen center (where static UI sits): the sprite drawn on it (the game's
        // smoothing settles within ~1/1000 of the screen).
        float cx = 2 * hx[0] - 1, cy = 1 - 2 * hy[0];
        if ((std::fabs(cx) > 0.05f || std::fabs(cy) > 0.05f) && std::fabs(m[12] - cx) < 1e-3f && std::fabs(m[13] - cy) < 1e-3f)
            return learn("at rest", player);
        continue;
    }
    // Aim moving: an element that stays on the recent aim path (within the game's smoothing) for 5 draws while
    // having travelled with it.
    float best = 1e9f;
    for (int i = 0; i < 10 && i < n; i++)
        best = std::min(best, std::fabs(m[12] - (2 * hx[i] - 1)) + std::fabs(m[13] - (1 - 2 * hy[i])));
    if (best >= 0.003f) e->hits[player] = 0;
    else if (++e->hits[player] >= 5 && e->travel > 0.02f) return learn("following the aim", player);
    }
    return false;
}
}  // namespace

bool adjust(unsigned char transpose, const float* m, float* out);

// RAVEN_XHAIRDEBUG=1 logs every element drawn near the aim: where the game put it and where it ends up.
// The last element matrix we adjusted, until the next one: its draw decides (draw_hook) whether the element is a
// crosshair or a screen overlay, which need the mesh and texture of that draw. The game sets an element's vertex
// pointer only after its matrix, so they can't be read at upload time.
struct Pending {
    bool active = false, restored = false;
    int location = 0;
    float original[16], adjusted[16];
} g_pending;

void upload(int location, const float* m) {
    static auto f = reinterpret_cast<void(__stdcall*)(int, int, unsigned char, const float*)>(gl::real("glUniformMatrix4fv"));
    f(location, 1, 0, m);
}

bool ui_adjust_matrix(int location, int, unsigned char transpose, const float* m, float* out) {
    bool changed = adjust(transpose, m, out);
    g_pending.active = changed;
    if (changed) {
        g_pending.location = location;
        std::memcpy(g_pending.original, m, sizeof(float) * 16);
        std::memcpy(g_pending.adjusted, out, sizeof(float) * 16);
    }
    // RAVEN_BIGDEBUG=1 logs each large 2D element (wider than ~1/20 of the canvas) once per texture and size.
    static bool big = std::getenv("RAVEN_BIGDEBUG") != nullptr;
    if (big && !transpose && m[15] == 1 && m[3] == 0 && std::fabs(m[0]) > 0.05f) {
        static std::vector<std::array<float, 5>> seen;
        std::array<float, 5> k{float(gl::bound_texture0()), m[0], m[5], m[12], m[13]};
        bool dup = false;
        for (auto& x : seen) dup |= x == k;
        if (!dup && seen.size() < 400) {
            seen.push_back(k);
            int vp[4] = {};
            reinterpret_cast<void(__stdcall*)(unsigned, int*)>(gl::real("glGetIntegerv"))(0x0BA2, vp);
            RLOG("bigdebug: tex %u m0 %.4f m5 %.4f t %.4f %.4f vp %d %d %d %d changed %d", gl::bound_texture0(), m[0], m[5],
                 m[12], m[13], vp[0], vp[1], vp[2], vp[3], int(changed));
        }
    }
    return changed;
}

bool ui_before_draw(int first, int count, unsigned index_type, const void* indices) {
    g_pending.restored = false;
    if (!g_pending.active) return false;
    if (index_type) index_range(count, index_type, indices, &g_draw_lo, &g_draw_hi);
    else g_draw_lo = first, g_draw_hi = first + count - 1;
    const float* m = g_pending.original;
    // Screen overlays (the turret view frame in Mission 1, ...) span the whole canvas: they stretch to the screen like
    // the game draws them, in every layout: both edges at or a little past the screen's. A single quad exactly the
    // size of the canvas isn't one: it's a layer of canvas content drawn into a texture first (the mission select's
    // highlight frame) or a movie plane (filled by gl_bridge's draw_movie_scaled), laid out like other elements.
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bool canvas_quad = false;
    bool known = current_extent_x(m, &x0, &x1, &canvas_quad, &y0, &y1);
    bool overlay = known && x0 <= -0.97f && x1 >= 0.97f && !canvas_quad;
    // In missions, a quad as tall as the canvas is a screen overlay for part of the screen (each player's damage
    // tint over their quarter or half): stretched like the game draws it, so the parts still tile the screen.
    overlay |= known && in_mission() && y0 <= -0.97f && y1 >= 0.97f && x1 - x0 < 1.9f;
    // In gameplay crosshairs follow the guns' aim, which the game maps across the whole 3D view: they must stay where
    // the game draws them, on what the shot hits (is_crosshair says how they're recognized). In the menus they are laid
    // out like everything else (see adjust).
    bool crosshair = is_crosshair(m) && in_mission();
    // RAVEN_UIDEBUG=1 logs each element once per texture, sprite and position, with what was done to it.
    static bool uidebug = std::getenv("RAVEN_UIDEBUG") != nullptr;
    if (uidebug) {
        static std::vector<std::array<int64_t, 4>> seen;
        std::array<int64_t, 4> k{int64_t(gl::bound_texture0()), int64_t(current_sprite()), std::lround(m[12] * 4), std::lround(m[13] * 4)};
        bool dup = false;
        for (auto& x : seen) dup |= x == k;
        if (!dup && seen.size() < 50000) {
            seen.push_back(k);
            RLOG("uidebug: tex %u sprite %016llx s %.6f t %.4f %.4f ext %.3f %.3f y %.3f %.3f -> %s", gl::bound_texture0(),
                 (unsigned long long)current_sprite(), m[0], m[12], m[13], x0, x1, y0, y1,
                 overlay ? "overlay (stretched)" : crosshair ? "crosshair (as drawn)" : "laid out");
        }
    }
    static bool debug = std::getenv("RAVEN_XHAIRDEBUG") != nullptr;
    if (debug) {
        const float* drawn = overlay || crosshair ? m : g_pending.adjusted;
        for (int player = 0; player < game::kMaxPlayers; player++) {
            const float *hx, *hy;
            int n = input::aim_history(player, &hx, &hy);
            if (n > 0 && std::fabs(m[12] - (2 * hx[0] - 1)) < 0.15f && std::fabs(m[13] - (1 - 2 * hy[0])) < 0.15f)
                RLOG("xhair: tex %u s %.6f game %.4f %.4f -> drawn %.4f %.4f | aim %.4f %.4f p%d sprite %016llx", gl::bound_texture0(),
                     m[0], m[12], m[13], drawn[12], drawn[13], 2 * hx[0] - 1, 1 - 2 * hy[0], player + 1,
                     (unsigned long long)current_sprite());
        }
    }
    if (!overlay && !crosshair) return false;
    upload(g_pending.location, m);
    g_pending.restored = true;
    return true;
}

void ui_after_draw() {
    if (g_pending.restored) upload(g_pending.location, g_pending.adjusted);
    g_pending.restored = false;
}

bool adjust(unsigned char transpose, const float* m, float* out) {
    g_mode = app::config().ui_layout;  // live: the settings menu can change it mid-game
    if (!g_mode || transpose) return false;
    // Column-major. An element matrix: affine (no perspective row), pixel-scaled (|x scale| ~ 1/960, far from the
    // 2.0 of full-screen unit quads), with a y scale too.
    if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) return false;
    float sx = std::sqrt(m[0] * m[0] + m[1] * m[1]);
    if (sx == 0 || sx > 0.05f || std::fabs(m[5]) + std::fabs(m[4]) == 0) return false;
    if (std::fabs(m[12]) > 1.5f || std::fabs(m[13]) > 1.5f) return false;  // not on screen: a 3D object's matrix
    float k = viewport_k();
    if (std::fabs(k - 1.0f) < 0.01f) return false;
    // In the 2D menus the game hit-tests the aim against its unadjusted layout, so there the crosshair is laid out
    // like every other element: drawn over the same item the game highlights. (In missions it is drawn where the game
    // puts it: ui_before_draw.) The menus use the centered 16:9 layout in both modes: Corners moves elements past 60%
    // of the width outward, which would make a menu crosshair jump there and pull menu items apart from their labels.
    bool menu = !in_mission();
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
        // On the 4-player cabinet each ODST's HUD fills a quarter of the canvas: the outer quarters go to the edges
        // whole (a narrower band would leave Graves's and Lang's kill counters behind), the inner two stay centered.
        float a = std::fabs(t);
        float edge = game::player_count() == 4 ? 0.5f : 0.6f;
        float moved = a > edge ? 1.0f - (1.0f - a) / f : a / f;
        out[12 + row] = t < 0 ? -moved : moved;
    }
    return true;
}
}  // namespace raven::game
