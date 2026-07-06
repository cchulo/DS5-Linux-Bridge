//
// Bandwidth-tier policy. See tier.h for the tier table and rationale.
//

#include "tier.h"

#include "bt.h"

uint8_t tier_audio_slot() {
#if BT_MAX_SLOTS == 1
    return 0;
#else
    const int s = bt_lowest_connected_slot();
    return s < 0 ? 0 : (uint8_t) s;
#endif
}

bool tier_audio_allowed() {
#if BT_MAX_SLOTS == 1
    return true;
#else
    return bt_connected_count() <= 1;
#endif
}
