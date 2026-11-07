# Project Raven architecture

Halo: Fireteam Raven (Raw Thrills / 343, 2018) is an arcade game that runs on a Linux x86-64 PC inside the
cabinet. Project Raven runs it natively on Windows. The game's code is already x86-64, so nothing is translated
instruction by instruction. Instead `raven.exe` does what Linux's `ld.so` would: it maps the game's ELF image into
memory at its own addresses and supplies Windows implementations of every Linux library the game calls. Wine works
the same way, just limited to this one game. Ultrawide, high frame rates, controllers and the settings menu all hook
in at that library layer.

The game files are never copied or changed. `raven.exe` reads them from the folder you point it at (Settings > Game
folder), and anything the game writes (settings, audits, high scores) goes to Project Raven's own save folder.

## The game files

From `<game>/halo/` (paths below are relative to it):

| File | What it is |
|---|---|
| `game` | The game executable: an ELF64 memory dump of the original `game2`, taken after its Sentinel HASP envelope decrypted it. Exactly 32 MiB, every segment RWX, GOT slots still holding the dumping machine's library addresses (`0x7fd0...`). |
| `halo.so` | An `LD_PRELOAD` library that came with the dump (not Raw Thrills code). It re-resolves the game's imports (`initialize` → `parseStruct`) and inline-hooks the game's HASP calls to its own `hasp_*` functions, which answer from the files in `<game>/dump/`. Built with capstone + plthook + funchook-style "flh". |
| `Patch`, `utils/rtinit` | Updater and hardware check that the cabinet runs before `game2`. Not needed. |
| `data/` | Assets: `g7/*.g7` (models, textures, animations: the "g7" engine), `snd/banks` (FMOD Studio banks), `webm/` (videos), `glcgz/`, `prod/` (configs, shaders). |
| `pmuser/` | Operator data the game writes: adjustments, audits, calibration. |
| `utils/run_game.sh` | How the cabinet starts it: `LD_PRELOAD=halo.so game2 -f<W>x<H> -n`, resolution picked from the cabinet type the dongle reports (1920x1080 on everything but the dual-screen 3880x1080 cabinet). Exit codes 222-226 mean restart / reboot / patch / factory reset / power cycle. |

### The embedded import table

The dump keeps the protector's own import list, which `halo.so`'s `parseStruct` (halo.so `0x4f14`) walks:

- table at `0x1f4d938`, entries `0x24` bytes, walked while the dword at `+0x20` is 6 (GLOB_DAT) or 7 (JUMP_SLOT)
- entry `+0x00` u32 name offset, `+0x04` u32 version offset (both from the string base `0x1f492cc`), `+0x08` u64 GOT
  slot address
- empty version → plain `dlsym`, otherwise `dlvsym`

654 entries: glibc (≈330), GL (≈90), FMOD Studio 1.10 C++ API (≈70), LAPACK (≈25), libiw, libudev, libstdc++ and
a few obfuscated protector names (`Ill1111ll1lllll`, `SEED_encrypt`). On top of these the ELF's normal dynamic
relocations import 182 more glibc/pthread/libdl functions. `tools/imports.py` lists the union with each one's
implementation status.

### Entry

`_start` at `0x414600` is the stock glibc one: `__libc_start_main(main=0x413e70, argc, argv,
init=0x11374e0 (__libc_csu_init), fini=0x1137550, rtld_fini, stack_end)`.

## Loading

1. Reserve `0x400000-0x2400000` before anything else in the process can take it, then copy the two `PT_LOAD`
   segments of `game` in (they are file-backed byte for byte, RWX).
2. Map the Linux C++ runtime (`runtime/linux/libstdc++.so.6`, `libgcc_s.so.1`, fetched from Debian 12 by
   `tools/fetch_runtime.py`) with the same ELF loader, applying their relocations. The game's C++ code calls
   libstdc++ through vtables with the SysV calling convention, so it needs the real Linux build, not MSVC's or
   MinGW's.
3. Resolve every import (embedded table + dynamic relocations) by name: first the shim table (`src/linux/`), then the
   loaded ELF libraries. Anything unresolved points at a per-name stub that logs the name and aborts, so a missing
   function is reported by name instead of crashing at a stale `0x7fd0...` address.
4. Patch the stack-protector reads (next section).
5. Call the game's `_start` on a fresh 8 MiB stack laid out like the kernel's (argc, argv, envp, auxv).

## Calling convention

