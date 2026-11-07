# Known game functions and data

Addresses are in the `game` dump (Welcome to Halo v03.11, Game Rev 57529, Library Rev 1412). Tools:
`tools/disasm.py <addr>` (imports and strings annotated), `tools/xref.py <addr> [hi]`,
`tools/callgraph.py callers|callees|boundary|strings`.

## Startup

| Address | Name | Notes |
|---|---|---|
| `0x414600` | `_start` | stock glibc entry |
| `0x413e70` | `main` | |
| `0x11374e0` / `0x1137550` | `__libc_csu_init` / `__libc_csu_fini` | |
| `0x437a20` | game init sequence | called from main. Calls SysInit `0x4b9450` at `0x437c7b`, then subsystem inits `0x4ba0d0` at `0x437d82` |
| `0x4b9450` | SysInit | `SDL_Init(SDL_INIT_NOPARACHUTE\|SDL_INIT_VIDEO)` (error at `0x4b9bb0`), `ResInit` at `0x4b984c` (error "ResInit() FAILED!" at `0x4b9e64`) |
| `0x4ba0d0` | subsystem inits | calls `0x54fc10`, `0x5a9ce0`, `0x49ae20`, `0x4defc0`, `0x59be40`, `0x55b540`, `0x4ac620`, `0x4fb7e0`, `0x54ca90`, ... |
| `0x596bc0` | `ResInit` | inlines `ResTableInit` (the out-of-line copy at `0x59a710` has no callers) |
| `0x59a710` | `ResTableInit` | allocates the resource hash tables: pointer array `0x1dd75e0..0x1dd76c0` (gResImgByHashTable and friends, 0x28-byte tables made by `0x921b30(tbl, 0x400, alloc=0x4d3c10, free=0x4d2820)`) |
| `0x59be40` | resource table toggle | flag `0x1dd7700`: first call resets every table in `0x1dd75e0[]` (`0x922cc0`), second call frees. Crashes if `ResInit` didn't run (what happens when `VidInit` fails) |
| `0x5a9c90` | `ProcInit` | the game's task ("proc") system: ucontext coroutines (`getcontext`/`makecontext`/`swapcontext`, src/linux/ucontext.cpp) |
| `0x5b92f7` | FMOD init | `FMOD_Memory_Initialize(pool, 0x3200000)`, then Studio::System create/initialize, banks from `data/snd_2p/banks` (MasterBank, MasterBank.strings, perm) |
| `0x550060` | game log printf | writes to `pmuser/gamelog.txt` |
| `0x586280` / `0x586290` | SysInfo set / get | keys like `SysInfoVideoCard`, `SysInfoVideoDriver`, `SysInfoVideoRes`, `SysInfoGameFullName` |
| `0x49ea00` | boot file test | "Press START or DIAG to skip File Test.": hashes every file with `popen("sha1sum ...")`, draws progress through the loading screen `0x4fc460`. Skipped when `0x1766d98` != 0 (Project Raven sets it; setter `0x49e970` has no callers) |
| `0x4fc460` | loading/message screen | clears, draws a text line, swaps, pumps SDL events (`SDL_PollEvent(NULL)`) |
| `0x4ad370` | RIO I/O board init | "SysInfoRioFirmware", "(ERROR - missing %i board(s)!)" in `0x4adac0` |
| `0x5e5240` | crash signal handler | installed for signals 1-14 |

## Video

| Address | Name | Notes |
|---|---|---|
| `0x4fbb60` | `VidInit` | stores the view scale at `0x16f3630`/`0x16f3634`/`0x16f364c`, calls `WndOpen(w=*(u16*)0x1cc1fa8, h=*(u16*)0x1cc1faa, 0x1cc1fb4, 0x1cc1fb8, 0x1cc1fa4, title)`, then `0x4fb9b0`, `0x6041f0`, `0x49b050`, `0x4dfe80`, reads `glGetString(GL_VENDOR/GL_RENDERER)`. "WndOpen(%d x %d) failed! (ec=%d)" at `0x4fbeea` |
| `0x49ae40` | `WndOpen` | 9x `SDL_GL_SetAttribute`, `SDL_CreateWindow`, `SDL_GL_CreateContext`, `0x646570`, `SDL_GL_SetAttribute` |

