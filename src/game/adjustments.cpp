// Operator adjustments (pmuser/adj/save/<group>/<name>.<0|1|2>.txt), written into the save overlay before the game
// starts. Each file is: u32 20, SHA-1 of the body, u32 body length, body. The body is the text below, NUL included.
// docs/functions.md#operator-adjustments
#include "game/adjustments.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app/paths.h"
#include "log.h"

namespace raven::game {
namespace {
std::vector<uint8_t> sha1(const std::string& data) {
    std::vector<uint8_t> out(20);
    BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), ULONG(data.size()), out.data(), 20);
    return out;
}
}  // namespace

void set_adjustment(const std::string& group, const std::string& name, int value) {
    namespace fs = std::filesystem;
    std::string body = "# RTDT Data Type AdjValStore\r\nname = " + name + "\r\nversion = 0\r\ntime_set = 1\r\nval = " +
                       std::to_string(value) + "\r\n";
    body.push_back('\0');
    auto h = sha1(body);
    fs::path dir = fs::path(app::to_wide(app::paths().save_dir)) / L"overlay" / L"pmuser" / L"adj" / L"save" / app::to_wide(group);
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (int copy = 0; copy < 3; copy++) {
        std::ofstream f(dir / app::to_wide(name + "." + std::to_string(copy) + ".txt"), std::ios::binary);
        uint32_t hl = 20, bl = uint32_t(body.size());
        f.write(reinterpret_cast<const char*>(&hl), 4);
        f.write(reinterpret_cast<const char*>(h.data()), 20);
        f.write(reinterpret_cast<const char*>(&bl), 4);
        f.write(body.data(), body.size());
    }
    RLOG("adjustment %s/%s = %d", group.c_str(), name.c_str(), value);
}
}  // namespace raven::game
