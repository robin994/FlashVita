#include "flash_player.h"

namespace flashvita {

bool FlashPlayer::open(const std::string& path) {
    // Release the previous movie before doing any work for the next one. Large
    // CWS files can otherwise hit the Vita heap while the old Ruffle movie and
    // all of its decoded bitmaps are still resident.
    ruffle_.stop();
    path_ = path;
    open_ = true;
    document_ = SwfDocumentInfo{};
    ruffle_info_ = RuffleProbeInfo{};

    if (RuffleRuntime::compiledIn()) {
        if (ruffle_.startHeadless(path, ruffle_info_)) {
            // Ruffle already parses/decompresses the SWF while constructing the
            // movie. Re-running the legacy parser here used another full
            // decompression buffer (24.7 MB for super-smash-flash.swf).
            document_.valid = ruffle_info_.parsed;
            document_.compressed = ruffle_info_.compression != 0;
            document_.lzma = ruffle_info_.compression == 2;
            document_.version = ruffle_info_.version;
            document_.width = ruffle_info_.stage_width;
            document_.height = ruffle_info_.stage_height;
            document_.frame_rate = ruffle_info_.frame_rate;
            document_.frame_count = ruffle_info_.frame_count;
            document_.tag_count = ruffle_info_.tag_count;
            document_.has_avm1 = ruffle_info_.has_avm1;
            document_.has_avm2 = ruffle_info_.has_avm2;
            status_ = "Ruffle runtime started";
            return true;
        }

        document_.error = ruffle_info_.message;
        status_ = ruffle_info_.message;
        return false;
    }

    if (parser_.parseFile(path, document_)) {
        if (document_.has_avm2) {
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
