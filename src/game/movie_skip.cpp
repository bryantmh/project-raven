// Mission intro / outro movies: Start skips them instead of Reload. The movie script (level_<n>/intro_movie/
// movie_skip) shows "Press RELOAD to skip" after any input (trigger SHOW_MOVIE_SKIP), then waits for a gun's reload
// button (GAME_WAITFORINPUT 1,1,1,1,0,1,0,1; Start isn't in that mask) and fires MOVIE_SKIP. The movie ends with
// intro_movie_done / outro_movie_done. While the prompt is up, src/input/input.cpp turns a Start press into a reload
// press and ignores the Reload action, and src/ui/menu.cpp draws the prompt with the Start button's icon (the
// game's own text is hidden by src/game/script_patches.cpp).
#include "game/movie_skip.h"

#include "app/config.h"
#include "game/triggers.h"
#include "log.h"

namespace raven::game {
namespace {
bool g_prompt = false;

// True when the trigger fired since the last call (its set count went up).
struct Watch {
    const char* name;
    int last = -2;
    bool fired() {
        int now = trigger_set_count(name);
        bool f = last != -2 && now > 0 && now != last && (last < 0 || now > last);
        last = now;
        return f;
    }
};
Watch g_show{"SHOW_MOVIE_SKIP"}, g_skip{"MOVIE_SKIP"}, g_intro_done{"intro_movie_done"}, g_outro_done{"outro_movie_done"};
}  // namespace

void update_movie_skip() {
    bool show = g_show.fired();
    bool end = g_skip.fired() | g_intro_done.fired() | g_outro_done.fired();
    if (end && g_prompt) RLOG("movie skip: movie over");
    if (end) g_prompt = false;
    if (show && !end) {
        g_prompt = true;
        RLOG("movie skip: prompt shown");
    }
}

bool movie_skip_prompt() { return g_prompt && app::config().start_skips_movies; }
}  // namespace raven::game
