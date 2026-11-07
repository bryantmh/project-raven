# Project Raven

Play the arcade game **Halo: Fireteam Raven** on a Windows PC: with a controller or mouse and keyboard, alone or
with a friend. It contains a few QOL enhancements as well including ultrawide aspect ratios and unlocked framerates

You need your own copy of the arcade game files. Nothing from the game is included here.

## Getting started

1. Download `raven.exe`. It's the only file you need, and you can put it in any folder.
2. Run it. The first time, it asks for your game folder: pick the folder that contains both `halo` and `dump`.
   The game needs both.
3. Press **Start** (or **1** on the keyboard) to join, and shoot to pick a mission.

Your game folder is only read, never changed. Settings and saves go to `%APPDATA%\ProjectRaven`.

## Controls

The controller defaults. You can change any of them in the settings menu.

| Action | Controller | Keyboard and mouse |
|---|---|---|
| Aim | Either stick, or the D-pad | Mouse, or the arrow keys / WASD |
| Fire | RT | Left click or Space |
| Reload | X or RB | Right click or R |
| Grenade | LT or LB | Middle click or G |
| Join / Start | Start | 1 or Enter |
| Settings menu | Select (View / Create) | F1 or Tab |

Rumble works on Xbox and PlayStation controllers.

## Two players

Connect two controllers: the first is player 1, the second is player 2. Each player aims and shoots on their own.
The keyboard and mouse always control player 1.

Want mouse and keyboard for player 1 and a controller for player 2? Turn on **Keyboard & mouse is player 1,
controllers start at player 2** in **Settings > Controls**.

Player 2 joins by pressing Start during a mission.

## Settings menu

Press **Select** or **F1**. The game pauses while the menu is open. **L1 / R1** (or **Q / E**) switch tabs.

- **Display**: windowed, borderless or fullscreen; resolution; aspect ratio (Auto, 16:9, 21:9); how the HUD is laid
  out on wide screens; VSync and frame rate limit.
- **Controls**: remap any action (two controller buttons and two keys each). Click a slot and press the new button.
  Right-click a slot, or press X / Square on it, to clear it. You can also set rumble strength, aim speed and stick
  deadzone. The button icons match your controller (Xbox or PlayStation).
- **Presentation**: hide the arcade's "Money In" and "FREEPLAY" text, the menu countdown, the Xbox Live screen and
  the QR scanner notices. Mission movies fill wide screens, and Start skips them (press once to show the prompt,
  again to skip).

Settings marked "(restart)" apply the next time you start the game. Everything else applies immediately.

## Troubleshooting

- **It asks for the game folder again**: pick the folder that contains both `halo` (with `halo\game` inside it) and
  `dump`.
- **Something went wrong**: the log is `%APPDATA%\ProjectRaven\raven.log`.
- **Start over with default settings**: delete `%APPDATA%\ProjectRaven\config.json`.

## Credits

- Audio: FMOD Studio by Firelight Technologies Pty Ltd.
- OpenBLAS (BSD license), SDL2 (zlib license), Dear ImGui (MIT license), mimalloc (MIT license).
- The GNU C++ runtime (libstdc++ and libgcc from Debian 12, GPL with the GCC Runtime Library Exception). Its source
  is at <https://sources.debian.org/src/gcc-12/>.
- Halo and Fireteam Raven are trademarks of Microsoft. This project isn't affiliated with Microsoft, 343 Industries/ Halo Studios,
  Play Mechanix or Raw Thrills.

## For developers

Building needs clang (LLVM for Windows), CMake, Ninja, Python 3 and 7-Zip:

    python tools/fetch_runtime.py
    python tools/fetch_fmod.py <path to fmodstudioapi11020win-installer.exe>
    cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_RC_COMPILER=llvm-rc
    cmake --build build

The result is `build\raven.exe`, with every runtime file embedded in it. It unpacks them on first run into
`%LOCALAPPDATA%\ProjectRaven\runtime`.

Releases: run `git config core.hooksPath tools/hooks` once per clone. Then every push of `master` to a GitHub remote
builds that commit in `.release` (so uncommitted changes never ship) and publishes `raven.exe` as a GitHub release
tagged from `VERSION` (`tools/release.py`; needs the `gh` CLI). `py tools/release.py --build-only` builds without
publishing, and `git push --no-verify` pushes without a release.

How it works and how to test it: [docs/architecture.md](docs/architecture.md), [docs/input.md](docs/input.md),
[docs/presentation.md](docs/presentation.md), [docs/functions.md](docs/functions.md).
