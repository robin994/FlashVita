#include "swf_parser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <zlib.h>

namespace flashvita {
namespace {

constexpr uint32_t kMaxSwfSize = 128u * 1024u * 1024u;

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool readUnsigned(unsigned bits, uint32_t& value) {
        if (bits > 32 || bit_pos_ + bits > size_ * 8) return false;
        value = 0;
        for (unsigned i = 0; i < bits; ++i) {
            const size_t absolute = bit_pos_++;
            const uint8_t byte = data_[absolute / 8];
            const unsigned shift = 7u - static_cast<unsigned>(absolute % 8);
            value = (value << 1) | ((byte >> shift) & 1u);
        }
        return true;
    }

    bool readSigned(unsigned bits, int32_t& value) {
        uint32_t raw = 0;
        if (!readUnsigned(bits, raw)) return false;
        if (bits == 0) {
            value = 0;
            return true;
        }
        if (bits < 32 && (raw & (1u << (bits - 1)))) raw |= (~0u << bits);
        value = static_cast<int32_t>(raw);
        return true;
    }

    size_t bytesConsumed() const { return (bit_pos_ + 7) / 8; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t bit_pos_ = 0;
};

uint32_t countAvm1Actions(const uint8_t* data, size_t size) {
    size_t offset = 0;
    uint32_t count = 0;
    while (offset < size) {
        const uint8_t opcode = data[offset++];
        if (opcode == 0) break;
        ++count;
        if (opcode >= 0x80) {
            if (offset + 2 > size) break;
            const uint16_t length = readLe16(data + offset);
            offset += 2;
            if (offset + length > size) break;
            offset += length;
        }
    }
    return count;
}

bool readFile(const std::string& path, std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long length = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (length < 0 || static_cast<uint64_t>(length) > kMaxSwfSize) {
        std::fclose(f);
        return false;
    }
    data.resize(static_cast<size_t>(length));
    const size_t read = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    return read == data.size();
}

} // namespace

bool SwfParser::parseFile(const std::string& path, SwfDocumentInfo& out) const {
    out = SwfDocumentInfo{};

    std::vector<uint8_t> file;
    if (!readFile(path, file)) {
        out.error = "Unable to read SWF file";
        return false;
    }
    if (file.size() < 8) {
        out.error = "SWF is smaller than the 8-byte header";
        return false;
    }

    const bool fws = std::memcmp(file.data(), "FWS", 3) == 0;
    const bool cws = std::memcmp(file.data(), "CWS", 3) == 0;
    const bool zws = std::memcmp(file.data(), "ZWS", 3) == 0;
    if (!fws && !cws && !zws) {
        out.error = "Unsupported SWF signature";
        return false;
    }

    out.compressed = cws || zws;
    out.lzma = zws;
    out.version = file[3];
    out.declared_size = readLe32(file.data() + 4);
    if (out.declared_size < 8 || out.declared_size > kMaxSwfSize) {
        out.error = "Invalid SWF declared size";
        return false;
    }

    if (zws) {
        out.error = "ZWS/LZMA parsing is not implemented yet";
        return false;
    }

    std::vector<uint8_t> decoded;
    const uint8_t* swf = file.data();
    size_t swf_size = file.size();

    if (cws) {
        decoded.resize(out.declared_size);
        std::memcpy(decoded.data(), file.data(), 8);
        decoded[0] = 'F';
        uLongf output_length = static_cast<uLongf>(out.declared_size - 8);
        const int zret = uncompress(decoded.data() + 8, &output_length,
                                    file.data() + 8, static_cast<uLong>(file.size() - 8));
        if (zret != Z_OK) {
            out.error = "zlib failed to decompress CWS body";
            return false;
        }
        decoded.resize(8 + static_cast<size_t>(output_length));
        swf = decoded.data();
        swf_size = decoded.size();
    } else if (file.size() < out.declared_size) {
        out.error = "Truncated FWS body";
        return false;
    }

    BitReader bits(swf + 8, swf_size - 8);
    uint32_t nbits = 0;
    int32_t xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    if (!bits.readUnsigned(5, nbits) || nbits == 0 || nbits > 31 ||
        !bits.readSigned(nbits, xmin) || !bits.readSigned(nbits, xmax) ||
        !bits.readSigned(nbits, ymin) || !bits.readSigned(nbits, ymax)) {
        out.error = "Malformed SWF stage RECT";
        return false;
    }

    size_t offset = 8 + bits.bytesConsumed();
    if (offset + 4 > swf_size) {
        out.error = "Missing SWF frame metadata";
        return false;
    }

    out.width = std::max(0, (xmax - xmin) / 20);
    out.height = std::max(0, (ymax - ymin) / 20);
    out.frame_rate = static_cast<float>(readLe16(swf + offset)) / 256.0f;
    offset += 2;
    out.frame_count = readLe16(swf + offset);
    offset += 2;

    while (offset + 2 <= swf_size) {
        const uint16_t record = readLe16(swf + offset);
        offset += 2;
        const uint16_t code = record >> 6;
        uint32_t length = record & 0x3f;
        if (length == 0x3f) {
            if (offset + 4 > swf_size) {
                out.error = "Truncated long SWF tag header";
                return false;
            }
            length = readLe32(swf + offset);
            offset += 4;
        }
        if (offset + length > swf_size) {
            out.error = "SWF tag extends beyond file body";
            return false;
        }

        const uint8_t* payload = swf + offset;
        ++out.tag_count;
        switch (code) {
            case 0: // End
                offset += length;
                out.valid = true;
                return true;
            case 1: // ShowFrame
                ++out.show_frame_tags;
                break;
            case 12: // DoAction
                ++out.do_action_tags;
                out.has_avm1 = true;
                out.avm1_action_records += countAvm1Actions(payload, length);
                break;
            case 59: // DoInitAction: UI16 sprite id then action records
                ++out.do_init_action_tags;
                out.has_avm1 = true;
                if (length > 2) out.avm1_action_records += countAvm1Actions(payload + 2, length - 2);
                break;
            case 69: // FileAttributes: ActionScript3 flag is bit 3.
                if (length >= 1 && (payload[0] & 0x08)) out.has_avm2 = true;
                break;
            case 82: // DoABC
                ++out.do_abc_tags;
                out.has_avm2 = true;
                break;
            default:
                break;
        }
        offset += length;
    }

    out.valid = true;
    return true;
}

} // namespace flashvita
