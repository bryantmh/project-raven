#include "linux/shim.h"

#include <string>
#include <unordered_map>

namespace raven::shim {
static std::unordered_map<std::string, void*>& table() {
    static std::unordered_map<std::string, void*> t;
    return t;
}
void register_symbol(const char* name, void* addr) { table()[name] = addr; }
void* lookup(const char* name) {
    auto it = table().find(name);
    return it == table().end() ? nullptr : it->second;
}
}  // namespace raven::shim
