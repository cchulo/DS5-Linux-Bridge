//
// Created by awalol on 2026/5/15.
//

#ifndef DS5_BRIDGE_STATE_MGR_H
#define DS5_BRIDGE_STATE_MGR_H

#include <cstdint>

#include "slots.h"

void state_init();
// Reset one slot's cached output state back to its connect-time defaults
// (slot color / player indicators on multi-slot builds). Called when a slot
// frees up so the next controller seated there doesn't inherit the previous
// pad's rumble/trigger/lightbar state.
void state_slot_reset(uint8_t slot);
// Copy the cached 63-byte state for `slot` into `data` (which must be at
// least `size` bytes). Despite the historical name, this is a *getter* — host
// inputs go through state_update(), not here.
void state_get(uint8_t slot, uint8_t *data, uint8_t size);
void state_update(uint8_t slot, const uint8_t *data, uint8_t size);

// Re-write `slot`'s cached lightbar bytes from the configured slot color
// (config slot_rgb). Multi-slot builds only; no-op otherwise. Called from
// state_slot_reset and when the user edits slot colors in the web UI.
void state_apply_slot_color(uint8_t slot);

// Shared global state variables for hybrid muting. Mute is an audio-path
// concern and audio serves the BT_USB_SLOT controller only, so these stay
// singletons scoped to that slot.
extern volatile bool g_firmware_mic_muted;
extern volatile bool g_host_hid_manages_mute;
extern volatile uint8_t g_last_uac_mute;

// Mute control helper functions (operate on slot BT_USB_SLOT's state).
void state_set_local_mute(bool muted);
void state_reset_mute();
void state_push_to_bt();

#endif //DS5_BRIDGE_STATE_MGR_H
