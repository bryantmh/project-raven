#pragma once
#include <windows.h>

#include "elf/elf_loader.h"

namespace raven::game {
// Fill the GOT slots listed in the dump's embedded import table. Returns the number of entries.
int bind_embedded_imports(elf::Image& game);
// Rewrite fs:[0x28] stack-protector reads to gs:[TEB TlsSlots[slot]]. Returns the number of sites.
int patch_stack_protector(elf::Image& img, DWORD tls_slot);
}  // namespace raven::game
