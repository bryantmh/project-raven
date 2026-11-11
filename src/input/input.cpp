// Mouse, keyboard and controller input for the game. docs/input.md
//
// The game's I/O core numbers physical inputs: 0x000-0x1ff are SDL scancodes from its PC input driver (pmrt_io,
// fed by the hooked SDL_PollEvent), 0x201+ are analog channels and cabinet I/O boards. Game code asks for logical
// switches (0x208 Gun0Trigger0, 0x24e CabStart0, 0x256 CabDiag, ...) through a physical->logical remap table that
// IoRemapAdd (0x4acdb0) appends to. We add our own entries so keys drive the cabinet switches; controllers and
// mouse buttons drive those keys (scancodes no keyboard sends).
//
// Aim: the game runs in mouse mode (src/game/patches.cpp), which remaps the PC driver's mouse analog channels
// (physical 0x201/0x202) to every gun's X/Y. Gun aim expects 0..1, but the driver stores SDL_MOUSEMOTION's pixel
// coordinates, so we keep mouse events from the game and write gun 0's aim into those channels every frame. Guns
// 1-3 get their own aim: their axes are unhooked from the mouse channels and fed from channels of ours.
//
// Players and guns: each gun slot (0-3) is one ODST (src/game/players.cpp). The 2-player cabinet uses slots 0 and 3,
// the 4-player cabinet all four. Each player plays the slot of the ODST they picked (game::player_slot), so the
// virtual keys and aim channels below belong to slots, and every frame each slot takes the state of its player.
//
// Buttons come from the remappable bindings (src/input/bindings.cpp): each player has their own controller
// bindings; the keyboard and mouse drive player 1.
#include "input/input.h"

#include <windows.h>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/config.h"
#include "input/bindings.h"
#include "game/movie_skip.h"
#include "game/players.h"
#include "game/triggers.h"
#include "game/ui_aspect.h"
#include "linux/shim.h"
#include "log.h"
#include "sdl/sdl_bridge.h"

