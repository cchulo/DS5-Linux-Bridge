//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

extern uint8_t mute[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)
extern float volume[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)

#ifdef ENABLE_WAKE_HID
// Dynamic config-descriptor variant. Switched when the DualSense connects
// or disconnects: minimal (kbd only — wake-from-S3 still works, no audio
// or gamepad ghost in OS) vs full (audio + gamepad + kbd, current
// behavior). Variant swap is a tud_disconnect()/tud_connect() bounce
// orchestrated by usb_apply_variant_swap().
void usb_set_descriptor_variant_full(void);
void usb_set_descriptor_variant_minimal(void);
bool usb_descriptor_variant_is_full(void);

// TinyUSB HID instance index of the boot keyboard. STABLE at 1 in both
// variants: in full the gamepad is instance 0 (parsed first); in minimal a
// dummy placeholder HID holds instance 0 so the kbd stays instance 1. Kept as a
// function so callers stay decoupled from the constant.
uint8_t usb_kbd_hid_instance(void);

// Request a variant swap: orchestrator notes the desired variant, then
// usb_variant_task() drives a tud_disconnect()/settle/swap/tud_connect()
// bounce on the main loop. Safe to call from any context. No-op if the
// desired variant is already active. The task internally refuses to act
// while the host is suspended — preserves wake-from-S3/S5 by avoiding
// USB re-enumeration mid-suspend.
void usb_request_variant_full(void);
void usb_request_variant_minimal(void);

// Suspend-state plumbing. wake.cpp owns the authoritative suspended
// state; usb_variant_task queries this before starting/continuing a
// swap so we don't yank the bus during S3/S5.
void usb_set_host_suspended(bool suspended);
#endif

#include <stdint.h>
#include "slots.h"

// --- Dynamic exposed-slot enumeration (all builds) ---
// The gamepad interfaces are enumerated lazily: the dongle presents ONE
// gamepad interface (slot 0) until a controller connects into a seat that
// isn't exposed yet, then re-enumerates with the descriptor grown to the new
// concurrent-controller high-water mark. Individual disconnects never shrink
// the exposed set (a vacated seat stays enumerated and the next controller
// takes it with no bus disruption); only the LAST controller leaving resets
// the exposure back to a single slot.

// A controller connected into `slot`; grow the exposure to cover it.
// No-op (no bus bounce) when the slot is already exposed.
void usb_notify_slot_connected(uint8_t slot);
// The last controller disconnected; reset exposure to slot 0 only.
void usb_notify_all_disconnected(void);
// Slots in the currently live (active) config descriptor.
uint8_t usb_exposed_slot_count(void);

// One-shot bus bounce keeping the current descriptor shape (see
// usb_descriptors.cpp; used once ever, after the first feature-snapshot
// capture).
void usb_request_rebind(void);

// Drive the swap state machine (variant, exposure growth/reset, rebind).
// Call from main loop alongside wake_task() / btstack hci_run().
void usb_variant_task(void);

// True while a swap is in flight (between tud_disconnect() and the
// post-tud_connect() settle). wake.cpp uses this to ignore the
// tud_mount_cb / tud_resume_cb that fire as a consequence of our own
// re-enumeration — otherwise the wake FSM treats them as a host wake-up
// event and starts mashing F15 into the host (-> stray "fic" key spam).
bool usb_variant_swap_in_progress(void);

// Slot <-> TinyUSB HID instance mapping. FULL parse order with the wake
// keyboard: slot 0 gamepad = instance 0, keyboard = instance 1, then the
// extra gamepad interfaces (slots 1..N-1) = instances 2..N. Without the wake
// keyboard the gamepads are simply instances 0..N-1.
#ifdef ENABLE_WAKE_HID
static inline uint8_t usb_slot_hid_instance(uint8_t slot) {
    return slot == 0 ? 0 : (uint8_t) (slot + 1);
}
// Returns -1 for the keyboard instance.
static inline int usb_hid_instance_slot(uint8_t instance) {
    if (instance == 0) return 0;
    if (instance == 1) return -1;
    return (int) instance - 1;
}
#else
static inline uint8_t usb_slot_hid_instance(uint8_t slot) { return slot; }
static inline int usb_hid_instance_slot(uint8_t instance) { return (int) instance; }
#endif

#endif //DS5_BRIDGE_USB_H