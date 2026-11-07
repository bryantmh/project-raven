#pragma once
// Settings, stored as flat JSON in <save>/config.json. Every setting the recomp menu shows lives here.
#include <map>
#include <string>

namespace raven::app {
struct Config {
    // General
    std::string game_folder;  // folder holding halo/ (the original files; never copied)
    std::string fmod_dll;     // FMOD Studio 1.10 runtime (src/audio/fmod_bridge.cpp)
    bool free_play = true;    // operator adjustment playerCost/freeplay (src/game/patches.cpp)

    // Display
    int window_width = 1920, window_height = 1080;  // windowed size
    int display_mode = 0;                           // 0 = windowed, 1 = borderless fullscreen, 2 = exclusive fullscreen
    int render_width = 0, render_height = 0;        // game resolution; 0 = match the window / display
    int aspect = 0;                                 // 0 = auto (the window's shape), 1 = 16:9, 2 = 21:9 (live)
    int ui_layout = 2;                              // 2D UI on wide screens: 0 = stretched (original), 1 = 16:9 centered, 2 = corners
    bool vsync = true;
    int fps_limit = 0;                              // 0 = display refresh (or unlimited without vsync)

    // Presentation (src/game/script_patches.cpp, src/game/boot_screen.cpp)
    bool hide_credit_text = true;   // hide "Money In" / "FREEPLAY"
    bool no_menu_countdown = true;  // no countdown on the game mode select
    bool skip_xbox_screen = true;   // skip the Xbox Live "Get Connected" boot screen
    bool hide_qr_notices = true;    // no QR scanner (webcam) notices
    bool boot_image = true;         // show assets/boot.jpg instead of the boot text
    bool start_skips_movies = true; // Start skips mission movies, with the Start button's icon in the prompt
    bool movies_fill = true;        // mission movies fill wide screens (src/gl/gl_bridge.cpp)
    bool hide_p2_join = false;      // hide player 2's "JOIN NOW / Press START" panel

    // Controls (src/input/input.cpp)
    int rumble = 100;        // controller rumble strength, percent (0 = off)
    int aim_speed = 100;     // controller stick aim speed, percent
    int aim_deadzone = 15;   // controller stick deadzone, percent
    int button_icons = 0;         // Controls tab icons: 0 = match the controller, 1 = Xbox, 2 = PlayStation
    bool kbm_own_player = false;  // keyboard & mouse is player 1 and controllers start at player 2 (else they share)
    std::map<std::string, std::string> binds;  // "pad_<action>" / "key_<action>" -> "slot1|slot2" (src/input/bindings.cpp)

    int version = 4;
};
Config& config();
void load_config();
void save_config();
}  // namespace raven::app
