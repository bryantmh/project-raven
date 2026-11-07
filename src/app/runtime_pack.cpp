// The runtime files embedded in raven.exe (tools/pack_runtime.py, resource 300): the Linux C++ runtime, OpenBLAS,
// FMOD and the boot image. Unpacked once per build into %LOCALAPPDATA%\ProjectRaven\runtime\<hash>, so raven.exe
// is the whole download.
#include "app/runtime_pack.h"

#include <windows.h>
#include <compressapi.h>
#include <shlobj.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "app/paths.h"
#include "log.h"

namespace raven::app {
namespace fs = std::filesystem;
namespace {
constexpr int kResource = 300;

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

bool unpack(const uint8_t* packed, size_t size, const fs::path& dir) {
    DECOMPRESSOR_HANDLE d = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &d)) return false;
    SIZE_T need = 0;
    Decompress(d, packed, size, nullptr, 0, &need);
    std::vector<uint8_t> blob(need);
    bool ok = need && Decompress(d, packed, size, blob.data(), blob.size(), &need);
    CloseDecompressor(d);
    if (!ok || blob.size() < 10 || std::memcmp(blob.data(), "RVPAK1", 6) != 0) return false;
    uint32_t count = 0;
    std::memcpy(&count, &blob[6], 4);
    size_t at = 10;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t name_len = 0;
        uint64_t len = 0;
        if (at + 2 > blob.size()) return false;
        std::memcpy(&name_len, &blob[at], 2);
        at += 2;
        if (at + name_len + 8 > blob.size()) return false;
        std::string name(reinterpret_cast<const char*>(&blob[at]), name_len);
        at += name_len;
        std::memcpy(&len, &blob[at], 8);
        at += 8;
        if (at + len > blob.size() || name.find("..") != std::string::npos) return false;
        fs::path out = dir / fs::path(to_wide(name));
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(&blob[at]), std::streamsize(len));
        if (!f) return false;
        at += len;
    }
    return true;
}
}  // namespace

bool unpack_runtime() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kResource), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    auto packed = static_cast<const uint8_t*>(mem ? LockResource(mem) : nullptr);
    DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!packed || !size) {
        RLOG("runtime: no embedded runtime files");
        return false;
    }
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return false;
    fs::path root = fs::path(local) / L"ProjectRaven" / L"runtime";
    CoTaskMemFree(local);
    wchar_t hash[17];
    swprintf(hash, 17, L"%016llx", (unsigned long long)fnv1a(packed, size));
    fs::path dir = root / hash;
    set_runtime_dir(to_utf8(dir.wstring()));
    std::error_code ec;
    if (fs::exists(dir / L".complete", ec)) return true;
    // Unpack into a temporary folder and rename it into place, so an interrupted unpack is never used.
    fs::path tmp = root / (std::wstring(hash) + L".tmp");
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (!unpack(packed, size, tmp)) {
        RLOG("runtime: unpacking failed");
        return false;
    }
    std::ofstream(tmp / L".complete") << "ok\n";
    fs::remove_all(dir, ec);
    fs::rename(tmp, dir, ec);
    if (ec) {
        RLOG("runtime: couldn't move the runtime into place: %s", ec.message().c_str());
        return false;
    }
    // Earlier builds' runtimes are no longer needed.
    for (auto& e : fs::directory_iterator(root, ec))
        if (e.path().filename() != hash) fs::remove_all(e.path(), ec);
    RLOG("runtime: unpacked %u KB into %s", unsigned(size / 1024), to_utf8(dir.wstring()).c_str());
    return true;
}
}  // namespace raven::app