namespace raven::input {
namespace {

// Game I/O core (docs/functions.md#input)
using IoRemapAdd = SYSV int (*)(int physical, int logical);
using IoRemapRemove = SYSV int (*)(int physical, int logical);
constexpr uint64_t kIoRemapRemove = 0x4ace10;
constexpr int kPlayers = game::kMaxPlayers;
constexpr int kGuns = 4;
// Gun k's aim: logical X 0x206 + 12k / Y 0x207 + 12k, plus the second pair mouse mode feeds every gun
// (0x2a3 + 3k / 0x2a4 + 3k, remap builder 0x41baf0). The I/O core fills logical channels every frame by copying each
// remap's physical channel over them (0x4ad6e8), and game code only uses a gun whose axes have a remap source
// (0x4acfe0), so guns 1-3 need physical channels of their own: see install_gun_channels.
int aim_logical(int gun, int axis, int pair) { return pair == 0 ? 0x206 + 12 * gun + axis : 0x2a3 + 3 * gun + axis; }
// Unused physical analog IDs for guns 1-3 (X, Y). Gun 3 (the 2-player cabinet's player 2) keeps 0x210 / 0x211.
constexpr int kGunPhysical[4][2] = {{0, 0}, {0x212, 0x213}, {0x214, 0x215}, {0x210, 0x211}};
constexpr uint64_t kPhysAnalogTable = 0x11bcd70;  // int32 offsets (from the table) per physical 0x201..0x3c5 (0x61ee70)
alignas(16) uint8_t g_gun_channels[kGuns][2][0x48];  // guns 1-3's physical X / Y channels (AnalogSet layout)
bool g_guns_ready = false;
using AnalogSet = SYSV void (*)(void* channel, float v);
constexpr uint64_t kIoRemapAdd = 0x4acdb0;
constexpr uint64_t kRemapCount = 0x1771850;              // int
constexpr uint64_t kRemapTable = 0x1766f40 + 0x1122 * 8;  // {int physical, int logical}[0x400]
constexpr uint64_t kAnalogSet = 0x61cdc0;
constexpr uint64_t kIoState = 0x180adf0;                  // PC driver state; mouse X/Y channels at +0x6000/+0x6048

// Per-slot virtual keys: the gun's fire trigger, reload button (Trigger1), grenade button (Trigger2) and START.
// Scancodes no keyboard sends: F13-F24, and 0xe8-0xeb (reserved in USB HID, unnamed in SDL).
struct SlotKeys {
    SDL_Scancode trigger, alt1, alt2, start;
};
const SlotKeys kKeys[kGuns] = {
    {SDL_SCANCODE_F13, SDL_SCANCODE_F14, SDL_SCANCODE_F15, SDL_SCANCODE_F16},
    {SDL_SCANCODE_F21, SDL_SCANCODE_F22, SDL_SCANCODE_F23, SDL_SCANCODE_F24},
    {SDL_Scancode(0xe8), SDL_Scancode(0xe9), SDL_Scancode(0xea), SDL_Scancode(0xeb)},
    {SDL_SCANCODE_F17, SDL_SCANCODE_F18, SDL_SCANCODE_F19, SDL_SCANCODE_F20},
};

struct Binding {
    int scancode;
    int logical;
};
// Scancodes are SDL's, which the game's PC driver uses as physical IDs. Gun k's triggers are 0x208/9/a + 12k and
// its start CabStart<k> 0x24e + k. Fixed operator keys (a key bound to an action is never passed to the game, so
// these can't clash).
std::vector<Binding> bindings() {
    std::vector<Binding> b = {
        {SDL_SCANCODE_5, 0x257},   // Cab0Coin0
        {SDL_SCANCODE_F2, 0x256},  // CabDiag (TEST)
        {SDL_SCANCODE_F3, 0x288},  // Cab0Service
    };
    for (int k = 0; k < kGuns; k++) {
        b.push_back({kKeys[k].trigger, 0x208 + 12 * k});
        b.push_back({kKeys[k].alt1, 0x209 + 12 * k});
        b.push_back({kKeys[k].alt2, 0x20a + 12 * k});
        b.push_back({kKeys[k].start, 0x24e + k});
    }
    return b;
}

bool remap_present(int phys, int logical) {
    int n = *reinterpret_cast<const int*>(kRemapCount);
    auto t = reinterpret_cast<const int*>(kRemapTable);
    for (int i = 0; i < n && i < 0x400; i++)
        if (t[i * 2] == phys && t[i * 2 + 1] == logical) return true;
    return false;
}

void ensure_bindings() {
    // The game rebuilds its remap table during init, so re-add ours whenever they're missing.
    static const std::vector<Binding> kBindings = bindings();
    for (auto& b : kBindings)
        if (!remap_present(b.scancode, b.logical)) {
            reinterpret_cast<IoRemapAdd>(kIoRemapAdd)(b.scancode, b.logical);
            RLOG("input: bound scancode %d -> logical %#x", b.scancode, b.logical);
        }
    // Guns 1-3 aim on their own: their axes come from their physical channels, not the mouse.
    if (!g_guns_ready) return;
    for (int gun = 1; gun < kGuns; gun++)
        for (int axis = 0; axis < 2; axis++)
            for (int pair = 0; pair < 2; pair++) {
                int logical = aim_logical(gun, axis, pair), mouse = 0x201 + axis, phys = kGunPhysical[gun][axis];
                if (remap_present(mouse, logical)) reinterpret_cast<IoRemapRemove>(kIoRemapRemove)(mouse, logical);
                if (!remap_present(phys, logical)) {
                    reinterpret_cast<IoRemapAdd>(kIoRemapAdd)(phys, logical);
                    RLOG("input: gun %d physical %#x -> logical %#x", gun, phys, logical);
                }
            }
}

// Give the PC driver more physical analog channels (IDs 0x210-0x215, which return none): their entries in the
// driver's lookup table (0x61ee70) point at stubs returning g_gun_channels. The table holds 32-bit offsets from
// itself, so the stubs live just above the game image.
void install_gun_channels() {
    uint8_t* stubs = nullptr;
    for (uint64_t at = 0x2400000; !stubs && at < 0x40000000; at += 0x10000)
        stubs = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!stubs) {
        RLOG("input: no memory near the game for the guns' channels; every gun aims with the mouse channels");
        return;
    }
    for (int gun = 1; gun < kGuns; gun++)
        for (int axis = 0; axis < 2; axis++) {
            uint8_t* stub = stubs + (gun * 2 + axis) * 16;
            uint64_t channel = reinterpret_cast<uint64_t>(g_gun_channels[gun][axis]);
            stub[0] = 0x48, stub[1] = 0xB8;  // mov rax, imm64
            std::memcpy(stub + 2, &channel, 8);
            stub[10] = 0xC3;                 // ret
            auto entry = reinterpret_cast<int32_t*>(kPhysAnalogTable + uint64_t(kGunPhysical[gun][axis] - 0x201) * 4);
            DWORD old = 0;
            VirtualProtect(entry, 4, PAGE_READWRITE, &old);
            *entry = int32_t(reinterpret_cast<int64_t>(stub) - int64_t(kPhysAnalogTable));
            VirtualProtect(entry, 4, old, &old);
        }
    g_guns_ready = true;
    RLOG("input: gun 1-3 physical channels at %p", stubs);
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
// RAVEN_INPUTDEBUG=1 logs every virtual switch change (which slot pressed what).
struct Switch {
    bool down = false;
    void set(SDL_Scancode sc, bool d) {
        static bool debug = std::getenv("RAVEN_INPUTDEBUG") != nullptr;
        if (d != down) {
            push_key(sc, d);
            if (debug) RLOG("inputdebug: %s (%d) %s", SDL_GetScancodeName(sc), int(sc), d ? "down" : "up");
        }
        down = d;
    }
};
// Each gun slot's switches: whoever plays that ODST presses them.
struct SlotSwitches {
    Switch trigger, alt1, alt2, start;
} g_slots[kGuns];

// The last aims written, newest first (the game draws its crosshairs a frame or more behind).
constexpr int kAimHistory = 16;

struct Player {
    SDL_GameController* pad = nullptr;
    SDL_JoystickID id = -1;
    bool trigger = false;  // fire held (rumble)
    float aim_x = 0.5f, aim_y = 0.5f;
    bool start_held = false;
    int skip_frames = 0;  // frames left of a reload press sent to skip a movie (src/game/movie_skip.cpp)
    float hist_x[kAimHistory] = {}, hist_y[kAimHistory] = {};
    int hist_n = 0;
    uint64_t next_rumble = 0;
    int led_slot = -1;  // the ODST whose color the controller's light shows
};
Player g_players[kPlayers];

// Keyboard and mouse state from events (real and scripted), for player 1's bindings.
bool g_keys_down[SDL_NUM_SCANCODES] = {};
unsigned g_mouse_buttons = 0;

// Each ODST's color on its JOIN NOW panel (Graves blue, Hudson orange, Ramos green, Lang purple), for the
// controller light.
constexpr uint8_t kSlotColors[kGuns][3] = {{0, 110, 255}, {255, 90, 0}, {40, 200, 40}, {150, 40, 255}};
void update_led(Player& p, int slot) {
    if (!p.pad || slot < 0 || slot == p.led_slot) return;
    p.led_slot = slot;
    if (SDL_GameControllerHasLED(p.pad)) SDL_GameControllerSetLED(p.pad, kSlotColors[slot][0], kSlotColors[slot][1], kSlotColors[slot][2]);
}

void write_aim() {
    auto base = *reinterpret_cast<uint8_t* const*>(kIoState);
    if (!base) return;
    for (int i = 0; i < game::player_count(); i++) {
        auto& p = g_players[i];
        for (int k = kAimHistory - 1; k > 0; k--) p.hist_x[k] = p.hist_x[k - 1], p.hist_y[k] = p.hist_y[k - 1];
        p.hist_x[0] = p.aim_x;
        p.hist_y[0] = p.aim_y;
        if (p.hist_n < kAimHistory) p.hist_n++;
    }
    auto set = reinterpret_cast<AnalogSet>(kAnalogSet);
    for (int gun = 0; gun < kGuns; gun++) {
        float x = 0.5f, y = 0.5f;
        for (int i = 0; i < game::player_count(); i++)
            if (game::player_slot(i) == gun) x = g_players[i].aim_x, y = g_players[i].aim_y;
        if (gun == 0) {
            set(base + 0x6000, x);
            set(base + 0x6048, y);
        } else if (g_guns_ready) {
            // Gun channels hold y from the bottom (the mouse Y channel's flags flip it for gun 0).
            set(g_gun_channels[gun][0], x);
            set(g_gun_channels[gun][1], 1.0f - y);
        }
    }
}

void open_pad(int device_index) {
    if (!SDL_IsGameController(device_index)) return;
    // Automated test runs (RAVEN_HEADLESS) use only the script's virtual controllers: real controllers are never
    // opened, so a test can't rumble, recolor or read the controllers the user is busy with.
    static const bool headless = std::getenv("RAVEN_HEADLESS") != nullptr;
    if (headless && !SDL_JoystickIsVirtual(device_index)) {
        RLOG("input: test run, ignoring controller \"%s\"", SDL_JoystickNameForIndex(device_index));
        return;
    }
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    for (auto& p : g_players)
        if (p.pad && p.id == id) return;
    // Controllers fill the players in order; with "keyboard & mouse is player 1" they start at player 2.
    for (int i = app::config().kbm_own_player ? 1 : 0; i < game::player_count(); i++) {
        auto& p = g_players[i];
        if (p.pad) continue;
        p.pad = SDL_GameControllerOpen(device_index);
        if (!p.pad) return;
        p.id = id;
        p.led_slot = -1;
        RLOG("input: player %d controller \"%s\" (%s)", i + 1, SDL_GameControllerName(p.pad),
             SDL_GameControllerHasRumble(p.pad) ? "rumble" : "no rumble");
        update_led(p, game::player_slot(i));
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
        p.trigger = false;
        RLOG("input: player %d controller removed", i + 1);
    }
}

float axis(SDL_GameController* c, SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(c, a) / 32767.0f; }

// Stick aim: whichever stick is deflected more, radial deadzone, squared response, velocity in screen heights/s.
// The Aim up/down/left/right bindings (D-pad, keys) push like a fully deflected stick; (dx, dy) is their direction.
void stick_aim(Player& p, float dt, float dx, float dy) {
    const auto& cfg = app::config();
    float x = dx, y = dy, k = 1.0f;
    if (x == 0 && y == 0) {
        if (!p.pad) return;
        float rx = axis(p.pad, SDL_CONTROLLER_AXIS_RIGHTX), ry = axis(p.pad, SDL_CONTROLLER_AXIS_RIGHTY);
        float lx = axis(p.pad, SDL_CONTROLLER_AXIS_LEFTX), ly = axis(p.pad, SDL_CONTROLLER_AXIS_LEFTY);
        x = rx, y = ry;
        if (lx * lx + ly * ly > rx * rx + ry * ry) x = lx, y = ly;
        float mag = std::sqrt(x * x + y * y);
        float dz = std::clamp(cfg.aim_deadzone, 0, 90) / 100.0f;
        if (mag <= dz) return;
        float scaled = std::min(1.0f, (mag - dz) / (1.0f - dz));
        k = scaled * scaled / mag;
    } else if (x != 0 && y != 0) {
        k = 0.7071f;
    }
    float speed = 1.6f * std::max(cfg.aim_speed, 1) / 100.0f;
    int w = 16, h = 9;
    sdl::game_size(&w, &h);
    // In the menus the crosshair is drawn squeezed into the 16:9 area (game::menu_aim_squeeze): move the aim
    // faster on that axis so the crosshair keeps the same on-screen speed.
    int axis_sq = 0;
    float sq = game::menu_aim_squeeze(&axis_sq);
    p.aim_x = std::clamp(p.aim_x + x * k * speed * dt * float(h) / float(w) * (axis_sq == 0 ? sq : 1.0f), 0.0f, 1.0f);
    p.aim_y = std::clamp(p.aim_y + y * k * speed * dt * (axis_sq == 1 ? sq : 1.0f), 0.0f, 1.0f);
}

// Recoil rumble: a short pulse every 100 ms while the trigger is held.
void rumble(Player& p, uint64_t now) {
    int strength = std::clamp(app::config().rumble, 0, 100);
    if (!p.pad || !p.trigger || strength == 0 || now < p.next_rumble) return;
    p.next_rumble = now + 100;
    auto lo = uint16_t(0x5000 * strength / 100), hi = uint16_t(0xd000 * strength / 100);
    SDL_GameControllerRumble(p.pad, lo, hi, 70);
}

// RAVEN_TEST_KEYS="seconds:kind:args,...": scripted input for automated runs.
//   down:<scancode> / up:<scancode>     key press / release (as the real keyboard: through the bindings)
//   move:<x>:<y>                        player 1 aims at (x, y) in 1920x1080 coordinates
//   movb / movc / movd:<x>:<y>          player 2 / 3 / 4 aims at (x, y)
//   cursor:<x>:<y>                      mouse motion to window pixel (x, y), as the real mouse
//   mdown:<button> / mup:<button>       mouse button (1 left = fire, 3 right = reload, 2 middle = grenade)
//   shot:<n>                            save the next n frames (needs RAVEN_SHOT)
//   aspect:<0|1|2>                      Aspect ratio setting (auto / 16:9 / 21:9), live
//   pad:<n>                             attach virtual Xbox-layout controller n (0-3; SDL virtual joystick)
//   pbtn:<SDL_GameControllerButton>:<0|1>, paxis:<SDL_GameControllerAxis>:<-32768..32767>   drive controller 0
//   pbtnb / paxisb, pbtnc / paxisc, pbtnd / paxisd   the same for controllers 1, 2, 3
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
    if (button < 1 || button > 31) return;
    if (down) g_mouse_buttons |= SDL_BUTTON(button);
    else g_mouse_buttons &= ~SDL_BUTTON(button);
}

// A virtual controller for automated runs. Its buttons and axes are numbered like SDL_GameController's, which
// SDL maps through the virtual joystick's button/axis masks.
SDL_Joystick* g_virtual[kPlayers] = {};
void attach_virtual_pad(int n) {
    n = std::clamp(n, 0, kPlayers - 1);
    SDL_VirtualJoystickDesc d{};
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
    d.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1;
    static const char* const names[kPlayers] = {"Raven test controller", "Raven test controller 2",
                                                "Raven test controller 3", "Raven test controller 4"};
    d.name = names[n];
    int index = SDL_JoystickAttachVirtualEx(&d);
    if (index < 0) {
        RLOG("input: virtual controller failed: %s", SDL_GetError());
        return;
    }
    g_virtual[n] = SDL_JoystickOpen(index);
    for (int a = SDL_CONTROLLER_AXIS_TRIGGERLEFT; a <= SDL_CONTROLLER_AXIS_TRIGGERRIGHT; a++)
        SDL_JoystickSetVirtualAxis(g_virtual[n], a, -32768);  // triggers rest at the minimum
}

}  // namespace

