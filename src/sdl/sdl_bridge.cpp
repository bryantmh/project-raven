// The game's statically linked SDL 2.0.2/2.0.3 can only use X11 here, so the SDL entry points the game's own code
// calls are redirected to a real Windows SDL2. docs/functions.md#statically-linked-sdl-202203-0x640000-0x690000
//
// The game only reaches SDL from a few places (WndOpen 0x49ae40, the swap wrapper 0x49b060, the cursor wrappers
// 0x49ae10/0x49b050, the swap-interval setters 0x4fb7e0/0x4fc400, the loading screen's event pump 0x4fc460, SysInit
// 0x4b9450 and SysShutdown 0x4ba180), so these hooks cover all of it. Window pointers handed to the game are real
// SDL2 windows; the game only passes them back to hooked functions.
#define SDL_MAIN_HANDLED
#include <windows.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <deque>
#include <mutex>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app/config.h"
#include "game/hook.h"
#include "gl/gl_bridge.h"
#include "input/input.h"
#include "ui/menu.h"
#include "linux/shim.h"
#include "log.h"
#include "sdl/sdl_bridge.h"

namespace raven::sdl {
namespace {
SDL_Window* g_window = nullptr;
SDL_GLContext g_context = nullptr;
int g_game_w = 1920, g_game_h = 1080;  // the game's render size (WndOpen)

// Game addresses of SDL entry points (docs/functions.md)
constexpr uint64_t kSDL_Init = 0x63a0a0, kSDL_Quit = 0x63a500, kSDL_ShowCursor = 0x65b800,
                   kSDL_GL_SetAttribute = 0x644850, kSDL_CreateWindow = 0x645e40, kSDL_GL_CreateContext = 0x644e90,
                   kSDL_SetWindowDisplayMode = 0x646570, kSDL_GL_SetSwapInterval = 0x6463a0,
                   kSDL_GL_SwapWindow = 0x646470, kSDL_PollEvent = 0x65c860;

SYSV int h_init(uint32_t flags) {
    RLOG("SDL_Init(%#x)", flags);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // The window and taskbar use raven.exe's icon (src/raven.rc).
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "101");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "101");
    // Controllers keep working while another window has focus (and on the hidden test desktop).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // The window and taskbar use raven.exe's icon (src/raven.rc).
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "101");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "101");
    return SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC | SDL_INIT_EVENTS);
}
SYSV void h_quit() {}  // the process exits right after; keep the window alive until then
SYSV int h_show_cursor(int toggle) { return SDL_ShowCursor(SDL_DISABLE); }  // never show the OS cursor in game
SYSV int h_gl_set_attribute(int attr, int value) {
    RTRACE("SDL_GL_SetAttribute(%d, %d)", attr, value);
    if (attr == SDL_GL_CONTEXT_FLAGS) value &= ~SDL_GL_CONTEXT_DEBUG_FLAG;
    return SDL_GL_SetAttribute(SDL_GLattr(attr), value);
}
void request_game_resize(int w, int h);
SYSV SDL_Window* h_create_window(const char* title, int x, int y, int w, int h, uint32_t flags) {
    auto& cfg = app::config();
    // The game asks for a window at its render size (-f). It renders into a framebuffer of that size which we scale
    // into the window (gl::create_game_framebuffer), so the window itself follows the display mode setting.
    RLOG("SDL_CreateWindow(\"%s\", %dx%d, game flags %#x)", title ? title : "", w, h, flags);
    g_game_w = w;
    g_game_h = h;
    uint32_t f = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    g_window = SDL_CreateWindow("Halo: Fireteam Raven", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                std::max(cfg.window_width, 320), std::max(cfg.window_height, 240), f);
    if (!g_window) RLOG("SDL_CreateWindow failed: %s", SDL_GetError());
    else {
        apply_display_mode();
        int ww, wh;
        SDL_GetWindowSize(g_window, &ww, &wh);  // (no drawable before the GL context; DPI-aware, so the same)
        request_game_resize(ww, wh);  // the game's aspect follows the window from the first frame
    }
    return g_window;
}
SYSV SDL_GLContext h_gl_create_context(SDL_Window* w) {
    g_context = SDL_GL_CreateContext(w);
    if (!g_context) {
        RLOG("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return nullptr;
    }
    SDL_GL_MakeCurrent(w, g_context);
    gl::load_entry_points();
    if (!std::getenv("RAVEN_NO_FBO")) gl::create_game_framebuffer(g_game_w, g_game_h);  // RAVEN_NO_FBO: A/B testing
    return g_context;
}
SYSV int h_set_window_display_mode(SDL_Window*, const SDL_DisplayMode*) { return 0; }
// VSync is the user's setting (Settings > Display), whatever the game's scripts ask for (SET_VSYNC).
SYSV int h_gl_set_swap_interval(int) { return SDL_GL_SetSwapInterval(app::config().vsync ? 1 : 0); }

bool g_applying_mode = false;

// Live window shape. The game renders at a fixed size (by default the display's, so it is sharp at any window size)
// into gl's game framebuffer, which is scaled to fill the window. Its render targets are made once at startup and
// can't be rebuilt mid-game (re-running 0x4ef630 corrupts the next level load), so when the window's shape changes we
// change only the aspect ratio the game projects with (0x1cc1fac, read every frame): the 3D view and the UI layout
// (ui_aspect.cpp) then match the window, and the frame fills it without bars or distortion.
constexpr uint64_t kVidConfig = 0x1749f80, kAspect = 0x1cc1fac;
int g_pending_w = 0, g_pending_h = 0;
uint64_t g_pending_at = 0;

void request_game_resize(int w, int h) {
    if (w < 64 || h < 64) return;
    g_pending_w = w;
    g_pending_h = h;
    g_pending_at = SDL_GetTicks64();
}

void apply_game_resize() {
    if (!g_pending_w || SDL_GetTicks64() - g_pending_at < 50) return;
    int w = g_pending_w, h = g_pending_h;
    g_pending_w = 0;
    // Aspect ratio setting: Auto follows the window; 16:9 / 21:9 show that shape, letterboxed in the window.
    int mode = app::config().aspect;
    float aspect = mode == 1 ? 16.0f / 9.0f : mode == 2 ? 64.0f / 27.0f : float(w) / float(h);
    gl::set_view_aspect(mode == 0 ? 0.0f : aspect);
    *reinterpret_cast<float*>(kAspect) = aspect;
    *reinterpret_cast<float*>(kVidConfig + 0xc) = aspect;
    RLOG("display: window %dx%d, game aspect %.3f (renders at %dx%d)", w, h, aspect, g_game_w, g_game_h);
}

// The windowed size the user drags to is kept for the next start (with Resolution on "match", the game then
// renders at exactly that size).
void remember_window_size() {
    auto& cfg = app::config();
    if (cfg.display_mode != 0 || g_applying_mode || !g_window) return;
    if (SDL_GetWindowFlags(g_window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED)) return;
    int w, h;
    SDL_GetWindowSize(g_window, &w, &h);
    if (w == cfg.window_width && h == cfg.window_height) return;
    cfg.window_width = w;
    cfg.window_height = h;
    app::save_config();
}

// Events for the game's PC input driver (pmrt_io, 0x618ea0): keyboard, mouse and text events from the real window,
// queued here and handed out by the hooked SDL_PollEvent. SDL_Event's layout for these types is the same in 2.0.3
// and 2.30. Mouse coordinates are rescaled from the window to the game's render size.
std::deque<SDL_Event> g_queue;
std::mutex g_queue_mutex;

bool for_game(const SDL_Event& e) {
    switch (e.type) {
        case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
        case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
            return true;
        default:
            return false;
    }
}

void pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            remember_window_size();
            int dw, dh;
            SDL_GL_GetDrawableSize(g_window, &dw, &dh);
            request_game_resize(dw, dh);
        }
        if (e.type == SDL_QUIT) {
            RLOG("window closed");
            ExitProcess(0);
        }
        if (ui::on_event(e)) continue;          // Select / F1 opens the settings menu
        if (input::on_sdl_event(e)) continue;  // controllers, the settings menu
        if (!for_game(e)) continue;
        if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
            int ww, wh;
            SDL_GetWindowSize(g_window, &ww, &wh);
            int& x = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
            int& y = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
            if (ww > 0 && wh > 0) { x = x * g_game_w / ww; y = y * g_game_h / wh; }
        }
        std::lock_guard<std::mutex> lk(g_queue_mutex);
        if (g_queue.size() < 1024) g_queue.push_back(e);
    }
}
int g_requested_shots = 0;  // from the test script ("shot:N")
// RAVEN_SHOT=<path.bmp>: save the frame every RAVEN_SHOT_SECONDS (default 5) for automated test runs.
void maybe_screenshot() {
    static const char* path = std::getenv("RAVEN_SHOT");
    if (!path) return;
    static uint64_t last = 0;
    static const uint64_t every = [] { const char* s = std::getenv("RAVEN_SHOT_SECONDS"); return uint64_t(s ? std::atoi(s) : 5) * 1000; }();
    uint64_t now = SDL_GetTicks64();
    // RAVEN_SHOT_BURST=<n>: each time, also save the next n-1 frames (to see per-frame changes such as flicker).
    static const int burst = [] { const char* s = std::getenv("RAVEN_SHOT_BURST"); return s ? std::atoi(s) : 1; }();
    static int burst_left = 0;
    if (g_requested_shots > 0) {
        burst_left = g_requested_shots;
        g_requested_shots = 0;
    }
    if (burst_left > 0) burst_left--;
    else if (now - last < every) return;
    else {
        last = now;
        burst_left = burst - 1;
    }
    int w, h;
    SDL_GL_GetDrawableSize(g_window, &w, &h);
    std::vector<uint8_t> px(size_t(w) * h * 4);
    auto read_buffer = reinterpret_cast<void(__stdcall*)(unsigned)>(gl::real("glReadBuffer"));
    auto read_pixels = reinterpret_cast<void(__stdcall*)(int, int, int, int, unsigned, unsigned, void*)>(gl::real("glReadPixels"));
    auto bind_fb = reinterpret_cast<void(__stdcall*)(unsigned, unsigned)>(gl::real("glBindFramebuffer"));
    if (bind_fb) bind_fb(0x8CA8 /*GL_READ_FRAMEBUFFER*/, 0);
    read_buffer(0x0405 /*GL_BACK*/);
    read_pixels(0, 0, w, h, 0x80E1 /*GL_BGRA*/, 0x1401, px.data());
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ARGB8888);
    // GL rows are bottom-up; BMP via SDL is top-down, so flip.
    std::vector<uint8_t> row(size_t(w) * 4);
    for (int y = 0; y < h / 2; y++) {
        std::memcpy(row.data(), &px[size_t(y) * w * 4], row.size());
        std::memcpy(&px[size_t(y) * w * 4], &px[size_t(h - 1 - y) * w * 4], row.size());
        std::memcpy(&px[size_t(h - 1 - y) * w * 4], row.data(), row.size());
    }
    // A "%d" in the path numbers the shots (0, 1, 2, ...).
    static int shot_index = 0;
    char name[1024];
    std::snprintf(name, sizeof name, path, shot_index++);
    SDL_SaveBMP(s, name);
    SDL_FreeSurface(s);
}

