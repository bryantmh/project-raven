// Mouse, keyboard and controller input for the game. docs/input.md
//
// The game's I/O core numbers physical inputs: 0x000-0x1ff are SDL scancodes from its PC input driver (pmrt_io,
// fed by the hooked SDL_PollEvent), 0x201+ are analog channels and cabinet I/O boards. Game code asks for logical
// switches (0x208 Gun0Trigger0, 0x24e CabStart0, 0x256 CabDiag, ...) through a physical->logical remap table that
// IoRemapAdd (0x4acdb0) appends to. We add our own entries so keys drive the cabinet switches; controllers and
// mouse buttons drive those keys (F13-F20, which no keyboard sends).
//
// Aim: the game runs in mouse mode (src/game/patches.cpp), which remaps the PC driver's mouse analog channels
// (physical 0x201/0x202) to every gun's X/Y. Gun aim expects 0..1, but the driver stores SDL_MOUSEMOTION's pixel
// coordinates, so we keep mouse events from the game and write normalized aim into those channels every frame.
#include "input/input.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/config.h"
#include "game/ui_aspect.h"
#include "linux/shim.h"
#include "log.h"
#include "sdl/sdl_bridge.h"

namespace raven::input {
namespace {

// Game I/O core (docs/functions.md#input)
using IoRemapAdd = SYSV int (*)(int physical, int logical);
using AnalogSet = SYSV void (*)(void* channel, float v);
constexpr uint64_t kIoRemapAdd = 0x4acdb0;
constexpr uint64_t kRemapCount = 0x1771850;              // int
constexpr uint64_t kRemapTable = 0x1766f40 + 0x1122 * 8;  // {int physical, int logical}[0x400]
constexpr uint64_t kAnalogSet = 0x61cdc0;
constexpr uint64_t kIoState = 0x180adf0;                  // PC driver state; mouse X/Y channels at +0x6000/+0x6048

// Per-player virtual keys: the gun's fire trigger, reload button (Trigger1), grenade button (Trigger2) and START.
// Controller layout: RT/A fire, LT/B reload, Y/LB reload by pointing off-screen, RB/X grenade, Start join.
struct PlayerKeys {
    SDL_Scancode trigger, alt1, alt2, start;
};
constexpr int kPlayers = 2;
const PlayerKeys kKeys[kPlayers] = {
    {SDL_SCANCODE_F13, SDL_SCANCODE_F14, SDL_SCANCODE_F15, SDL_SCANCODE_F16},
    {SDL_SCANCODE_F17, SDL_SCANCODE_F18, SDL_SCANCODE_F19, SDL_SCANCODE_F20},
};

struct Binding {
    int scancode;
    int logical;
};
// Scancodes are SDL's, which the game's PC driver uses as physical IDs. Gun k's triggers are 0x208/9/a + 12k.
const Binding kBindings[] = {
    {SDL_SCANCODE_SPACE, 0x208},  // Gun0Trigger0 (fire)
    {SDL_SCANCODE_1, 0x24e},      // CabStart0
    {SDL_SCANCODE_2, 0x24f},      // CabStart1
    {SDL_SCANCODE_5, 0x257},      // Cab0Coin0
    {SDL_SCANCODE_F2, 0x256},     // CabDiag (TEST)
    {SDL_SCANCODE_S, 0x288},      // Cab0Service
    {SDL_SCANCODE_F13, 0x208}, {SDL_SCANCODE_F14, 0x209}, {SDL_SCANCODE_F15, 0x20a}, {SDL_SCANCODE_F16, 0x24e},
    {SDL_SCANCODE_F17, 0x214}, {SDL_SCANCODE_F18, 0x215}, {SDL_SCANCODE_F19, 0x216}, {SDL_SCANCODE_F20, 0x24f},
};

bool remap_present(int phys, int logical) {
    int n = *reinterpret_cast<const int*>(kRemapCount);
    auto t = reinterpret_cast<const int*>(kRemapTable);
    for (int i = 0; i < n && i < 0x400; i++)
        if (t[i * 2] == phys && t[i * 2 + 1] == logical) return true;
    return false;
}

void ensure_bindings() {
    // The game rebuilds its remap table during init, so re-add ours whenever they're missing.
    for (auto& b : kBindings)
        if (!remap_present(b.scancode, b.logical)) {
            reinterpret_cast<IoRemapAdd>(kIoRemapAdd)(b.scancode, b.logical);
            RLOG("input: bound scancode %d -> logical %#x", b.scancode, b.logical);
        }
}

void push_key(int scancode, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.timestamp = SDL_GetTicks();
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = SDL_Scancode(scancode);
    e.key.keysym.sym = SDL_GetKeyFromScancode(SDL_Scancode(scancode));
    sdl::push_event(e);
}

// A virtual switch that sends a key event when its state changes.
struct Switch {
    bool down = false;
    void set(SDL_Scancode sc, bool d) {
        if (d != down) push_key(sc, d);
        down = d;
    }
};

struct Player {
    SDL_GameController* pad = nullptr;
    SDL_JoystickID id = -1;
    Switch trigger, alt1, alt2, start;
    // Mouse buttons count for player 1 (and scripted presses).
    bool mouse_trigger = false, mouse_alt1 = false, mouse_alt2 = false, key_reload = false;
    bool reload = false;  // the gun points off-screen while held, which reloads it (offscreenReload)
    uint64_t next_rumble = 0;
};
Player g_players[kPlayers];
float g_aim_x = 0.5f, g_aim_y = 0.5f;  // shared by all guns in mouse mode

// The last aims written, newest first (the game draws its crosshairs a frame or more behind).
constexpr int kAimHistory = 16;
float g_hist_x[kAimHistory], g_hist_y[kAimHistory];
int g_hist_n = 0;
void write_aim() {
    auto base = *reinterpret_cast<uint8_t* const*>(kIoState);
    if (!base) return;
    for (int i = kAimHistory - 1; i > 0; i--) g_hist_x[i] = g_hist_x[i - 1], g_hist_y[i] = g_hist_y[i - 1];
    g_hist_x[0] = g_aim_x;
    g_hist_y[0] = g_aim_y;
    if (g_hist_n < kAimHistory) g_hist_n++;
    bool off = g_players[0].reload || g_players[1].reload;
    reinterpret_cast<AnalogSet>(kAnalogSet)(base + 0x6000, off ? -0.5f : g_aim_x);
    reinterpret_cast<AnalogSet>(kAnalogSet)(base + 0x6048, off ? -0.5f : g_aim_y);
}

void open_pad(int device_index) {
    if (!SDL_IsGameController(device_index)) return;
    // Automated test runs (RAVEN_HEADLESS) use only the script's virtual controller: real controllers are never
    // opened, so a test can't rumble, recolor or read the controllers the user is busy with.
    static const bool headless = std::getenv("RAVEN_HEADLESS") != nullptr;
    if (headless && !SDL_JoystickIsVirtual(device_index)) {
        RLOG("input: test run, ignoring controller \"%s\"", SDL_JoystickNameForIndex(device_index));
        return;
    }
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    for (auto& p : g_players)
        if (p.pad && p.id == id) return;
    for (int i = 0; i < kPlayers; i++) {
        auto& p = g_players[i];
        if (p.pad) continue;
        p.pad = SDL_GameControllerOpen(device_index);
        if (!p.pad) return;
        p.id = id;
        RLOG("input: player %d controller \"%s\" (%s)", i + 1, SDL_GameControllerName(p.pad),
             SDL_GameControllerHasRumble(p.pad) ? "rumble" : "no rumble");
        if (SDL_GameControllerHasLED(p.pad)) {
            static const uint8_t colors[kPlayers][3] = {{0, 160, 255}, {255, 60, 0}};
            SDL_GameControllerSetLED(p.pad, colors[i][0], colors[i][1], colors[i][2]);
        }
        return;
    }
}

void close_pad(SDL_JoystickID id) {
    for (int i = 0; i < kPlayers; i++) {
        auto& p = g_players[i];
        if (!p.pad || p.id != id) continue;
        SDL_GameControllerClose(p.pad);
        p.pad = nullptr;
        p.id = -1;
        p.trigger.set(kKeys[i].trigger, false);
        p.alt1.set(kKeys[i].alt1, false);
        p.alt2.set(kKeys[i].alt2, false);
        p.start.set(kKeys[i].start, false);
        RLOG("input: player %d controller removed", i + 1);
    }
}

float axis(SDL_GameController* c, SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(c, a) / 32767.0f; }

// Stick aim: whichever stick is deflected more, radial deadzone, squared response, velocity in screen heights/s.
void stick_aim(Player& p, float dt) {
    const auto& cfg = app::config();
    float rx = axis(p.pad, SDL_CONTROLLER_AXIS_RIGHTX), ry = axis(p.pad, SDL_CONTROLLER_AXIS_RIGHTY);
    float lx = axis(p.pad, SDL_CONTROLLER_AXIS_LEFTX), ly = axis(p.pad, SDL_CONTROLLER_AXIS_LEFTY);
    float x = rx, y = ry;
    if (lx * lx + ly * ly > rx * rx + ry * ry) x = lx, y = ly;
    float mag = std::sqrt(x * x + y * y);
    float dz = std::clamp(cfg.aim_deadzone, 0, 90) / 100.0f;
    if (mag <= dz) return;
    float scaled = std::min(1.0f, (mag - dz) / (1.0f - dz));
    float k = scaled * scaled / mag;
    float speed = 1.6f * std::max(cfg.aim_speed, 1) / 100.0f;
    int w = 16, h = 9;
    sdl::game_size(&w, &h);
    // In the menus the crosshair is drawn squeezed into the 16:9 area (game::menu_aim_squeeze): move the aim
    // faster on that axis so the crosshair keeps the same on-screen speed.
    int axis_sq = 0;
    float sq = game::menu_aim_squeeze(&axis_sq);
    g_aim_x = std::clamp(g_aim_x + x * k * speed * dt * float(h) / float(w) * (axis_sq == 0 ? sq : 1.0f), 0.0f, 1.0f);
    g_aim_y = std::clamp(g_aim_y + y * k * speed * dt * (axis_sq == 1 ? sq : 1.0f), 0.0f, 1.0f);
}

// Recoil rumble: a short pulse every 100 ms while the trigger is held.
void rumble(Player& p, uint64_t now) {
    int strength = std::clamp(app::config().rumble, 0, 100);
    if (!p.pad || !p.trigger.down || strength == 0 || now < p.next_rumble) return;
    p.next_rumble = now + 100;
    auto lo = uint16_t(0x5000 * strength / 100), hi = uint16_t(0xd000 * strength / 100);
    SDL_GameControllerRumble(p.pad, lo, hi, 70);
}

// RAVEN_TEST_KEYS="seconds:kind:args,...": scripted input for automated runs.
//   down:<scancode> / up:<scancode>     key press / release
//   move:<x>:<y>                        aim at (x, y) in 1920x1080 coordinates
//   cursor:<x>:<y>                      mouse motion to window pixel (x, y), as the real mouse
//   mdown:<button> / mup:<button>       mouse button (1 left = fire, 3 right = reload, 2 middle = grenade)
//   shot:<n>                            save the next n frames (needs RAVEN_SHOT)
//   aspect:<0|1|2>                      Aspect ratio setting (auto / 16:9 / 21:9), live
//   pad:0                               attach a virtual Xbox-layout controller (SDL virtual joystick)
//   pbtn:<SDL_GameControllerButton>:<0|1>, paxis:<SDL_GameControllerAxis>:<-32768..32767>   drive it
struct ScriptStep {
    double t;
    std::string kind;
    int a = 0, b = 0;
    bool done = false;
};
std::vector<ScriptStep> g_script;
uint64_t g_start = 0;

void load_script() {
    const char* s = std::getenv("RAVEN_TEST_KEYS");
    if (!s) return;
    std::string str = s;
    size_t i = 0;
    while (i < str.size()) {
        size_t j = str.find(',', i);
        if (j == std::string::npos) j = str.size();
        std::string item = str.substr(i, j - i);
        ScriptStep st{};
        char kind[8];
        if (std::sscanf(item.c_str(), "%lf:%7[a-z]:%d:%d", &st.t, kind, &st.a, &st.b) >= 3) {
            st.kind = kind;
            g_script.push_back(st);
        }
        i = j + 1;
    }
}

void mouse_button(int button, bool down) {
    auto& p = g_players[0];
    if (button == SDL_BUTTON_LEFT) p.mouse_trigger = down;
    else if (button == SDL_BUTTON_RIGHT) p.mouse_alt1 = down;
    else if (button == SDL_BUTTON_MIDDLE) p.mouse_alt2 = down;
}

// A virtual controller for automated runs. Its buttons and axes are numbered like SDL_GameController's, which
// SDL maps through the virtual joystick's button/axis masks.
SDL_Joystick* g_virtual = nullptr;
void attach_virtual_pad() {
    SDL_VirtualJoystickDesc d{};
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
    d.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1;
    d.name = "Raven test controller";
    int index = SDL_JoystickAttachVirtualEx(&d);
    if (index < 0) {
        RLOG("input: virtual controller failed: %s", SDL_GetError());
        return;
    }
    g_virtual = SDL_JoystickOpen(index);
    for (int a = SDL_CONTROLLER_AXIS_TRIGGERLEFT; a <= SDL_CONTROLLER_AXIS_TRIGGERRIGHT; a++)
        SDL_JoystickSetVirtualAxis(g_virtual, a, -32768);  // triggers rest at the minimum
}

void run_script() {
    double now = double(SDL_GetTicks64() - g_start) / 1000.0;
    for (auto& s : g_script)
        if (!s.done && now >= s.t) {
            s.done = true;
            RLOG("input: test %s %d %d", s.kind.c_str(), s.a, s.b);
            if (s.kind == "down" || s.kind == "up") push_key(s.a, s.kind == "down");
            else if (s.kind == "move") g_aim_x = s.a / 1920.0f, g_aim_y = s.b / 1080.0f;
            else if (s.kind == "cursor") {
                SDL_Event e{};
                e.type = SDL_MOUSEMOTION;
                e.motion.x = s.a;
                e.motion.y = s.b;
                on_sdl_event(e);
            }
            else if (s.kind == "mdown" || s.kind == "mup") mouse_button(s.a, s.kind == "mdown");
            else if (s.kind == "pad") attach_virtual_pad();
            else if (s.kind == "shot") sdl::request_shots(s.a);
            else if (s.kind == "aspect") {  // the Aspect ratio setting, as the menu changes it
                app::config().aspect = s.a;
                sdl::reapply_view_aspect();
            }
            else if (s.kind == "pbtn" && g_virtual) SDL_JoystickSetVirtualButton(g_virtual, s.a, uint8_t(s.b));
            else if (s.kind == "paxis" && g_virtual) SDL_JoystickSetVirtualAxis(g_virtual, s.a, int16_t(s.b));
        }
}

void io_debug() {
    static bool on = std::getenv("RAVEN_IODEBUG") != nullptr;
    static int n = 0;
    if (!on || ++n % 60) return;
    auto base = *reinterpret_cast<uint8_t* const*>(kIoState);
    if (!base) return;
    auto f = [&](size_t o) { return *reinterpret_cast<const float*>(base + o); };
    RLOG("iodebug: mouse x raw %.3f val %.3f | y raw %.3f val %.3f", f(0x600c), f(0x6004), f(0x6054), f(0x604c));
}
}  // namespace

int aim_history(const float** xs, const float** ys) {
    *xs = g_hist_x;
    *ys = g_hist_y;
    return g_hist_n;
}

bool aim(int player, float* x, float* y) {
    if (player != 0) return false;  // all guns share the mouse-mode aim for now
    *x = g_aim_x;
    *y = g_aim_y;
    return true;
}

bool on_sdl_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_CONTROLLERDEVICEADDED:
            open_pad(e.cdevice.which);
            return true;
        case SDL_CONTROLLERDEVICEREMOVED:
            close_pad(e.cdevice.which);
            return true;
        case SDL_MOUSEMOTION: {
            float gx, gy;
            sdl::window_to_game(e.motion.x, e.motion.y, &gx, &gy);
            // Menus: the crosshair is drawn squeezed into the 16:9 area; aim so it lands under the cursor.
            int axis_sq = 0;
            float sq = game::menu_aim_squeeze(&axis_sq);
            (axis_sq == 0 ? gx : gy) = 0.5f + ((axis_sq == 0 ? gx : gy) - 0.5f) * sq;
            g_aim_x = std::clamp(gx, 0.0f, 1.0f);
            g_aim_y = std::clamp(gy, 0.0f, 1.0f);
            return true;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            mouse_button(e.button.button, e.type == SDL_MOUSEBUTTONDOWN);
            return true;
        case SDL_MOUSEWHEEL:
            return true;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (e.key.keysym.scancode != SDL_SCANCODE_R) return false;
            if (!e.key.repeat) g_players[0].key_reload = e.type == SDL_KEYDOWN;
            return true;
        default:
            return false;
    }
}

