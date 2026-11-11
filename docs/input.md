# Input

The game reads all input through its I/O core: physical inputs are numbered (SDL scancodes 0x000-0x1ff from the PC
driver, 0x201+ analog channels and cabinet boards), and game code asks for logical switches and axes through a
physical-to-logical remap table (`IoRemapAdd`, docs/functions.md#input). Every frame the core copies each remap's
physical channel over its logical channel (`0x4ad6e8`), and game code only uses a gun whose axes have a remap source
(`0x4acfe0`).

## Players and guns

Each gun slot is a seat at the cabinet, and each seat is one ODST: the seat decides the character, its JOIN NOW
panel, HUD quarter and color. The 2-player cabinets (types 4 and 8) use slots 0 and 3: the join loop (`0x4269e0`)
skips slots 1 and 2, and slot 3's gun enable is the operator adjustment `hardware/p2gun` (`0x4178d0`). With
**Settings > Gameplay > Players: Up to 4** the game runs as the 4-player single-screen cabinet (type 3,
docs/functions.md#cabinet-type) and uses all four. `hardware/p1gun`-`p4gun` are set to 1 at startup
(src/game/players.cpp); off, the game shows `GUN<n>_DISABLED` instead of letting the player join.

| Slot | ODST | Aim X / Y (logical) | Second aim pair | Fire / reload / grenade | Start | Virtual keys | Aim channels |
|---|---|---|---|---|---|---|---|
| 0 | Ethan Graves | `0x206` / `0x207` | `0x2a3` / `0x2a4` | `0x208` / `0x209` / `0x20a` | `0x24e` | F13-F16 | mouse `0x201` / `0x202` |
| 1 | Marcus Hudson | `0x212` / `0x213` | `0x2a6` / `0x2a7` | `0x214` / `0x215` / `0x216` | `0x24f` | F21-F24 | `0x212` / `0x213` |
| 2 | Victor Ramos | `0x21e` / `0x21f` | `0x2a9` / `0x2aa` | `0x220` / `0x221` / `0x222` | `0x250` | `0xe8`-`0xeb` | `0x214` / `0x215` |
| 3 | Ava Lang | `0x22a` / `0x22b` | `0x2ac` / `0x2ad` | `0x22c` / `0x22d` / `0x22e` | `0x251` | F17-F20 | `0x210` / `0x211` |

Players pick their ODST in **Settings > Controls** (config `odst_p<n>`, the slot). By default player 1 is Graves and
player 2 is Lang (2-player) or Hudson (4-player). A choice the cabinet doesn't have, or one an earlier player already
took, falls back to the first free seat (`game::player_slot`). Picking another player's ODST in the menu swaps the
two. The virtual keys and aim channels belong to slots: every frame each slot takes its player's state, so changing
ODST mid-game releases the old seat's buttons.

## What Project Raven does (src/input/input.cpp)

- **Mouse mode** is forced on (src/game/patches.cpp): the remap builder (`0x41baf0`) binds the PC driver's mouse X/Y
  (physical `0x201` / `0x202`) to both aim pairs of every gun, and gun calibration is skipped.
- **Player 1 aim**: the gun code wants 0..1 (Y from the bottom: the mouse Y channel's flags flip it), but the driver
  stores `SDL_MOUSEMOTION`'s pixel coordinates. So mouse events never reach the game; every frame we write player 1's
  normalized aim into the mouse channels with `AnalogSet` (`[0x180adf0]+0x6000` / `+0x6048`).
- **Guns 1-3 aim**: the driver's physical analog lookup (`0x61ee70`, a table of 32-bit offsets at `0x11bcd70`) has
  no channel for IDs `0x210`-`0x215`. We point those entries at stubs (allocated just above the game image)
  returning channels of our own, remap them to guns 1-3's aim pairs instead of the mouse, and write each gun's
  player's aim (Y as 1 - y) into them every frame. Gun 0's player's aim goes into the mouse channels.
- **Buttons**: we add remap entries from scancodes no keyboard sends to the cabinet switches (table above: each
  slot's fire, reload, grenade and start, in that order), and press those keys. Fixed operator keys:

  | Key | Logical | Meaning |
  |---|---|---|
  | 5 | `0x257` | Cab0Coin0 |
  | F2 | `0x256` | CabDiag (operator test menu) |
  | F3 | `0x288` | Cab0Service |

- **Controllers** (SDL_GameController: Xbox, PlayStation, ...) fill the players in order, up to 2 or 4 (from
  player 2 with "keyboard & mouse is player 1"). Either stick aims (radial deadzone, squared response). Rumble pulses
  every 100 ms while firing; PlayStation light bars show the player's ODST color: Graves blue, Hudson orange, Ramos
  green, Lang purple.
- **Keyboard and mouse** always drive player 1.

## Remapping (src/input/bindings.cpp, Settings > Controls)

Actions: fire, reload, grenade, join / start, settings menu, aim up / down / left / right
(digital aim pushes like a fully deflected stick). Each has two controller slots (buttons, or LT / RT) per player, and
two keyboard / mouse slots (player 1's). The Controls tab shows one player's controller buttons at a time ("Buttons
for Player n"), and can give every player the shown player's buttons. Controller defaults follow Halo (Reach / MCC): RT fire, LT / LB grenade,
X / RB reload. Bindings stored for actions that no longer exist are dropped on load. They are stored in config.json as `"pad_<action>": "slot1|slot2"` (player 1;
`"p<n>_pad_<action>"` for players 2-4, which start from player 1's when missing; SDL controller button names,
`lefttrigger` / `righttrigger`) and `"key_<action>"` (SDL scancode names, `mouse1`-`mouse5`).

- Binding a button that another action uses on the same device (and player) removes it there.
- The settings-menu button uses the bindings of the player holding that controller.
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
  - `move:<x>:<y>`, `movb` / `movc` / `movd`: set player 1, 2, 3 or 4's aim (1920x1080 space).
  - `cursor:<x>:<y>`: mouse motion in window pixels.
  - `mdown/mup:<button>`: mouse buttons.
  - `pad:<0-3>`: attach virtual controller 0-3, driven by `pbtn` / `paxis` (controller 0) and `pbtnb` / `paxisb`,
    `pbtnc` / `paxisc`, `pbtnd` / `paxisd` (controllers 1-3).
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