## Statically linked SDL 2.0.2/2.0.3 (`0x640000`-`0x690000`)

Entry points called from game code (`tools/callgraph.py boundary 640000 700000`), identified from their error
strings:

| Address | Function |
|---|---|
| `0x644850` | `SDL_GL_SetAttribute` |
| `0x644e90` | `SDL_GL_CreateContext` |
| `0x645e40` | `SDL_CreateWindow` |
| `0x6463a0` | `SDL_GL_SetSwapInterval` |
| `0x646470` | `SDL_GL_SwapWindow` (probable: "The specified window has not been made current") |
| `0x646570` | a window query ("Invalid window"), called once from `WndOpen` |
| `0x642b70`, `0x6453f0`, `0x645760` | window functions, not yet identified |
| `0x65c860` | `SDL_PollEvent` / `SDL_PeepEvents` path ("Couldn't lock event queue") |
| `0x65d150` | `SDL_PushEvent` path ("Event queue is full") |
| `0x65f6f0` | game controller mapping |
| `0x664240` | audio open ("No available audio device") |
| `0x666640`, `0x666980` | `SDL_RWops` |
| `0x66c3b0`-`0x66f020` | renderer / textures |

## Arcade I/O and protection (`0x610000`-`0x640000`)

| Address | Notes |
|---|---|
| `0x621150` | `io_rio2_config_update` (RIO I/O board) |
| `0x61d640` | gun calibration ("Invalid number of calibration points") |
| `0x639740` | HASP key info XML |
| `0xb2d630`, `0xb2dd48`, `0xb2e1aa`, `0xb2e391`, `0xb2f5ac`, `0xb2fd63`, `0xb2fe60` | HASP API: login, logout, encrypt, free, get_sessioninfo, read, write (inline-hooked by `halo.so`'s `initialize`) |

## Startup flow and presentation

| Address | Name | Notes |
|---|---|---|
| `0x435560` | option parser | `-a<W>x<H>` aspect (`0x1749f8c`), `-f<W>x<H>` resolution (`0x1749f84/88`), `-k` print key and exit, `-dev` `0x1749fbe`, `-fps` `0x1749fb8`, `-random` `0x1749fbf`, `-ms` `0x1749fc1`, `-n` `0x1749fbc`, `-b` `0x1749fb9` |
| `0x417db0` | startup cabinet check | cabinet types 3/4/8 halt on "wrongcabinettype" when the window aspect (`0x1cc1fac`) is above 2.0, type 9 when below 3.0. Project Raven turns `cmp eax, 8` at `0x417de6` into `jmp 0x417e64` (ultrawide) |
| `0x417e30` | halt loop | polls I/O (`0x61cc70(5,1)`) and sleeps forever after a fatal startup screen |
| `0x448c60` | boot flow | shows the `online_popup` scene (Xbox Live "Get Connected") until 30 s pass (`cmp ebx, 0x752f` at `0x4493f8`) or a button is pressed; Project Raven sets the limit to 0 |
| `0x444a80` | factory setup done? | returns `0x174a220`; patched to `mov al, 1; ret` |
| `0x454e00` / `0x454e10` / `0x454e20` | QR scanners fitted (get / set) / QR active | the QR scanners are webcams (`WebcamIdProc`); flag `0x174c100` |
| `0x5a8960` | `ArchiveLoad(type, path, char** buf, u32* size, alloc, free, terminate)` | decrypts one file from `data/prod/archives/*.zip` (minizip: `0x1119260` file info, `0x111ace0` read). Scripts ("programs") and scenes load through it; Project Raven wraps it to edit scripts (src/game/script_patches.cpp) |
| `0x54cdc0` | frame timing | frame delta in ms at `0x1dc416c`, seconds `0x17f8470` + fraction `0x17f8464`. The game is delta-time driven, so any frame rate works |

Script commands are a table of 0x80-byte records (handler at name-0x14, id at name-4), e.g. `TRANS` (id 0x6d, `0x50ccf0`), `SCALE` (0x73, `0x520510`).

## Rendering

| Address | Name | Notes |
|---|---|---|
| `0x492670` | camera update | builds a camera's projection; ortho cameras: flags `+0x88` bit 1, half extents `+0xa0`/`+0xa4`, bit 4 = pixel-sized from the viewport |
| `0x492580` | `MatOrtho(out, l, r, b, t, n, f)` | |
| `0x5342d0` | `CameraSetOrtho(cam, l, r, b, t, n, f)` | the global 2D camera `0x1cbe8a0` is set by `0x4f6b00(x, y, w, h)` |
| | 2D elements | drawn with one matrix (ortho x element) through `glUniformMatrix4fv`: x scale = element scale / 960, x translation = element center in clip space. The 1920x1080 scene cameras are `gameflowCam_1920x1080_occ` (half extents 960 x 539.74) |

## Input (`0x618ea0` PC driver, I/O core at `0x180ae80`); see docs/input.md

| Address | Name | Notes |
|---|---|---|
| `0x618ea0` | pmrt_io update | polls SDL events: keys become physical scancode switches, mouse motion goes to analog channels at `[0x180adf0]+0x6000` (X) / `+0x6048` (Y), mouse buttons count presses, SDL game controller events (0x650-0x652) feed its own joystick support |
| `0x61cdc0` | `AnalogSet(channel, value)` | 0x48-byte channel: value `+4`, raw `+0xc`, flags `+0` (bit 0 = invert as 1 - v), offset `+0x3c`, scale `+0x44` |
| `0x4acdb0` | `IoRemapAdd(physical, logical)` | table count `0x1771850`, entries `0x1766f40 + 0x1122*8` |
| `0x41baf0` | remap builder | mouse mode (`0x1749fa9`, read by `0x438020`) maps physical `0x201/0x202` to every gun's X/Y (logical `0x206+12k/0x207+12k`) and second pair (`0x2a3+3k/0x2a4+3k`); triggers from mouse buttons `0x204-0x206` (gun 0) and keys Q/W/E, R/T/Y, U/I/O (guns 1-3) |
| `0x4ace10` | `IoRemapRemove(physical, logical)` | removes one entry (the last entry moves into its slot) |
| `0x4acfe0` | `IoRemapSource(logical)` | the physical mapped to a logical, or -1; game code skips a gun whose axes have none |
| `0x4ad6e8` | remap copy (in `0x4ad370`) | every frame, for each remap: copies the physical analog channel (`0x61ee70`) over the logical one (`0x4ac220`) |
| `0x4ae030` | `IoChannel(id)` | logical / physical analog channel pointer (jump table `0x1147cb0`, ids `0x200-0x2f2`); `0x4af0a0` reads its value |
| `0x61ee70` | `DriverAnalog(physical)` | PC driver analog channel: `0x201-0x203` mouse X/Y/wheel, `0x21d-0x220`, `0x239-0x298` joystick axes; 32-bit offset table at `0x11bcd70` (from itself) for `0x201-0x3c5`. Project Raven points `0x210/0x211` at player 2's channels |
| `0x4269e0` | player join loop | per slot 0-3: start switch `0x24e+slot`, `MARK_PLAYER_JOIN`; on this cabinet type slots 1 and 2 are skipped (2 players = slots 0 and 3) |
| `0x4178d0` | `GunEnabled(slot)` | adjustment `hardware/p<n>gun` (slot 3 reads `p2gun` on this cabinet type) |
| `0x494fa0` | `SceneStart(scene, flag)` | scene name at `+0`; `0x4957d0` ends one |
| `0x4ab530` | `SetTrigger(domain, name)` | looks the trigger up in the hash table `0x1c1fac0` (`0x922160(table, name, 0)`), sets it and counts sets at `+0xb8` |
| `0x438010` | `-random` flag | `0x1749fbf` |

## Operator adjustments

The cabinet's operator settings. Each one has a definition, `data/prod/adj/<core|game|online|redemption>/defs/<group>/<name>.txt`
(encrypted; region subfolders such as `jp/` override per region), and an optional lookup table for its value names
(`.../luts/<lut>.txt`). The saved value is `pmuser/adj/save/<group>/<name>.<0|1|2>.txt` (three copies). Each copy is
u32 20, the SHA-1 of the body, u32 body length, then the body: `# RTDT Data Type AdjValStore` text with `name`,
`version`, `time_set` and `val`. src/game/adjustments.cpp writes these into the save overlay before the game starts.

| Address | Name | Notes |
|---|---|---|
| `0x540550` | `Adj_Init` | creates the `gAdjDefs` / `gAdjVals` hash tables |
| `0x540080` | load `ADJ_DEF` files | for each def file: `0x4e6800("ADJ_DEF", path, "AdjDef", &def)`, then inserts the 256-byte `AdjDef` into `gAdjDefs` (`0x1cc3e00`) |
| `0x4e6800` | RTDT object load | `(kind, path, type name, out)`: reads a file (decrypting it) and parses it into a struct of the named data type |
| `0x910c00` | RTDT text parser | parses the data type headers (C structs), e.g. `AdjDef`, `AdjValStore`, `AudDef` |
| `0x4e65f0` | FileLoad | looks the path up in `eFileMaps` (`0x922160`); flag `0x200000` → ArchiveLoad `0x5a8960`, `0x10000` → `0x774b00`, 0 → plain `0x92fe60` |
| `0x53f270` | `Adj_GetValue(group, name)` | int |
| `0x53f860` | `Adj_SetValue(group, name, value)` | clamps to the definition's range and sets the running value (not saved to disk) |
| `0x53f7f0` | `(group, name, value)` setter (probably the default) | `0x434950` calls it and `Adj_SetValue` with 135 for `minGameTime` at boot, the second only when `0x53f660` returns nonzero |

`AdjDef`: `name[64]`, `version` (+64), flag bytes `default_on_version_change, online_reconcile, hidden, inactive, loop,
restart_required` (+68), then int32 `adminOnly` (+76), `default_value, min, max, step_value, divisor` (+80),
`displayAsTime, Date, Currency, FracCurrency, WithRealCurrType, Percentage` (+100), `displayPriority` (+124),
`lookup_table[64]` (+128), `sound_to_play[64]` (+192).

`RAVEN_DUMP_SCRIPTS=<folder>` also wraps `0x4e6800` and logs every `AdjDef` (`adjdef <path> | name=... def=... min=...
max=...` in raven.log), and saves the data type headers that `0x910c00` parses to `<folder>/rtdt/`.

### What they are

All 84 definitions in this build. Ones that matter for play:

| Adjustment | Default | Range | What it does |
|---|---|---|---|
| `gameOptions/minGameTime` | 150 s (the game sets 135 at boot) | 30-1200 s, shown as a time | **Difficulty.** Player damage scales with how long that player has been playing, relative to this time (below) |
| `gameOptions/violence` | 1 | 0-1, `violenceLUT` | gore level |
| `gameOptions/HUDviolence` | 1 | 0-1, `violenceLUT` (hidden) | gore in the HUD; the Korean version (`kr/`) has its own default |
| `gameOptions/subtitles` | 1 (on) | 0-1 | subtitles in mission movies: each movie's `*_movie_subtitles.txt` checks it once at the start (`BR_ON_ADJUSTMENT gameOptions,subtitles,SHOW,0` jumps when it's nonzero). Settings > Gameplay (off by default) edits that check out and makes each line in `movie_subtitles/` poll it every 6 frames, so it switches while a movie plays (src/game/script_patches.cpp) |
| `hardware/forcefeedback` | 1 (on) | 0-1 | gun recoil (read in many places, `0x42ae20`-`0x44d030`) |
| `hardware/coloredRing` | 1 (on) | 0-1 | the guns' colored light rings |
| `hardware/p1gun`-`p4gun` | 1 | 0-1, `diagEnableLUT` | gun enabled (`0x4178d0`) |
| `hardware/LampBrightness` | 50 % | 0-100 (hidden, admin only) | cabinet lamps |
| `sound/gameVolume` | 50 | 0-100 | game volume: read at startup (`0x437a20`) and passed to SetGameVolume `0x59ed20(float)`; Settings > Gameplay |
| `sound/attractVolume` | 25 | 0-100 | attract mode volume |
| `sound/attractSoundFreq` | 1 | 0-3, `attractSoundFreqs` | how often attract mode plays sound |
| `languages/language` | 0 | 0-10, `languages` | language (restart required) |
| `playerCost/freeplay` | 0 | 0-1 | free play (Project Raven sets it from Settings) |
| `playerCost/gameCost`, `continueCost` | 200, 100 | currency | cost to start / continue; unused in free play |
| `playerCost/bonusPrice*` | | | "N games for X" pricing |

