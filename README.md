# Project Raven

Runs the Linux arcade build of Halo: Fireteam Raven natively on Windows. See docs/architecture.md.

You need your own copy of the game files. Nothing from the game is included here, and the game folder is never
modified (writes go to `%APPDATA%\ProjectRaven\overlay`).

## Build

Requires clang (LLVM for Windows), CMake and Ninja.

    python tools/fetch_runtime.py
    cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_RC_COMPILER=llvm-rc
    cmake --build build
    xcopy /E /I runtime build\runtime

Run `build\raven.exe`. The first launch asks for the game folder (the one containing `halo\`).
`RAVEN_TRACE=1` logs every shim call. `RAVEN_HEADLESS=1` logs fatal errors without showing a dialog (for
automated test runs). The log is `%APPDATA%\ProjectRaven\raven.log`.

## Status

- Done: ELF loader, the embedded import table, stack-protector patch, glibc/pthread/libdl/libudev shims, ucontext
  coroutines, the Linux libstdc++ runtime, file system overlay, the SDL bridge (real SDL2 window and GL context),
  GL thunks for every GL command, FMOD Studio 1.10. The game is playable (tested from boot through Mission 1 combat).
- Controls: Xbox/PlayStation controllers (stick aim, fire, reload, grenade, start, rumble), mouse and keyboard
  (docs/input.md).
- Display: any resolution and aspect including ultrawide, 2D UI in corners or 16:9 layout, VSync and frame rate
  limit (the game is delta-time driven, so high refresh rates are native) (docs/presentation.md).
- Settings menu on Select / F1, with presentation options: no credit text, no menu countdown, no Xbox Live screen,
  no QR scanner notices, custom boot image.
- Next: separate aim for player 2.

## FMOD

The game needs an FMOD Studio **1.10** runtime DLL. Put `fmodstudio.dll` (a Unity build, core and studio in one) or
`fmodstudio64.dll` + `fmod64.dll` (the FMOD Engine 1.10.x package from fmod.com) in `runtime\windows\`, or set
`fmod_dll` in config.json. Without one the game runs silent. `RAVEN_MUTE=1` selects FMOD's no-sound output.
`RAVEN_SHOT=<file.bmp>` saves the frame every `RAVEN_SHOT_SECONDS`, and `RAVEN_SAMPLE=<s>` logs where the game
thread is (test runs).
