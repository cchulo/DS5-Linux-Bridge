//
// Multi-slot constants shared across modules (BT, state manager, USB bridge).
//
// MULTI_SLOT_COUNT comes from CMake. A slot is a *connection* seat assigned at
// connect time (session order: lowest free slot wins), not a bond: bonds are
// BD_ADDR + link-key credentials and carry no slot number.
//

#ifndef DS5_BRIDGE_SLOTS_H
#define DS5_BRIDGE_SLOTS_H

#ifndef MULTI_SLOT_COUNT
#define MULTI_SLOT_COUNT 1
#endif

#define BT_MAX_SLOTS MULTI_SLOT_COUNT

// The slot currently bridged to the single USB HID gamepad interface and the
// audio pipeline. Slot fan-out to per-slot USB interfaces replaces the HID
// half of this; the audio half becomes the tier manager's designated
// primary-audio slot.
#define BT_USB_SLOT 0

#ifdef __cplusplus
// Reset one slot's USB-facing input buffer to the neutral idle report and
// mark it dirty (defined in main.cpp). Called on BT disconnect so a pad that
// drops mid-press doesn't leave its buttons frozen "held" on the host.
void bridge_reset_slot_input(uint8_t slot);
#endif

#endif // DS5_BRIDGE_SLOTS_H
