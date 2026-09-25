#pragma once

#include <cstdint>
#include <string>

namespace flashvita {

struct SwfDocumentInfo {
    bool valid = false;
    bool compressed = false;
    bool lzma = false;
    int version = 0;
    uint32_t declared_size = 0;
    int width = 0;
    int height = 0;
    float frame_rate = 0.0f;
    uint16_t frame_count = 0;
    uint32_t tag_count = 0;
    uint32_t show_frame_tags = 0;
    uint32_t do_action_tags = 0;
    uint32_t do_init_action_tags = 0;
    uint32_t avm1_action_records = 0;
    uint32_t do_abc_tags = 0;
    bool has_avm1 = false;
    bool has_avm2 = false;
    std::string error;
};

class SwfParser {
public:
    bool parseFile(const std::string& path, SwfDocumentInfo& out) const;
};

} // namespace flashvita