void frame() {
    static bool init = false;
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks64();
    if (!init) {
        init = true;
        g_start = last = now;
        load_script();
        for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
    }
    float dt = std::min(0.1f, float(now - last) / 1000.0f);
    last = now;
    ensure_bindings();
    run_script();
    for (int i = 0; i < kPlayers; i++) {
        auto& p = g_players[i];
        bool trig = p.mouse_trigger, alt1 = p.mouse_alt1, alt2 = p.mouse_alt2, start = false;
        p.reload = p.key_reload;
        if (p.pad) {
            stick_aim(p, dt);
            auto b = [&](SDL_GameControllerButton x) { return SDL_GameControllerGetButton(p.pad, x) != 0; };
            trig |= axis(p.pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.4f || b(SDL_CONTROLLER_BUTTON_A);
            alt1 |= axis(p.pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.4f || b(SDL_CONTROLLER_BUTTON_B);
            alt2 |= b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) || b(SDL_CONTROLLER_BUTTON_X);
            start = b(SDL_CONTROLLER_BUTTON_START);
            p.reload |= b(SDL_CONTROLLER_BUTTON_Y) || b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        }
        p.trigger.set(kKeys[i].trigger, trig);
        p.alt1.set(kKeys[i].alt1, alt1);
        p.alt2.set(kKeys[i].alt2, alt2);
        p.start.set(kKeys[i].start, start);
        rumble(p, now);
    }
    write_aim();
    io_debug();
}
}  // namespace raven::input
