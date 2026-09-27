#include "../src/platform/vita_native.h"

#include <cstdio>

namespace flashvita::vita {

// The parser test exercises the production parser with host file I/O.
bool readFile(const std::string& path, std::vector<uint8_t>& out, size_t max_bytes) {
    out.clear();
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;

    bool ok = std::fseek(file, 0, SEEK_END) == 0;
    const long size = ok ? std::ftell(file) : -1;
    ok = ok && size >= 0 && static_cast<unsigned long>(size) <= max_bytes &&
         std::fseek(file, 0, SEEK_SET) == 0;
    if (ok) {
        out.resize(static_cast<size_t>(size));
        ok = out.empty() || std::fread(out.data(), 1, out.size(), file) == out.size();
    }
    std::fclose(file);
    if (!ok) out.clear();
    return ok;
}

} // namespace flashvita::vita
