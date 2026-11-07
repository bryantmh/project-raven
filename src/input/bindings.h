#pragma once
// Remappable controls (docs/input.md#remapping). Every action has two controller slots and two keyboard / mouse
// slots; all players share the controller bindings. Stored in config.json as "pad_<action>" / "key_<action>".
#include <string>

union SDL_Event;
struct _SDL_GameController;
typedef struct _SDL_GameController SDL_GameController;

namespace raven::input {
enum class Action {
    Fire,
    Reload,
    Grenade,
    Start,            // join / start
    Menu,             // the recomp settings menu
    AimUp,
    AimDown,
    AimLeft,
    AimRight,
    Count
};
constexpr int kActionCount = int(Action::Count);
constexpr int kSlots = 2;

// One binding. Controller: a button (SDL_GameControllerButton) or a trigger (lt / rt). Keyboard: an SDL scancode
// or a mouse button (1 left, 2 middle, 3 right, 4 / 5 side).
struct Bind {
    enum Kind { None, PadButton, PadTrigger, Key, Mouse } kind = None;
    int code = 0;  // button, trigger (0 = left, 1 = right), scancode or mouse button
    bool operator==(const Bind& o) const { return kind == o.kind && code == o.code; }
    bool is_pad() const { return kind == PadButton || kind == PadTrigger; }
};

const char* action_name(Action a);        // for the menu
Bind pad_bind(Action a, int slot);          // current bindings (defaults until changed)
Bind key_bind(Action a, int slot);
// Set a slot (Bind{} clears it). A binding used by another action on the same device is removed there.
void set_bind(Action a, bool pad, int slot, Bind b);
void reset_binds();                         // back to the defaults
void load_binds();                          // from the config (call after app::load_config)

std::string bind_label(const Bind& b);      // "A", "RT", "Space", "Mouse 1", ... (for text and logs)

// State queries for the input layer.
bool pad_action(SDL_GameController* pad, Action a);
bool key_action(Action a, const unsigned char* keys, unsigned mouse_buttons);
// True if this key or mouse event is bound to any action (the game must not also see it).
bool event_is_bound(const SDL_Event& e);
// True if this event presses the given action (a key down, mouse down, controller button / trigger press).
bool event_presses(const SDL_Event& e, Action a);
// The binding a press event would make, for the menu's "press a button" capture.
Bind bind_from_event(const SDL_Event& e);
}  // namespace raven::input