The rest are business settings: `coin/*` (coin values per region), `altPay/*` (card readers), `collection/*` (fees,
splits), `tickets/*` (redemption tickets: `ticketMode` off by default), `online/*` and `onlineperms/*` (Raw Thrills
servers), `switch_telem/*` (button fault detection), `dateTime/*`. There is no adjustment for difficulty other than
`minGameTime`, for lives, or for player health.

### Damage over play time (`minGameTime`)

Player damage, `0x419760(slot, damage)`: players at `0x1c0c7c0`, stride `0x250`; health at `+0x14` (1000 full),
play time in ms at `+0x88`. `t` is play time / (`minGameTime` × 1000). The tuning values come from the
`player_health` variable table (`0x1c0d4a0`, read with GetFloat `0x9247a0(table, key, default)` on every hit). The game
fills it with values other than the code defaults (where from isn't known; not the scripts or `data/prod/conf`):

| Key | Code default | Value at run time | Meaning |
|---|---|---|---|
| `GAMETIME_DAMAGE_START` | 0.2 | 0.3 | damage multiplier at `t = 0` |
| `GAMETIME_DAMAGE_LOW` | 0.5 | 1.0 | at `t = GAMETIME_NORMAL_TIME` |
| `GAMETIME_DAMAGE_HIGH` | 2.0 | 2.5 | at `t = GAMETIME_HARD_TIME` |
| `GAMETIME_DAMAGE_KILL` | 3.0 | 5.0 | at `t = GAMETIME_KILL_TIME` and after |
| `GAMETIME_NORMAL_TIME` | 0.5 | 1.0 | |
| `GAMETIME_HARD_TIME` | 1.5 | 1.125 | |
| `GAMETIME_KILL_TIME` | 2.0 | 1.25 | |
| `GAMETIME_DAMAGE_MOD` | 0.5 | | damage past the health floor is multiplied by this |
| `INVULNERABLE_TIME` | 3 s | | no damage for this long after joining |
| `player_health` | 1000 | | |

The multiplier is linear between those points (`0x419a2a`, `0x419b2f`, `0x419ce8`). On top of it, health has a floor of
`(1 - t) × player_health` (`0x419a79`): damage that would go below it is cut to `GAMETIME_DAMAGE_MOD` of the excess
(`0x419d70`). So with the cabinet's 135 s you take 0.3× damage at first, rising to 1× at 135 s, then 2.5× at 152 s
and 5× from 169 s; until 135 s health only drains about as fast as the floor falls, so you can't die early. Then
damage climbs to end the credit. Measured (`RAVEN_DAMAGE_LOG`): 30-point hits took 9, then 1-2 per hit once health
reached the floor.

Settings > Gameplay > Difficulty (src/game/gameplay.cpp) wraps GetFloat: for Easy / Normal / Heroic / Legendary it
answers all four `GAMETIME_DAMAGE_*` curve keys with 0.5 / 1 / 2 / 3 (a flat curve) and `GAMETIME_DAMAGE_MOD` with 1
(no floor). Measured: 30-point hits take 30 on Normal and 90 on Legendary, and the game continues as usual at 0 health
("CONTINUE?" countdown). Arcade leaves them alone. `RAVEN_DAMAGE_LOG=1` logs every hit with health before and after.