bool set_virtual_button(int n, int button, bool down) {
    if (n < 0 || n >= kPlayers || !g_virtual[n]) return false;
    SDL_JoystickSetVirtualButton(g_virtual[n], button, down ? 1 : 0);
    return true;
}

namespace {
void run_script() {
    double now = double(SDL_GetTicks64() - g_start) / 1000.0;
    // RAVEN_TEST_TRIGGER="<seconds>:<trigger name>,...": fire the game's script triggers (tests), on the same clock.
    static std::vector<std::pair<double, std::string>> triggers = [] {
        std::vector<std::pair<double, std::string>> out;
        if (const char* s = std::getenv("RAVEN_TEST_TRIGGER")) {
            std::string str = s;
            for (size_t i = 0; i < str.size();) {
                size_t j = str.find(',', i), c = str.find(':', i);
                if (j == std::string::npos) j = str.size();
                if (c < j) out.push_back({std::atof(str.substr(i, c - i).c_str()), str.substr(c + 1, j - c - 1)});
                i = j + 1;
            }
        }
        return out;
    }();
    for (auto& [t, name] : triggers)
        if (!name.empty() && now >= t) {
            int before = game::trigger_set_count(name.c_str());
            game::set_trigger(name.c_str());
            RLOG("input: test trigger %s (set count %d -> %d)", name.c_str(), before, game::trigger_set_count(name.c_str()));
            name.clear();
        }
    for (auto& s : g_script)
        if (!s.done && now >= s.t) {
            s.done = true;
            RLOG("input: test %s %d %d", s.kind.c_str(), s.a, s.b);
            if (s.kind == "down" || s.kind == "up") {
                SDL_Event e{};
                e.type = s.kind == "down" ? SDL_KEYDOWN : SDL_KEYUP;
                e.key.state = s.kind == "down" ? SDL_PRESSED : SDL_RELEASED;
                e.key.keysym.scancode = SDL_Scancode(s.a);
                e.key.keysym.sym = SDL_GetKeyFromScancode(SDL_Scancode(s.a));
                SDL_PushEvent(&e);  // through the event pump, like the real keyboard (menu, bindings, game)
            }
            else if (s.kind == "move") g_players[0].aim_x = s.a / 1920.0f, g_players[0].aim_y = s.b / 1080.0f;
            else if (s.kind == "movb" || s.kind == "movc" || s.kind == "movd") {
                auto& p = g_players[s.kind[3] - 'a'];
                p.aim_x = s.a / 1920.0f, p.aim_y = s.b / 1080.0f;
            }
            else if (s.kind == "cursor") {
                SDL_Event e{};
                e.type = SDL_MOUSEMOTION;
                e.motion.x = s.a;
                e.motion.y = s.b;
                on_sdl_event(e);
            }
            else if (s.kind == "mdown" || s.kind == "mup") mouse_button(s.a, s.kind == "mdown");
            else if (s.kind == "pad") attach_virtual_pad(s.a);
            else if (s.kind == "shot") sdl::request_shots(s.a);
            else if (s.kind == "aspect") {  // the Aspect ratio setting, as the menu changes it
                app::config().aspect = s.a;
                sdl::reapply_view_aspect();
            }
            else if (s.kind.rfind("pbtn", 0) == 0 || s.kind.rfind("paxis", 0) == 0) {
                // pbtn / pbtnb / pbtnc / pbtnd (paxis the same): controller 0-3
                bool button = s.kind[1] == 'b';
                size_t base = button ? 4 : 5;
                int n = s.kind.size() > base ? s.kind[base] - 'a' : 0;
                if (n < 0 || n >= kPlayers || !g_virtual[n]) continue;
                if (button) SDL_JoystickSetVirtualButton(g_virtual[n], s.a, uint8_t(s.b));
                else SDL_JoystickSetVirtualAxis(g_virtual[n], s.a, int16_t(s.b));
            }
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
    auto ch = [](uint64_t a, int o) { return *reinterpret_cast<const float*>(a + o); };
    RLOG("iodebug: gun0 x %.3f y %.3f | gun3 x %.3f y %.3f | gun3 set2 x %.3f y %.3f | p2 aim %.3f %.3f",
         ch(0x176d2a0, 4), ch(0x176d2e8, 4), ch(0x176d9f0, 4), ch(0x176da38, 4), ch(0x176eb48, 4), ch(0x176ec68, 4),
         g_players[1].aim_x, g_players[1].aim_y);
    for (int gun = 1; gun < kGuns; gun++)
        RLOG("iodebug: gun %d channels x %.3f y %.3f", gun, ch(uint64_t(g_gun_channels[gun][0]), 4), ch(uint64_t(g_gun_channels[gun][1]), 4));
}
}  // namespace

int aim_history(int player, const float** xs, const float** ys) {
    if (player < 0 || player >= game::player_count()) return 0;
    auto& p = g_players[player];
    *xs = p.hist_x;
    *ys = p.hist_y;
    return p.hist_n;
}

bool aim(int player, float* x, float* y) {
    if (player < 0 || player >= game::player_count()) return false;
    *x = g_players[player].aim_x;
    *y = g_players[player].aim_y;
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
            g_players[0].aim_x = std::clamp(gx, 0.0f, 1.0f);
            g_players[0].aim_y = std::clamp(gy, 0.0f, 1.0f);
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
            // Keys bound to an action are ours; the rest (operator keys) go to the game.
            if (e.key.keysym.scancode < SDL_NUM_SCANCODES) g_keys_down[e.key.keysym.scancode] = e.type == SDL_KEYDOWN;
            return event_is_bound(e);
        default:
            return false;
    }
}

