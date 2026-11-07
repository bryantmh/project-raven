#pragma once
// Mission movies skip with Start (src/game/movie_skip.cpp).
namespace raven::game {
// Once per frame, before input: follows the movie's skip prompt through the game's triggers.
void update_movie_skip();
// True while a movie's skip prompt is up (and the "Start skips mission movies" setting is on).
bool movie_skip_prompt();
}  // namespace raven::game
