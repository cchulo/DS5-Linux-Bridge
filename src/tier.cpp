//
// Bandwidth-tier policy. See tier.h for the tier table and rationale.
//

#include "tier.h"

#include "bt.h"
#include "config.h"

uint8_t tier_audio_slot() {
#if BT_MAX_SLOTS == 1
    return 0;
#else
    const uint8_t s = get_config().audio_slot;
    return s < BT_MAX_SLOTS ? s : 0;
#endif
}

bool tier_audio_allowed() {
#if BT_MAX_SLOTS <= 2
    return true;
#else
    return bt_connected_count() <= 2;
#endif
}