const char* controller_name(int player) {
    if (player < 0 || player >= kPlayers || !g_players[player].pad) return "";
    const char* n = SDL_GameControllerName(g_players[player].pad);
    return n ? n : "Controller";
}

bool controller_is_playstation(int player) {
    if (player < 0 || player >= kPlayers || !g_players[player].pad) return false;
    auto t = SDL_GameControllerGetType(g_players[player].pad);
    return t == SDL_CONTROLLER_TYPE_PS3 || t == SDL_CONTROLLER_TYPE_PS4 || t == SDL_CONTROLLER_TYPE_PS5;
}

int player_of_controller(int32_t joystick_id) {
    for (int i = 0; i < kPlayers; i++)
        if (g_players[i].pad && g_players[i].id == joystick_id) return i;
    return -1;
}

void reassign_controllers() {
    for (int i = 0; i < kPlayers; i++)
        if (g_players[i].pad) close_pad(g_players[i].id);
    for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
}

void frame() {
    static bool init = false;
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks64();
    if (!init) {
        init = true;
        g_start = last = now;
        install_gun_channels();
        load_script();
        for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
    }
    float dt = std::min(0.1f, float(now - last) / 1000.0f);
    last = now;
    ensure_bindings();
    run_script();
    game::update_movie_skip();
    bool movie_prompt = game::movie_skip_prompt();
    struct SlotState {
        bool trigger = false, alt1 = false, alt2 = false, start = false;
    } slots[kGuns];
    for (int i = 0; i < game::player_count(); i++) {
        auto& p = g_players[i];
        int slot = game::player_slot(i);
        update_led(p, slot);
        // Player 1 has the keyboard and mouse (with or without a controller); each player their own controller
        // bindings.
        auto on = [&](Action a) {
            return pad_action(i, p.pad, a) || (i == 0 && key_action(a, reinterpret_cast<const unsigned char*>(g_keys_down), g_mouse_buttons));
        };
        bool trig = on(Action::Fire), alt1 = on(Action::Reload), alt2 = on(Action::Grenade), start = on(Action::Start);
        // Mission movies: once the skip prompt is up, Start skips (as the reload press the movie waits for) and
        // Reload doesn't.
        if (movie_prompt) {
            if (start && !p.start_held) p.skip_frames = 3;
            alt1 = false;
        }
        p.start_held = start;
        if (p.skip_frames > 0) alt1 = true, p.skip_frames--;
        float dx = float(on(Action::AimRight)) - float(on(Action::AimLeft));
        float dy = float(on(Action::AimDown)) - float(on(Action::AimUp));
        stick_aim(p, dt, dx, dy);
        p.trigger = trig;
        if (slot >= 0) slots[slot] = {trig, alt1, alt2, start};
        rumble(p, now);
    }
    // Every slot follows its player; a slot nobody plays (or one a player just left for another ODST) is released.
    for (int k = 0; k < kGuns; k++) {
        g_slots[k].trigger.set(kKeys[k].trigger, slots[k].trigger);
        g_slots[k].alt1.set(kKeys[k].alt1, slots[k].alt1);
        g_slots[k].alt2.set(kKeys[k].alt2, slots[k].alt2);
        g_slots[k].start.set(kKeys[k].start, slots[k].start);
    }
    write_aim();
    io_debug();
    static bool trig_debug = std::getenv("RAVEN_TRIGDEBUG") != nullptr;
    if (trig_debug) {
        static int last[4] = {-2, -2, -2, -2};
        const char* names[4] = {"SHOW_MOVIE_SKIP", "MOVIE_SKIP", "intro_movie_done", "MOVIE_START"};
        for (int i = 0; i < 4; i++) {
            int c = game::trigger_set_count(names[i]);
            if (c != last[i]) RLOG("trigdebug: %s %d", names[i], c), last[i] = c;
        }
    }
}
}  // namespace raven::input
