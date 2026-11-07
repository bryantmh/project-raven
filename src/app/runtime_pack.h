#pragma once
// Runtime files embedded in raven.exe (src/app/runtime_pack.cpp).
namespace raven::app {
// Unpack them (once per build) and set paths().runtime_dir. False if they're missing or couldn't be written.
bool unpack_runtime();
}  // namespace raven::app
