// Fixed game patches applied before the game starts. Each one is documented in docs/functions.md.
#include "game/patches.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "app/config.h"
#include "game/adjustments.h"
#include "game/boot_screen.h"
#include "game/script_patches.h"
#include "game/ui_aspect.h"
#include "game/view_aspect.h"
#include "log.h"

namespace raven::game {
void apply_patches() {
    // Skip the boot-time file test (0x49ea00): it SHA-1s every game file through `sha1sum` (~10 GB) and waits on
    // the cabinet's START/DIAG buttons. 0x1766d98 is its skip flag (setter 0x49e970, unused in this build).
    *reinterpret_cast<uint8_t*>(0x1766d98) = 1;
    // PC mouse mode (0x1749fa9, read by 0x438020; this build only ever clears it). With it on, the I/O remap
    // builder (0x41baf0) binds mouse X/Y (physical 0x201/0x202) to every gun's aim axes (logical 0x206+12k /
    // 0x207+12k) and mouse buttons / keys to the triggers, which is what the controller layer drives.
    // main (0x437aa3) and the option parser reset (0x4354d8) clear it with `mov byte [rip+x], 0`: make both store 1.
    for (uint64_t at : {0x437aa3ull, 0x4354d8ull}) {
        auto p = reinterpret_cast<uint8_t*>(at);
        if (p[0] == 0xC6 && p[1] == 0x05 && p[6] == 0) p[6] = 1;
        else RLOG("patch: unexpected bytes at %#llx", (unsigned long long)at);
    }
    *reinterpret_cast<uint8_t*>(0x1749fa9) = 1;
    // Factory setup (entering the cabinet serial number into the dongle) can't happen off-cabinet: report it as
    // done. 0x444a80 returns the flag at 0x174a220; make it `mov al, 1; ret`.
    {
        auto p = reinterpret_cast<uint8_t*>(0x444a80);
        p[0] = 0xB0; p[1] = 0x01; p[2] = 0xC3;
    }
    // Ultrawide: game startup (0x417db0) halts on a "wrong cabinet type" screen when the window's aspect ratio
    // (float at 0x1cc1fac) is above 2.0 on cabinet types 3/4/8 (or below 3.0 on dual-screen type 9). Skip the check:
    // `cmp eax, 8` at 0x417de6 becomes `jmp 0x417e64` (the code after it).
    {
        auto p = reinterpret_cast<uint8_t*>(0x417de6);
        if (p[0] == 0x83 && p[1] == 0xF8 && p[2] == 0x08) {
            const uint8_t jmp[5] = {0xE9, 0x79, 0x00, 0x00, 0x00};
            std::memcpy(p, jmp, 5);
        } else RLOG("patch: unexpected bytes at 0x417de6");
    }
    // Free play (Settings > General), as the operator adjustment playerCost/freeplay.
    set_adjustment("playerCost", "freeplay", app::config().free_play ? 1 : 0);
    // Both guns enabled (operator adjustments hardware/p1gun, p2gun; 0x4178d0 reads them). Off, a player can't
    // join: the game shows GUN<n>_DISABLED instead.
    set_adjustment("hardware", "p1gun", 1);
    set_adjustment("hardware", "p2gun", 1);
    // Xbox Live "Get Connected" boot screen (Settings > Presentation): the boot flow (0x448c60) shows the
    // online_popup scene until 30 s pass (`cmp ebx, 0x752f` at 0x4493f8) or a cabinet button is pressed. A limit
    // of 0 ends it after one frame.
    if (app::config().skip_xbox_screen) {
        auto p = reinterpret_cast<uint8_t*>(0x4493f8);
        if (p[0] == 0x81 && p[1] == 0xFB && p[2] == 0x2F && p[3] == 0x75) p[2] = p[3] = 0;
        else RLOG("patch: unexpected bytes at 0x4493f8");
    }
    install_script_patches();
    install_boot_screen();
    install_ui_aspect();
    install_view_aspect();
    RLOG("patches applied");
}
}  // namespace raven::game
