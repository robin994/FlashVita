#pragma once

#include "swf_parser.h"
#include "ruffle_runtime.h"
#include "../input/input_mapper.h"

#include <string>

namespace flashvita {

class FlashPlayer {
public:
    bool open(const std::string& path);
    void close();
    bool isOpen() const { return open_; }
    const std::string& path() const { return path_; }
    const std::string& status() const { return status_; }
    const SwfDocumentInfo& document() const { return document_; }
    const RuffleProbeInfo& ruffleInfo() const { return ruffle_info_; }
    bool ruffleRunning() const { return ruffle_.running(); }
    bool tick(double dt_ms) { return ruffle_.tick(dt_ms); }
    bool renderedLastTick() const { return ruffle_.renderedLastTick(); }
    bool sendKey(FlashKey key, bool down) { return ruffle_.keyEvent(static_cast<int>(key), down); }
    bool sendMouseMove(double x, double y) { return ruffle_.mouseMove(x, y); }
    bool sendMouseButton(double x, double y, bool down) { return ruffle_.mouseButton(x, y, down); }

private:
    bool open_ = false;
    std::string path_;
    std::string status_;
    SwfDocumentInfo document_;
    SwfParser parser_;
    RuffleRuntime ruffle_;
    RuffleProbeInfo ruffle_info_;
};

} // namespace flashvita