uint64_t g_frames = 0;
// Frame rate limit (Settings > Display): sleep to the next frame deadline, the last ~1 ms spinning.
void limit_frame_rate() {
    int fps = app::config().fps_limit;
    static uint64_t next = 0;
    uint64_t freq = SDL_GetPerformanceFrequency(), now = SDL_GetPerformanceCounter();
    if (fps <= 0) {
        next = 0;
        return;
    }
    uint64_t period = freq / uint64_t(fps);
    if (next == 0 || now > next + period) next = now;  // first frame, or fell behind: no catch-up burst
    while (true) {
        now = SDL_GetPerformanceCounter();
        if (now >= next) break;
        uint64_t left_us = (next - now) * 1000000 / freq;
        if (left_us > 1500) SDL_Delay(uint32_t((left_us - 1000) / 1000));
        else SwitchToThread();
    }
    next += period;
}

SYSV void h_gl_swap_window(SDL_Window* w) {
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);
    gl::present_game_framebuffer(dw, dh);  // the game's frame, scaled into the window
    // Movies fill wide screens (Settings > Presentation): scale them by the view's aspect over 16:9.
    float view_aspect = *reinterpret_cast<const float*>(0x1cc1fac);
    gl::set_movie_scale(app::config().movies_fill ? std::max(1.0f, view_aspect / (16.0f / 9.0f)) : 1.0f);
    ui::before_present();
    maybe_screenshot();
    if (++g_frames % 600 == 0) RLOG("frame %llu at %.1f s (game frame ms %d, clock %d s %.3f)", (unsigned long long)g_frames, SDL_GetTicks64() / 1000.0, *reinterpret_cast<const int*>(0x1dc416c), *reinterpret_cast<const int*>(0x17f8470), *reinterpret_cast<const float*>(0x17f8464));
    limit_frame_rate();
    SDL_GL_SwapWindow(w ? w : g_window);
    gl::rebind_game_framebuffer();
    pump();
    apply_game_resize();
    input::frame();
}
}  // namespace
void present_menu_frame() {
    maybe_screenshot();
    SDL_GL_SwapWindow(g_window);
}
void present() {
    maybe_screenshot();
    SDL_GL_SwapWindow(g_window);
    pump();
}
namespace {
// The game only pumps (event == NULL) from its loading screen; it reads no SDL events.
SYSV int h_poll_event(void* ev) {
    if (!ev) {
        pump();
        std::lock_guard<std::mutex> lk(g_queue_mutex);
        return !g_queue.empty();
    }
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    if (g_queue.empty()) return 0;
    std::memcpy(ev, &g_queue.front(), sizeof(SDL_Event));
    g_queue.pop_front();
    return 1;
}
}  // namespace

