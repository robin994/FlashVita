#include "swf_library.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace flashvita {
namespace {

bool hasSwfExtension(const char* name) {
    const char* dot = std::strrchr(name, '.');
    if (!dot || std::strlen(dot) != 4) return false;
    return std::tolower(static_cast<unsigned char>(dot[1])) == 's' &&
           std::tolower(static_cast<unsigned char>(dot[2])) == 'w' &&
           std::tolower(static_cast<unsigned char>(dot[3])) == 'f';
}

uint32_t readLe32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

bool SwfLibrary::inspectHeader(SwfInfo& game) {
    FILE* f = std::fopen(game.path.c_str(), "rb");
    if (!f) return false;

    unsigned char header[8] = {};
    const size_t read = std::fread(header, 1, sizeof(header), f);
    std::fclose(f);
    if (read != sizeof(header)) return false;

    const bool fws = std::memcmp(header, "FWS", 3) == 0;
    const bool cws = std::memcmp(header, "CWS", 3) == 0;
    const bool zws = std::memcmp(header, "ZWS", 3) == 0;
    if (!fws && !cws && !zws) return false;

    game.compressed = cws || zws;
    game.lzma = zws;
    game.version = header[3];
    game.declared_size = readLe32(header + 4);
    game.header_valid = true;
    return true;
}

void SwfLibrary::scan(const std::string& root) {
    root_ = root;
    games_.clear();

    DIR* dir = opendir(root.c_str());
    if (!dir) return;

    for (dirent* entry = readdir(dir); entry; entry = readdir(dir)) {
        if (entry->d_name[0] == '.' || !hasSwfExtension(entry->d_name)) continue;

        SwfInfo game;
        game.name = entry->d_name;
        game.path = root;
        if (!game.path.empty() && game.path.back() != '/') game.path += '/';
        game.path += entry->d_name;

        struct stat st {};
        if (stat(game.path.c_str(), &st) == 0) game.size = static_cast<uint64_t>(st.st_size);
        inspectHeader(game);
        games_.push_back(game);
    }
    closedir(dir);

    std::sort(games_.begin(), games_.end(), [](const SwfInfo& a, const SwfInfo& b) {
        std::string left = a.name;
        std::string right = b.name;
        std::transform(left.begin(), left.end(), left.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(right.begin(), right.end(), right.begin(), [](unsigned char c) { return std::tolower(c); });
        return left < right;
    });
}

} // namespace flashvita
