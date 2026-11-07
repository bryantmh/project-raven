# Input

The game reads all input through its I/O core: physical inputs are numbered (SDL scancodes 0x000-0x1ff from the PC
driver, 0x201+ analog channels and cabinet boards), and game code asks for logical switches and axes through a
physical-to-logical remap table (`IoRemapAdd`, docs/functions.md#input). Every frame the core copies each remap's
physical channel over its logical channel (`0x4ad6e8`), and game code only uses a gun whose axes have a remap source
(`0x4acfe0`).

## Players and guns

The 2-player cabinet uses player slots 0 and 3: the join loop (`0x4269e0`) skips slots 1 and 2 on this cabinet type,
and slot 3's gun enable is the operator adjustment `hardware/p2gun` (`0x4178d0`). So player 2 is **gun 3**. Both
`hardware/p1gun` and `p2gun` are set to 1 at startup (src/game/patches.cpp); off, the game shows `GUN<n>_DISABLED`
instead of letting the player join.

| | Gun | Aim X / Y (logical) | Second aim pair | Fire / reload / grenade | Start |
|---|---|---|---|---|---|
| Player 1 | 0 | `0x206` / `0x207` | `0x2a3` / `0x2a4` | `0x208` / `0x209` / `0x20a` | `0x24e` |
| Player 2 | 3 | `0x22a` / `0x22b` | `0x2ac` / `0x2ad` | `0x22c` / `0x22d` / `0x22e` | `0x251` |

## What Project Raven does (src/input/input.cpp)

- **Mouse mode** is forced on (src/game/patches.cpp): the remap builder (`0x41baf0`) binds the PC driver's mouse X/Y
  (physical `0x201` / `0x202`) to both aim pairs of every gun, and gun calibration is skipped.
- **Player 1 aim**: the gun code wants 0..1 (Y from the bottom: the mouse Y channel's flags flip it), but the driver
  stores `SDL_MOUSEMOTION`'s pixel coordinates. So mouse events never reach the game; every frame we write player 1's
  normalized aim into the mouse channels with `AnalogSet` (`[0x180adf0]+0x6000` / `+0x6048`).
- **Player 2 aim**: the driver's physical analog lookup (`0x61ee70`, a table of 32-bit offsets at `0x11bcd70`) has
  no channel for IDs `0x210` / `0x211`. We point those entries at stubs (allocated just above the game image)
  returning two channels of our own, remap them to gun 3's aim pairs instead of the mouse, and write player 2's aim
  (Y as 1 - y) into them every frame.
- **Buttons**: we add remap entries from scancodes no keyboard sends to the cabinet switches, and press those keys:

  | Key | Logical | Meaning |
  |---|---|---|
  | F13 / F17 | `0x208` / `0x22c` | player 1 / 2 fire |
  | F14 / F18 | `0x209` / `0x22d` | player 1 / 2 reload |
  | F15 / F19 | `0x20a` / `0x22e` | player 1 / 2 grenade |
  | F16 / F20 | `0x24e` / `0x251` | player 1 / 2 start |
  | 5 | `0x257` | Cab0Coin0 |
  | F2 | `0x256` | CabDiag (operator test menu) |
  | F3 | `0x288` | Cab0Service |

- **Controllers** (SDL_GameController: Xbox, PlayStation, ...) fill the players in order (from player 2 with
  "keyboard & mouse is player 1"). Either stick aims (radial deadzone, squared response). Rumble pulses every 100 ms
  while firing; PlayStation light bars show the player color.
- **Keyboard and mouse** always drive player 1.

## Remapping (src/input/bindings.cpp, Settings > Controls)

Actions: fire, reload, grenade, join / start, settings menu, aim up / down / left / right
(digital aim pushes like a fully deflected stick). Each has two controller slots (buttons, or LT / RT) shared by every
controller, and two keyboard / mouse slots. Controller defaults follow Halo (Reach / MCC): RT fire, LT / LB grenade,
X / RB reload. Bindings stored for actions that no longer exist are dropped on load. They are stored in config.json as `"pad_<action>": "slot1|slot2"` (SDL
controller button names, `lefttrigger` / `righttrigger`) and `"key_<action>"` (SDL scancode names, `mouse1`-`mouse5`).

- Binding a button that another action uses on the same device removes it there.
- Keys bound to an action are kept from the game, so they can't trigger operator switches; unbound keys pass through.
- If the settings-menu action has nothing bound on a device, Select / F1 still opens the menu there.
- Every connected controller drives the menu. ImGui reads controller buttons from their held state, so after a
  capture nothing is cleared until every button and key is released (otherwise the X / Square that was just bound
  would clear its own slot on the next frame).
- The Controls tab draws the button icons with ImGui shapes (src/ui/glyphs.cpp): Xbox or PlayStation style, from the
  first controller's type or the "Button icons" setting.

## Test hooks

- `RAVEN_TEST_KEYS="sec:kind:args,..."` (seconds from the first input frame):
  - `down/up:<scancode>`: keys, through the bindings like the real keyboard.
  - `move:<x>:<y>`, `movb:<x>:<y>`: set player 1 or player 2 aim (1920x1080 space).
  - `cursor:<x>:<y>`: mouse motion in window pixels.
  - `mdown/mup:<button>`: mouse buttons.
  - `pad:<0|1>`: attach virtual controller 0 or 1, driven by `pbtn` / `paxis` (controller 0) and `pbtnb` / `paxisb` (controller 1).
  - `shot:<n>`: save the next n frames.
  - `aspect:<n>`: change the aspect ratio setting.
- `RAVEN_INPUTDEBUG=1` logs every virtual key press. `RAVEN_IODEBUG=1` logs the aim channels.
- `RAVEN_MENU_CAPTURE=<action>:<pad 0|1>:<slot>:<code>` focuses a slot and rebinds it through the menu's capture; with
  a virtual controller attached (`pad:0`, before the menu opens) the button is held on it for 0.5 s like a real
  press. `RAVEN_MENU_PRESS=<sec>:<button>` holds a virtual controller button later (e.g. X to clear the focused slot).
  `RAVEN_MENU_ICONS` forces the icon style, and `RAVEN_MENU_SCROLL` scrolls the open tab.
- `RAVEN_SAVE_DIR` keeps a test run's settings away from the real ones.
- In test runs (`RAVEN_HEADLESS`) real controllers are never opened, and the menu reads only the virtual ones, so tests
  can't rumble or read the user's controllers.
