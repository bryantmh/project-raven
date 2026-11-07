// The 3D view follows the window's shape (Hor+: the vertical field of view stays as designed, wider windows see
// more on the sides).
//
// Perspective cameras (flags +0x88 bit 1 clear) are a lens model: focal length +0xc0, film back width +0xb8 and
// height +0xbc. CameraUpdate (0x492670) derives the angles (+0x8c horizontal, +0x90 vertical, tangents +0xa8/+0xac)
// and the projection from them every frame (tan(h/2) = width / 2 / (focal / 10)). The game's scene cameras have a
// 16:9 film back (4.2721 x 2.4) whatever the screen, so the frame was always a 16:9 view stretched to the window.
// We hook CameraUpdate and, for every screen camera (film back 1.78:1, or a shape we set), make the film back the
// current view aspect (0x1cc1fac, set by sdl_bridge from the window and the Aspect ratio setting) by its height:
// the vertical angle stays, the horizontal one follows the window. Other cameras (shadow maps 1:1, render-to-
// texture views) are left alone. Measured with RAVEN_PROJDEBUG: P11 / P00 equals the window's aspect.
#include "game/view_aspect.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "game/hook.h"
#include "linux/shim.h"
#include "log.h"

namespace raven::game {
namespace {
constexpr uint64_t kCameraUpdate = 0x492670, kAspect = 0x1cc1fac;
using CameraUpdate = SYSV void (*)(uint8_t*, int);
CameraUpdate g_update = nullptr;
std::vector<float> g_screen_aspects = {1.78f, 16.0f / 9.0f};  // aspects that mean "the screen"

bool is_screen_aspect(float a) {
    for (float s : g_screen_aspects)
        if (std::fabs(a - s) < 0.002f) return true;
    return false;
}

// Scene manager: 0x494fa0 starts a scene (rdi: the scene, its name at +0). Many scenes run at once (hud_2d,
// transitions, enemies...); the main ones say where the game is: level_<n> is a mission (also the attract demo),
// attract_2d / game_select2 / chaptersel are the menus. Scenes are rarely ended, so the last main one started wins.
using SceneFn = SYSV void (*)(char*, int);
SceneFn g_scene_start = nullptr;
bool g_in_level = false, g_scenes_started = false;
SYSV void h_scene_start(char* scene, int arg) {
    g_scenes_started = true;
    if (scene) {
        static bool debug = std::getenv("RAVEN_SCENEDEBUG") != nullptr;
        if (debug) RLOG("scene: start %s (%d)", scene, arg);
        auto starts = [&](const char* p) { return std::strncmp(scene, p, std::strlen(p)) == 0; };
        bool level = starts("level_");
        if (level || starts("attract_2d") || starts("game_select") || starts("chaptersel")) {
            if (level != g_in_level) RLOG("scene: %s (%s)", level ? "mission" : "menus", scene);
            g_in_level = level;
        }
    }
    g_scene_start(scene, arg);
}

SYSV void h_camera_update(uint8_t* cam, int arg) {
    uint32_t flags = *reinterpret_cast<uint32_t*>(cam + 0x88);
    auto f = [&](int off) -> float& { return *reinterpret_cast<float*>(cam + off); };
    if (!(flags & 2) && f(0xbc) > 0) {
        float now = *reinterpret_cast<const float*>(kAspect);
        if (now > 0.1f && !is_screen_aspect(now)) g_screen_aspects.push_back(now);
        if (now > 0.1f && is_screen_aspect(f(0xb8) / f(0xbc))) f(0xb8) = f(0xbc) * now;
    }
    g_update(cam, arg);
    static bool debug = std::getenv("RAVEN_PROJDEBUG") != nullptr;
    if (debug && !(flags & 2) && f(0x90) > 0.5f) {
        static float last = 0;
        if (f(0x94) != last) {
            last = f(0x94);
            const float* p = reinterpret_cast<const float*>(cam + 0x170);
            RLOG("projdebug: +0xa0..0xc4: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f", f(0xa0), f(0xa4), f(0xa8),
                 f(0xac), f(0xb0), f(0xb4), f(0xb8), f(0xbc), f(0xc0), f(0xc4));
            RLOG("projdebug: cam %p aspect %.3f vfov %.3f hfov %.3f -> P00 %.4f P11 %.4f (P11/P00 %.3f)", cam, f(0x94),
                 f(0x90), f(0x8c), p[0], p[5], p[0] != 0 ? p[5] / p[0] : 0.0f);
        }
    }
}
}  // namespace

bool in_mission() { return g_in_level; }
bool scenes_started() { return g_scenes_started; }

void install_view_aspect() {
    // push r12; push rbp; mov r12d, esi; push rbx; mov rbx, rdi; sub rsp, 0x10 (14 bytes, position-independent)
    g_update = reinterpret_cast<CameraUpdate>(make_trampoline(kCameraUpdate, 14));
    hook_jump(kCameraUpdate, reinterpret_cast<const void*>(&h_camera_update));
    // push r15/r14/r13/r12/rbp/rbx; sub rsp, 0xe8 (17 bytes)
    g_scene_start = reinterpret_cast<SceneFn>(make_trampoline(0x494fa0, 17));
    hook_jump(0x494fa0, reinterpret_cast<const void*>(&h_scene_start));
}
}  // namespace raven::game
