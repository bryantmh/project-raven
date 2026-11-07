# Input

The game reads all input through its I/O core: physical inputs are numbered (SDL scancodes 0x000-0x1ff from the PC
driver, 0x201+ analog channels and cabinet boards), and game code asks for logical switches and axes through a
physical-to-logical remap table (`IoRemapAdd`, docs/functions.md#input).

## What Project Raven does (src/input/input.cpp)

- **Mouse mode** is forced on (src/game/patches.cpp): the remap builder then binds the PC driver's mouse X/Y
  channels to every gun's aim, and gun calibration is skipped.
- **Aim**: the gun code wants 0..1 (Y is inverted as 1 - y by the channel flags), but the driver stores
  `SDL_MOUSEMOTION`'s pixel coordinates. So mouse events never reach the game; every frame we write the normalized
  aim into the channels with `AnalogSet` (`[0x180adf0]+0x6000` / `+0x6048`). All guns share this aim for now.
- **Buttons**: we add remap entries from unused scancodes to the cabinet switches and send those keys:

  | Key | Logical | Meaning |
  |---|---|---|
  | F13 / F17 | `0x208` / `0x214` | gun 0/1 trigger (fire) |
  | F14 / F18 | `0x209` / `0x215` | gun 0/1 trigger 1 (reload) |
  | F15 / F19 | `0x20a` / `0x216` | gun 0/1 trigger 2 (grenade) |
  | F16 / F20, 1 / 2 | `0x24e` / `0x24f` | CabStart0/1 |
  | 5 | `0x257` | Cab0Coin0 |
  | F2 | `0x256` | CabDiag (TEST) |
  | S | `0x288` | Cab0Service |
  | Space | `0x208` | fire |

- **Reload by pointing off-screen** (`offscreenReload`): Y / LB / R send the aim to (-0.5, -0.5) while held.
- **Controllers** (SDL_GameController: Xbox, PlayStation, ...), one per player: either stick aims (radial deadzone,
  squared response), RT/A fire, LT/B reload, Y/LB reload off-screen, RB/X grenade, Start join, Select opens the
  settings menu. Rumble pulses every 100 ms while firing. PlayStation light bars show the player color.
- **Mouse**: aim, left fire, right reload, middle grenade.

## Test hooks

`RAVEN_TEST_KEYS="sec:kind:args,..."`: `down/up:<scancode>`, `move:<x>:<y>` (1920x1080 space), `cursor:<x>:<y>` (mouse motion in window pixels), `mdown/mup:<button>`,
`pad:0` (attach a virtual controller), `pbtn:<button>:<0|1>`, `paxis:<axis>:<value>`. `RAVEN_IODEBUG=1` logs the
mouse channels. In test runs (`RAVEN_HEADLESS`) real controllers are never opened.
