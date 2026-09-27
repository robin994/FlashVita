#pragma once

#if FLASHVITA_ENABLE_RUFFLE
extern "C" {
void flashvita_vitagl_prepare_ui();
void flashvita_vitagl_invalidate_cache();
}
#endif
