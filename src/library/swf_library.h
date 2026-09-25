#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace flashvita {

struct SwfInfo {
    std::string name;
    std::string path;
    uint64_t size = 0;
    bool compressed = false;
    bool lzma = false;
    int version = 0;
    uint32_t declared_size = 0;
    bool header_valid = false;
};

class SwfLibrary {
public:
    void scan(const std::string& root);
    const std::vector<SwfInfo>& games() const { return games_; }
    const std::string& root() const { return root_; }

private:
    static bool inspectHeader(SwfInfo& game);
    std::string root_;
    std::vector<SwfInfo> games_;
};

} // namespace flashvita
