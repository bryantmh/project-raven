# Display and presentation

## Resolution, aspect, frame rate

- The game is started with `-f<W>x<H>` (render size: the configured resolution, else the display's) and
  `-a<W>x<H>`. The startup check that rejects aspects above 2.0 is patched out. raven.exe is per-monitor DPI aware
  (otherwise a 3440x1440 display at 125% looks like 2752x1152).
- The game draws "framebuffer 0" into a framebuffer of its render size (gl_bridge.cpp redirects binds of 0, draw/read
  buffers and binding queries), which is scaled to fill the window on present. When the window changes shape, only
  the game's projection aspect (`0x1cc1fac`, read every frame) follows it, so the 3D view and UI match the window
  with no bars. Rebuilding the game's render targets mid-game (re-running `0x4ef630`) is not possible: the next
  level load then never finishes.
- Display modes: windowed (resizable, size remembered), borderless (an undecorated window one pixel taller than the
  display, so the driver doesn't promote it to exclusive fullscreen and the monitor never switches modes), exclusive
  fullscreen. All switch at once from the menu.
- `tools/hidden_run.py --resize SEC:W:H --winlog SEC` resizes the window from outside and logs its style, size and
  the display mode, for testing these.
- The game is delta-time driven (frame time at `0x1dc416c`), so high refresh rates work natively; VSync and a frame
  rate limit are settings (src/sdl/sdl_bridge.cpp).

## UI layout (src/game/ui_aspect.cpp)

The 2D layer is laid out on a 1920x1080 canvas and stretched on wider windows. Each 2D element is drawn with one
matrix through `glUniformMatrix4fv`; we adjust those matrices (affine, pixel-scaled ones only, so 3D and full-screen
passes are untouched):

- **16:9**: undo the stretch about the screen center (the canvas shows as a centered 16:9 area).
- **Corners** (default, missions only; the menus use 16:9): undo the stretch about each element's center; elements in the outer 20% of the canvas keep
  their distance to the nearest real screen edge, the rest to the center.
- Menus vs missions: the game's scene manager (`0x494fa0` starts a scene, its name at +0) is hooked; the last of
  `level_<n>` (mission) or `attract_2d` / `game_select*` / `chaptersel` (menus) started decides
  (`game::in_mission()`, src/game/view_aspect.cpp; `RAVEN_SCENEDEBUG=1` logs every scene start). A 3D camera is no
  signal: the menu backgrounds are 3D scenes.
- In the menus the game hit-tests the aim against its unadjusted canvas, so the menu crosshair is laid out like
  every other element (16:9: scaled toward the center, a linear map) and lands on the item the game highlights.
  The mouse aim is scaled out by the same factor so the crosshair stays under the pointer, and the stick moves
  faster on that axis to keep its on-screen speed (`game::menu_aim_squeeze`). Corners isn't used in menus: it moves
  elements past 60% of the width outward, which pulls items apart from their labels and would make a crosshair jump.
- In missions crosshairs are left where the game draws them, so they stay on what the shot hits. The game smooths the guns, so
  while aiming moves a crosshair trails the aim by 2–4 frames and matches no exact aim value; picking crosshairs by
  position made them flip between moved and unmoved (the reticle "jumped"). Instead they are recognized by bound
  texture and shape, y scale over x scale (`gl::bound_texture0()`); not size, since crosshairs pulse while firing.
  An element is learned as a crosshair once it has travelled across the screen and then either sits on an aim
  resting off-center or follows the recent aim path (`input::aim_history`) for 5 draws. Gameplay crosshairs get new
  texture ids per level and are learned within their first few frames. Learned ones are exempt from then on.
  `RAVEN_XHAIRDEBUG=1` logs every element drawn near the aim (game position, drawn position, aim).

Changing the projection per camera instead does not work: full-screen layers (menu backgrounds, the 3D view composite)
are drawn through the same 2D cameras and get squeezed.

## Script edits (src/game/script_patches.cpp)

The UI is scripted ("programs", decrypted from `data/prod/archives`). `ArchiveLoad` (`0x5a8960`) is wrapped and these
files are edited as they load (`RAVEN_DUMP_SCRIPTS=<folder>` saves every decrypted script):

| Setting | Files | Edit |
|---|---|---|
| Hide "Money In" / "FREEPLAY" | `attract_2d/moneyin_txt`, `attract_2d/MoneyIn_Cost{,2}`, `moneyin_game/MoneyInGame_Cost{,2}` | HIDE, then sleep forever |
| No menu countdown | `game_select2/gamesel_time{,_r}` | `KILLPRIME` in place of `UI_TIMER_START 30,0` (no timer, no timeout) |
| Hide QR scanner notices | `attract_2d/qr_scanner_{config,p1,p2}`, `perm/webcam_notfound1-4` | HIDE, then sleep forever |

Skip Xbox screen: code patch at `0x4493f8` (docs/functions.md). Boot image: the message screen `0x4fc460` is replaced
by `assets/boot.jpg`, drawn with a framebuffer blit (src/game/boot_screen.cpp).

## Settings menu (src/ui/menu.cpp)

Dear ImGui over the game's GL context. Select / F1 opens it; the game thread stays in the hooked swap running the
menu (the last frame kept in a texture) and FMOD's master channel group is paused. Test hooks: `RAVEN_MENU_AT`,
`RAVEN_MENU_SECONDS`, `RAVEN_MENU_TAB`.

## Icon

`assets/raven.ico`, drawn by `tools/make_icon.py` (original art: a raven over a ring world; no game logos), is
embedded through src/raven.rc and used for the window.

## Testing without a window

`python tools/hidden_run.py --seconds N --env K=V ...` runs raven.exe on its own hidden desktop (muted, no popups).
`RAVEN_SHOT=<path with %d>`, `RAVEN_SHOT_SECONDS` and `RAVEN_SHOT_BURST` save frames.