The game, libstdc++ and libgcc_s use the System V AMD64 ABI. Windows uses the Microsoft x64 ABI. Every function the
game can call is declared `__attribute__((sysv_abi))` in the shim layer, so clang emits the conversion (argument
registers, xmm6-15 preservation) itself. Function pointers the game hands us (thread start routines, `qsort`
comparators, FMOD callbacks, `atexit`) are called through `sysv_abi` pointer types the same way.

- `long` is 64-bit on Linux and 32-bit on Windows: shims use `int64_t` / `uint64_t` for every Linux `long`.
- Variadic functions (`printf` family, `open`, `fcntl`, `ioctl`, `syscall`, `sscanf`): clang has no SysV `va_list`
  on Windows, so each variadic shim is a small naked stub (`SYSV_VARIADIC` in `src/linux/sysv_va.h`) that spills the
  register arguments into a SysV register save area and passes a `sysv_va_list*` to the real implementation, which
  reads arguments with `sysv_va_arg_*`. The `v*` variants receive the game's own SysV `va_list` and use the same
  reader.
- The SysV red zone (128 bytes below `rsp`) is safe because no Windows exception is ever raised inside game code;
  the loader does not rely on fault handlers.

## Thread-local storage

Linux keeps the thread pointer in the FS base. Windows keeps its TEB in GS and does not preserve a user-set FS base
across context switches (measured: `wrfsbase` works, but the base is reset to 0 within milliseconds).

- The game's only FS use is the stack protector: 9,539 sites, every one a 9-byte
  `64 REX.W (8b|33) modrm 25 28000000` (`mov`/`xor reg, qword fs:[0x28]`, `tools/fsscan.py`; halo.so also uses `sub` (2b)). The loader rewrites
  the `64` prefix to `65` (GS) and the displacement to a TEB TLS slot (`0x1480 + 8 * TlsAlloc()`), same length, no
  runtime cost. The canary only has to match itself within one call, so the slot's value (0 on threads the loader
  didn't make) is fine.
- libstdc++ and libgcc_s use the general-dynamic TLS model (no FS access at all): `__tls_get_addr` is a shim that
  returns per-thread blocks allocated on first use.
- `errno` goes through `__errno_location`, a shim backed by a C++ `thread_local`.

## Linux libraries

| Library | Implementation |
|---|---|
| glibc, libpthread, libdl, librt, libm | `src/linux/*.cpp` over the Windows CRT and Win32. Linux struct layouts (`stat`, `tm`, `dirent`, `pthread_*`, `sem_t`, `sockaddr`), flag values (`O_*`, `AF_INET6`) and errno numbers are translated at the boundary. |
| libstdc++, libgcc_s | The real Debian 12 Linux builds, mapped by the ELF loader. |
| libGL, libGLU, GLX | SysV thunks over `opengl32.dll` / WGL, generated from the GL registry. `glXGetProcAddressARB` hands out a thunk for each extension function. |
| SDL2 | Built into the game (2.0.2/2.0.3, no dynamic API, `0x640000`-`0x690000`). Its X11 driver can't run here, so every SDL entry point the game's own code calls (docs/functions.md) is redirected to a real Windows SDL2 through SysV thunks. That gives a native window and GL context, events, and SDL_GameController with rumble. |
| FMOD Studio 1.10 | The game calls the C++ API (`FMOD::Studio::EventInstance::start` and so on). Each method is a SysV thunk to the matching C API function of the Windows FMOD 1.10 DLLs. |
| LAPACK | Fortran calling convention (everything by pointer), so SysV thunks over a Windows LAPACK build. |
| libudev, libiw, HASP | libudev enumerates the I/O board, which is emulated (below). libiw (wireless) reports no devices. HASP stays `halo.so`'s job: the loader maps it like any other library and runs its `initialize`. |

## Arcade I/O

The cabinet's guns, buttons, coin slots, lights and recoil solenoids hang off Raw Thrills' RIO board
(`io_connections.xml` audits `rio[0..]`). The loader emulates the board and drives it from SDL game controllers:
aim from the right stick (or gyro), trigger from RT, grenade/action from the other buttons, recoil and damage
mapped to rumble (and DualSense trigger effects).

## Display

- Ultrawide: the game is started with `-f<W>x<H>` at the window's size, and its projection setup is patched so the
  horizontal field of view grows with the aspect (Hor+) while the HUD stays anchored.
- High frame rate: native if the game's simulation tolerates a variable timestep; otherwise frames are interpolated
  between the game's fixed ticks.
