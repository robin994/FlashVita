#include "../src/player/swf_parser.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

namespace {

void pushLe16(std::vector<unsigned char>& out, unsigned value) {
    out.push_back(static_cast<unsigned char>(value & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
}

void pushLe32(std::vector<unsigned char>& out, unsigned value) {
    out.push_back(static_cast<unsigned char>(value & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
}

class BitWriter {
public:
    void write(unsigned value, unsigned bits) {
        for (int i = static_cast<int>(bits) - 1; i >= 0; --i) {
            if ((bit_count_ & 7u) == 0) data_.push_back(0);
            if ((value >> i) & 1u)
                data_.back() |= static_cast<unsigned char>(1u << (7u - (bit_count_ & 7u)));
            ++bit_count_;
        }
    }

    const std::vector<unsigned char>& data() const { return data_; }

private:
    std::vector<unsigned char> data_;
    unsigned bit_count_ = 0;
};

std::vector<unsigned char> makeBody() {
    std::vector<unsigned char> body;
    BitWriter rect;
    rect.write(14, 5);      // Nbits
    rect.write(0, 14);      // xmin
    rect.write(6400, 14);   // xmax = 320 px in twips
    rect.write(0, 14);      // ymin
    rect.write(4800, 14);   // ymax = 240 px in twips
    body.insert(body.end(), rect.data().begin(), rect.data().end());

    pushLe16(body, 24u << 8); // 24 FPS FIXED8
    pushLe16(body, 1);        // one frame

    pushLe16(body, (12u << 6) | 2u); // DoAction, length 2
    body.push_back(0x04);             // ActionNextFrame
    body.push_back(0x00);             // action terminator
    pushLe16(body, (1u << 6));        // ShowFrame
    pushLe16(body, 0);                // End
    return body;
}

std::vector<unsigned char> makeFws() {
    const auto body = makeBody();
    std::vector<unsigned char> swf = {'F', 'W', 'S', 8};
    pushLe32(swf, static_cast<unsigned>(8 + body.size()));
    swf.insert(swf.end(), body.begin(), body.end());
    return swf;
}

std::vector<unsigned char> makeCws() {
    const auto body = makeBody();
    uLongf compressed_size = compressBound(static_cast<uLong>(body.size()));
    std::vector<unsigned char> compressed(compressed_size);
    const int result = compress2(compressed.data(), &compressed_size,
                                 body.data(), static_cast<uLong>(body.size()), Z_BEST_SPEED);
    assert(result == Z_OK);
    compressed.resize(compressed_size);

    std::vector<unsigned char> swf = {'C', 'W', 'S', 8};
    pushLe32(swf, static_cast<unsigned>(8 + body.size()));
    swf.insert(swf.end(), compressed.begin(), compressed.end());
    return swf;
}

void writeFile(const std::string& path, const std::vector<unsigned char>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    assert(f);
    assert(std::fwrite(data.data(), 1, data.size(), f) == data.size());
    std::fclose(f);
}

void verify(const std::string& path, bool compressed) {
    flashvita::SwfParser parser;
    flashvita::SwfDocumentInfo info;
    assert(parser.parseFile(path, info));
    assert(info.valid);
    assert(info.compressed == compressed);
    assert(info.version == 8);
    assert(info.width == 320);
    assert(info.height == 240);
    assert(info.frame_rate == 24.0f);
    assert(info.frame_count == 1);
    assert(info.do_action_tags == 1);
    assert(info.avm1_action_records == 1);
    assert(info.show_frame_tags == 1);
    assert(info.has_avm1);
    assert(!info.has_avm2);
}

} // namespace

int main() {
    const std::string fws = "/tmp/flashvita_test_fws.swf";
    const std::string cws = "/tmp/flashvita_test_cws.swf";
    writeFile(fws, makeFws());
    writeFile(cws, makeCws());
    verify(fws, false);
    verify(cws, true);
    std::remove(fws.c_str());
    std::remove(cws.c_str());
    std::puts("FlashVita SWF parser tests: PASS");
    return 0;
}