void install() {
    SDL_SetMainReady();
    game::hook_jump(kSDL_Init, reinterpret_cast<const void*>(&h_init));
    game::hook_jump(kSDL_Quit, reinterpret_cast<const void*>(&h_quit));
    game::hook_jump(kSDL_ShowCursor, reinterpret_cast<const void*>(&h_show_cursor));
    game::hook_jump(kSDL_GL_SetAttribute, reinterpret_cast<const void*>(&h_gl_set_attribute));
    game::hook_jump(kSDL_CreateWindow, reinterpret_cast<const void*>(&h_create_window));
    game::hook_jump(kSDL_GL_CreateContext, reinterpret_cast<const void*>(&h_gl_create_context));
    game::hook_jump(kSDL_SetWindowDisplayMode, reinterpret_cast<const void*>(&h_set_window_display_mode));
    game::hook_jump(kSDL_GL_SetSwapInterval, reinterpret_cast<const void*>(&h_gl_set_swap_interval));
    game::hook_jump(kSDL_GL_SwapWindow, reinterpret_cast<const void*>(&h_gl_swap_window));
    game::hook_jump(kSDL_PollEvent, reinterpret_cast<const void*>(&h_poll_event));
}

SDL_Window* window() { return g_window; }

// Windowed / borderless fullscreen / exclusive fullscreen (Settings > Display). Applies at once.
void apply_display_mode() {
    auto& cfg = app::config();
    if (!g_window) return;
    int display = std::max(0, SDL_GetWindowDisplayIndex(g_window));
    if (cfg.display_mode == 2) {
        // Exclusive: the display mode closest to the game's render size, at the highest refresh rate.
        SDL_SetWindowBordered(g_window, SDL_TRUE);
        SDL_DisplayMode want{}, got{}, best{};
        want.w = g_game_w;
        want.h = g_game_h;
        for (int i = 0, n = SDL_GetNumDisplayModes(display); i < n; i++)
            if (SDL_GetDisplayMode(display, i, &got) == 0 && got.w == want.w && got.h == want.h &&
                got.refresh_rate > best.refresh_rate)
                best = got;
        if (!best.w && SDL_GetClosestDisplayMode(display, &want, &got)) best = got;
        if (best.w) SDL_SetWindowDisplayMode(g_window, &best);
        SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN);
        RLOG("display: exclusive fullscreen %dx%d @ %d Hz", best.w, best.h, best.refresh_rate);
        return;
    }
    g_applying_mode = true;
    SDL_SetWindowFullscreen(g_window, 0);
    if (cfg.display_mode == 1) {
        // Borderless: a plain window without decorations over the display, never an SDL fullscreen mode. It hangs
        // one pixel below the screen: a GL window covering a monitor exactly is promoted to exclusive fullscreen
        // by the driver (the monitor blanks and switches modes, VRR resyncs); one pixel more keeps it an ordinary
        // composited window. The extra row is off-screen.
        SDL_Rect r;
        SDL_GetDisplayBounds(display, &r);
        SDL_SetWindowBordered(g_window, SDL_FALSE);
        SDL_SetWindowResizable(g_window, SDL_FALSE);
        SDL_SetWindowPosition(g_window, r.x, r.y);
        SDL_SetWindowSize(g_window, r.w, r.h + 1);
        RLOG("display: borderless %dx%d (+1 row)", r.w, r.h);
    } else {
        SDL_SetWindowBordered(g_window, SDL_TRUE);
        SDL_SetWindowResizable(g_window, SDL_TRUE);
        SDL_SetWindowSize(g_window, std::max(cfg.window_width, 320), std::max(cfg.window_height, 240));
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        RLOG("display: windowed %dx%d", cfg.window_width, cfg.window_height);
    }
    g_applying_mode = false;
}

void request_shots(int n) { g_requested_shots = n; }

void reapply_view_aspect() {
    int w = 0, h = 0;
    if (g_window) SDL_GetWindowSize(g_window, &w, &h);
    request_game_resize(w, h);
    g_pending_at = 0;  // at once
}

// Window coordinates to 0..1 across the game's frame (which may be letterboxed in the window).
void window_to_game(int x, int y, float* gx, float* gy) {
    int ww = 0, wh = 0, rx, ry, rw, rh;
    SDL_GetWindowSize(g_window, &ww, &wh);
    gl::game_rect_in(ww, wh, &rx, &ry, &rw, &rh);
    *gx = rw > 0 ? float(x - rx) / float(rw) : 0.5f;
    *gy = rh > 0 ? float(y - ry) / float(rh) : 0.5f;
}
void game_size(int* w, int* h) { *w = g_game_w; *h = g_game_h; }

void push_event(const SDL_Event& e) {
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    if (g_queue.size() < 1024) g_queue.push_back(e);
}
}  // namespace raven::sdl
