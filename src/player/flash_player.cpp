#include "flash_player.h"

namespace flashvita {

bool FlashPlayer::open(const std::string& path) {
    path_ = path;
    open_ = true;
    ruffle_info_ = RuffleProbeInfo{};
    if (parser_.parseFile(path, document_)) {
        if (RuffleRuntime::compiledIn()) {
            if (ruffle_.startHeadless(path, ruffle_info_))
                status_ = "Ruffle headless runtime started. Rendering backend is the next integration phase.";
            else
                status_ = ruffle_info_.message;
        } else if (document_.has_avm2) {
            status_ = "SWF parsed. AVM2 detected; Ruffle bridge is not linked in this build.";
        } else if (document_.has_avm1) {
            status_ = "SWF parsed. AVM1 detected; Ruffle bridge is not linked in this build.";
        } else {
            status_ = "SWF parsed. Ruffle bridge is not linked in this build.";
        }
        return true;
    }

    status_ = document_.error;
    return false;
}

void FlashPlayer::close() {
    ruffle_.stop();
    open_ = false;
    path_.clear();
    status_.clear();
    document_ = SwfDocumentInfo{};
    ruffle_info_ = RuffleProbeInfo{};
}

} // namespace flashvita
