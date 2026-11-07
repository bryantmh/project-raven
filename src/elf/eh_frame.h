#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "elf/elf_loader.h"

namespace raven::elf {
// [start, end) of every function with an FDE in a mapped image, sorted.
std::vector<std::pair<uint64_t, uint64_t>> fde_ranges(const Image& im);
}  // namespace raven::elf
