// Remappable controls: defaults, storage, and matching. docs/input.md#remapping
#include "input/bindings.h"

#include <SDL.h>

#include <cstring>
#include <map>

#include "app/config.h"
#include "log.h"

namespace raven::input {
namespace {
struct ActionInfo {
    const char* key;   // config suffix
    const char* name;  // menu label
    Bind pad[kSlots], keys[kSlots];
};
constexpr Bind P(SDL_GameControllerButton b) { return {Bind::PadButton, int(b)}; }
constexpr Bind T(int trigger) { return {Bind::PadTrigger, trigger}; }
constexpr Bind K(SDL_Scancode s) { return {Bind::Key, int(s)}; }
constexpr Bind M(int button) { return {Bind::Mouse, button}; }
constexpr Bind N{};

// Defaults: Halo's controller layout (Reach / MCC: RT fire, LT grenade, X reload; RB reload as in Halo 3, LB a
// second grenade button), mouse buttons and Halo PC keys (R reload, G grenade) on the keyboard side.
const ActionInfo kDefaults[kActionCount] = {
    {"fire", "Fire", {T(1), N}, {M(SDL_BUTTON_LEFT), K(SDL_SCANCODE_SPACE)}},
    {"reload", "Reload", {P(SDL_CONTROLLER_BUTTON_X), P(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)}, {M(SDL_BUTTON_RIGHT), K(SDL_SCANCODE_R)}},
    {"grenade", "Grenade", {T(0), P(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)}, {M(SDL_BUTTON_MIDDLE), K(SDL_SCANCODE_G)}},
    {"start", "Join / Start", {P(SDL_CONTROLLER_BUTTON_START), N}, {K(SDL_SCANCODE_1), K(SDL_SCANCODE_RETURN)}},
    {"menu", "Settings menu", {P(SDL_CONTROLLER_BUTTON_BACK), N}, {K(SDL_SCANCODE_F1), K(SDL_SCANCODE_TAB)}},
    {"aim_up", "Aim up", {P(SDL_CONTROLLER_BUTTON_DPAD_UP), N}, {K(SDL_SCANCODE_UP), K(SDL_SCANCODE_W)}},
    {"aim_down", "Aim down", {P(SDL_CONTROLLER_BUTTON_DPAD_DOWN), N}, {K(SDL_SCANCODE_DOWN), K(SDL_SCANCODE_S)}},
    {"aim_left", "Aim left", {P(SDL_CONTROLLER_BUTTON_DPAD_LEFT), N}, {K(SDL_SCANCODE_LEFT), K(SDL_SCANCODE_A)}},
    {"aim_right", "Aim right", {P(SDL_CONTROLLER_BUTTON_DPAD_RIGHT), N}, {K(SDL_SCANCODE_RIGHT), K(SDL_SCANCODE_D)}},
};

Bind g_pad[kActionCount][kSlots], g_keys[kActionCount][kSlots];

// ---- text form: "a|righttrigger" (controller), "mouse1|Space" (keyboard); "" for an empty slot
std::string to_text(const Bind& b) {
    switch (b.kind) {
        case Bind::PadButton: {
            const char* s = SDL_GameControllerGetStringForButton(SDL_GameControllerButton(b.code));
            return s ? s : "";
        }
        case Bind::PadTrigger: return b.code ? "righttrigger" : "lefttrigger";
        case Bind::Key: return SDL_GetScancodeName(SDL_Scancode(b.code));
        case Bind::Mouse: return "mouse" + std::to_string(b.code);
        default: return "";
    }
}
Bind from_text(const std::string& s, bool pad) {
    if (s.empty()) return {};
    if (pad) {
        if (s == "lefttrigger") return T(0);
        if (s == "righttrigger") return T(1);
        auto b = SDL_GameControllerGetButtonFromString(s.c_str());
        return b == SDL_CONTROLLER_BUTTON_INVALID ? Bind{} : P(b);
    }
    if (s.rfind("mouse", 0) == 0 && s.size() == 6) return M(s[5] - '0');
    auto sc = SDL_GetScancodeFromName(s.c_str());
    return sc == SDL_SCANCODE_UNKNOWN ? Bind{} : K(sc);
}
void store(Action a) {
    auto& cfg = app::config().binds;
    const char* key = kDefaults[int(a)].key;
    cfg[std::string("pad_") + key] = to_text(g_pad[int(a)][0]) + "|" + to_text(g_pad[int(a)][1]);
    cfg[std::string("key_") + key] = to_text(g_keys[int(a)][0]) + "|" + to_text(g_keys[int(a)][1]);
}

float trigger(SDL_GameController* pad, int t) {
    return SDL_GameControllerGetAxis(pad, t ? SDL_CONTROLLER_AXIS_TRIGGERRIGHT : SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
}
// The last trigger value per controller, for press edges. Several callers may look at the same event, so the
// result for the last event is remembered.
std::map<SDL_JoystickID, int> g_last_trigger[2];
struct {
    Uint32 timestamp = 0;
    SDL_JoystickID which = -1;
    int axis = -1, value = 0;
    bool press = false;
} g_trigger_event;
}  // namespace

const char* action_name(Action a) { return kDefaults[int(a)].name; }
Bind pad_bind(Action a, int slot) { return g_pad[int(a)][slot]; }
Bind key_bind(Action a, int slot) { return g_keys[int(a)][slot]; }

void set_bind(Action a, bool pad, int slot, Bind b) {
    auto& table = pad ? g_pad : g_keys;
    if (b.kind != Bind::None)
        for (int i = 0; i < kActionCount; i++)
            for (int s = 0; s < kSlots; s++)
                if (table[i][s] == b && !(i == int(a) && s == slot)) {
                    table[i][s] = {};
                    store(Action(i));
                }
    table[int(a)][slot] = b;
    store(a);
}

void reset_binds() {
    for (int i = 0; i < kActionCount; i++) {
        for (int s = 0; s < kSlots; s++) g_pad[i][s] = kDefaults[i].pad[s], g_keys[i][s] = kDefaults[i].keys[s];
        store(Action(i));
    }
}

void load_binds() {
    auto& cfg = app::config().binds;
    // Drop stored bindings of actions that no longer exist.
    for (auto it = cfg.begin(); it != cfg.end();) {
        bool known = false;
        for (auto& d : kDefaults) known |= it->first == std::string("pad_") + d.key || it->first == std::string("key_") + d.key;
        it = known ? std::next(it) : cfg.erase(it);
    }
    for (int i = 0; i < kActionCount; i++) {
        for (int s = 0; s < kSlots; s++) g_pad[i][s] = kDefaults[i].pad[s], g_keys[i][s] = kDefaults[i].keys[s];
        for (bool pad : {true, false}) {
            auto it = cfg.find(std::string(pad ? "pad_" : "key_") + kDefaults[i].key);
            if (it == cfg.end()) continue;
            auto bar = it->second.find('|');
            std::string a = it->second.substr(0, bar), b = bar == std::string::npos ? "" : it->second.substr(bar + 1);
            (pad ? g_pad : g_keys)[i][0] = from_text(a, pad);
            (pad ? g_pad : g_keys)[i][1] = from_text(b, pad);
        }
    }
}

std::string bind_label(const Bind& b) {
    switch (b.kind) {
        case Bind::PadButton: {
            static const char* names[] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "LS", "RS", "LB", "RB",
                                          "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Share",
                                          "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"};
            return b.code >= 0 && b.code < int(std::size(names)) ? names[b.code] : "?";
        }
        case Bind::PadTrigger: return b.code ? "RT" : "LT";
        case Bind::Key: return SDL_GetScancodeName(SDL_Scancode(b.code));
        case Bind::Mouse: {
            static const char* names[] = {"", "Left click", "Middle click", "Right click", "Mouse 4", "Mouse 5"};
            return b.code > 0 && b.code < 6 ? names[b.code] : "Mouse";
        }
        default: return "";
    }
}

bool pad_action(SDL_GameController* pad, Action a) {
    if (!pad) return false;
    for (const Bind& b : g_pad[int(a)]) {
        if (b.kind == Bind::PadButton && SDL_GameControllerGetButton(pad, SDL_GameControllerButton(b.code))) return true;
        if (b.kind == Bind::PadTrigger && trigger(pad, b.code) > 0.4f) return true;
    }
    return false;
}

bool key_action(Action a, const unsigned char* keys, unsigned mouse_buttons) {
    for (const Bind& b : g_keys[int(a)]) {
        if (b.kind == Bind::Key && keys && keys[b.code]) return true;
        if (b.kind == Bind::Mouse && (mouse_buttons & SDL_BUTTON(b.code))) return true;
    }
    return false;
}

bool event_is_bound(const SDL_Event& e) {
    Bind b{};
    if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) b = K(e.key.keysym.scancode);
    else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) b = M(e.button.button);
    else return false;
    for (auto& row : g_keys)
        for (const Bind& x : row)
            if (x == b) return true;
    return false;
}

