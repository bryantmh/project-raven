// The game's named triggers (script SETTRIGGER / CLRTRIGGER / WAITFOR_TRIGGER). SetTrigger (0x4ab530) finds a
// trigger by name in the hash table at 0x1c1fac0 (lookup 0x922160) and counts every set at +0xb8, so a set that is
// cleared right away (a pulse) still shows up as a change of that count.
#include "game/triggers.h"

#include <cstdint>
#include <cstdlib>

#include "linux/shim.h"
#include "game/view_aspect.h"
#include "log.h"

namespace raven::game {
namespace {
using HashFind = SYSV void* (*)(void* table, const char* name, int flags);
constexpr uint64_t kHashFind = 0x922160, kTriggerTable = 0x1c1fac0;
}  // namespace

void set_trigger(const char* name) {
    using SetTrigger = SYSV int (*)(int domain, const char* name);
    reinterpret_cast<SetTrigger>(0x4ab530)(0, name);
}

int trigger_set_count(const char* name) {
    // The trigger table is built during boot, while the boot screen already presents frames: wait for the first scene.
    if (!scenes_started()) return -1;
    // Looked up every time: triggers belong to scenes, which come and go.
    auto t = static_cast<uint8_t*>(reinterpret_cast<HashFind>(kHashFind)(reinterpret_cast<void*>(kTriggerTable), name, 0));
    return t ? *reinterpret_cast<const int*>(t + 0xb8) : -1;
}
}  // namespace raven::game
