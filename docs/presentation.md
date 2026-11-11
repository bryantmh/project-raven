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
  their distance to the nearest real screen edge, the rest to the center. On the 4-player cabinet each ODST's HUD
  fills a quarter of the canvas, so the outer quarters go to the edges whole (a 20% band left Graves's and Lang's kill
  counters behind) and the inner two stay centered.
- Menus vs missions: the game's scene manager (`0x494fa0` starts a scene, its name at +0) is hooked; the last of
  `level_<n>` (mission) or `attract_2d` / `game_select*` / `chaptersel` (menus) started decides
  (`game::in_mission()`, src/game/view_aspect.cpp; `RAVEN_SCENEDEBUG=1` logs every scene start). A 3D camera is no
  signal: the menu backgrounds are 3D scenes.
- In the menus the game hit-tests the aim against its unadjusted canvas, so the menu crosshair is laid out like
  every other element (16:9: scaled toward the center, a linear map) and lands on the item the game highlights.
  The mouse aim is scaled out by the same factor so the crosshair stays under the pointer, and the stick moves
  faster on that axis to keep its on-screen speed (`game::menu_aim_squeeze`). Corners isn't used in menus: it moves
  elements past 60% of the width outward, which pulls items apart from their labels and would make a crosshair jump.
- Two kinds of element keep the game's own matrix. They can't be told apart when the matrix is uploaded (the game
  sets the element's vertex pointer after it), so the adjusted matrix is uploaded as usual and stashed; around the
  next draw call (`ui_before_draw` / `ui_after_draw`, called from gl_bridge.cpp's glDrawArrays / glDrawElements /
  glDrawRangeElements wrappers) the draw's mesh and texture decide, and if needed the original matrix is uploaded
  for that draw only and the adjusted one put back after.
  - **Screen overlays** (the turret view frame in Mission 1, ...) stretch to the screen in every layout. They are
    recognized by extent: the vertices the draw uses (from the indices, or `first` / `count`) mapped through the
    matrix reach |x| >= 0.97 on both sides. Positions come from client memory, or for meshes in a vertex buffer are
    read back once with `glGetBufferSubData` and cached. A single quad exactly the size of the canvas is not an
    overlay: the game draws some canvas content into a texture first and shows it that way (the mission select's
    highlight frame), and movie planes are such quads too (filled by `draw_movie_scaled`). Those are laid out like
    other elements; stretching them put the highlight frame off its tile and stretched movies twice.
    In missions a quad as tall as the canvas (each player's damage tint over their half or quarter) is an overlay
    too, so the tints still tile the screen.
  - **Crosshairs in missions** stay where the game draws them, on what the shot hits. The game smooths the guns, so
    a moving crosshair trails the aim by 2-4 frames and matches no exact aim value; picking crosshairs by position
    made them flip between moved and unmoved (the reticle "jumped"). Instead they are keyed by bound texture
    (`gl::bound_texture0()`) and sprite: the uv rectangle read from the first enabled 2-component float attribute
    (the HUD packs many icons into one atlas, so the texture alone isn't enough; size isn't used since crosshairs
    pulse while firing). A key is learned as a crosshair when its element sits on an aim resting off-center or
    follows the recent aim path (`input::aim_history`) for 5 draws. Gameplay crosshairs get new texture ids per
    level and are learned within their first few frames.
- Only screen-space matrices are adjusted (|translation| <= 1.5): 3D objects drawn through 2D cameras (menu tiles)
  are left alone.
- Debug: `RAVEN_UIDEBUG=1` logs each element once (texture, sprite, scale, position, extent, what was done);
  `RAVEN_XHAIRDEBUG=1` logs every element drawn near the aim (game position, drawn position, aim);
  `RAVEN_BIGDEBUG=1` logs large elements at upload.

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
| Hide player 2's "JOIN NOW / Press START" panel (off by default) | `moneyin_game/CashPlay_P4_Msg`, `join_plaq_p4`, `join_in_p4_movie`, `NoStart_P4_Msg` | `KILLPRIME` at the start |
| Start skips mission movies | `level_<n>/intro_movie/skip_text` | HIDE (the menu code draws its own prompt) |

Skip Xbox screen: code patch at `0x4493f8` (docs/functions.md). Boot image: the message screen `0x4fc460` is replaced
by `assets/boot.jpg`, drawn with a framebuffer blit (src/game/boot_screen.cpp).

## Mission movies

- **Fill wide screens** (src/gl/gl_bridge.cpp, "Mission movies fill wide screens"): movies are 16:9 frames (1920x1079,
  YUV planes on texture units 0-2) with the film, about 2.21:1 (1920x867), letterboxed inside. The game fits the frame
  to the screen height. A movie texture is recognized by its repeated large uploads (`gl::is_video_texture`). Draw
  calls that sample one on unit 0 over the full frame get a viewport scaled by the view aspect over 16:9 around the
  center: the film fills the width (a 2.39:1 screen cuts about 7% of its height). On 16:9 nothing changes.
- **Start skips** (src/game/movie_skip.cpp, "Start skips mission movies"): the movie script `level_<n>/intro_movie/
  movie_skip` shows its prompt after any input (trigger `SHOW_MOVIE_SKIP`), then waits for a reload press
  (`GAME_WAITFORINPUT 1,1,1,1,0,1,0,1`) and fires `MOVIE_SKIP`; movies end with `intro_movie_done` /
  `outro_movie_done`. Trigger set counts are polled each frame (src/game/triggers.cpp). While the prompt is up, a Start
  press sends that player's reload press for 3 frames and the Reload action is ignored. The game's "Press RELOAD to
  skip" texture (`intro_movie/skip_text`) is hidden, and the menu code draws "Press <icon> to skip" in Bahnschrift with
  the Start binding's icon (the controller's, else the keyboard key).

## Settings menu (src/ui/menu.cpp)

Dear ImGui over the game's GL context. Select / F1 opens it; the game thread stays in the hooked swap running the
menu (the last frame kept in a texture) and FMOD's master channel group is paused. Test hooks: `RAVEN_MENU_AT`
(open after that many seconds), `RAVEN_MENU_SECONDS` (close after), `RAVEN_MENU_TAB` (0 General, 1 Gameplay,
2 Display, 3 Controls, 4 Presentation), `RAVEN_MENU_SCROLL`, `RAVEN_MENU_ICONS` (1 Xbox, 2 PlayStation). Periodic shots
(`RAVEN_SHOT_SECONDS`) keep running while it's open; the script's `shot:` doesn't, since game input is paused.

## Icon

`assets/raven.ico`, drawn by `tools/make_icon.py` (original art: a raven over a ring world; no game logos), is
embedded through src/raven.rc and used for the window.

## Testing without a window

`python tools/hidden_run.py --seconds N --env K=V ...` runs raven.exe on its own hidden desktop (muted, no popups).
`RAVEN_SHOT=<path with %d>`, `RAVEN_SHOT_SECONDS` and `RAVEN_SHOT_BURST` save frames. Runs default to a
1720x720 window (`--res WxH` sets `RAVEN_TEST_RES`, which forces windowed at that window and render size) to keep
test load low. Give each run a `RAVEN_SAVE_DIR` with a config.json whose `game_folder` is set, or the run waits at
the folder picker. `RAVEN_TEST_TRIGGER="sec:name,..."` fires game triggers (src/game/triggers.cpp) to reach scenes
directly: `60:poa_overlay_go,61:s1_osb_disp,62:space_battle_transition_go` reaches the Mission 1 turret overlay.
`RAVEN_TRIGDEBUG=1` logs trigger set counts.