Bind bind_from_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_KEYDOWN: return e.key.repeat ? Bind{} : K(e.key.keysym.scancode);
        case SDL_MOUSEBUTTONDOWN: return M(e.button.button);
        case SDL_CONTROLLERBUTTONDOWN: return P(SDL_GameControllerButton(e.cbutton.button));
        case SDL_CONTROLLERAXISMOTION: {
            int t = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 0 : e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? 1 : -1;
            if (t < 0) return {};
            auto& c = g_trigger_event;
            if (!(c.timestamp == e.caxis.timestamp && c.which == e.caxis.which && c.axis == e.caxis.axis &&
                  c.value == e.caxis.value)) {
                int& last = g_last_trigger[t][e.caxis.which];
                c = {e.caxis.timestamp, e.caxis.which, e.caxis.axis, e.caxis.value, e.caxis.value > 16000 && last <= 16000};
                last = e.caxis.value;
            }
            return c.press ? T(t) : Bind{};
        }
        default: return {};
    }
}

bool event_presses(const SDL_Event& e, Action a) {
    Bind b = bind_from_event(e);
    if (b.kind == Bind::None) return false;
    const auto& row = b.is_pad() ? g_pad[int(a)] : g_keys[int(a)];
    for (const Bind& x : row)
        if (x == b) return true;
    return false;
}
}  // namespace raven::input
