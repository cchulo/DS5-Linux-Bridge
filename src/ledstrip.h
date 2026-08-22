//
// WS2812B controller-status LEDs (ENABLE_LED_STRIP, data on LED_STRIP_GPIO).
//
// Strip length (led_count, default 8, max LED_STRIP_MAX_PIXELS) and the
// pixels each slot lights (slot_led_mask bitmasks) are user-configured from
// the web UI, so any physical layout works (line, ring, square...). The
// default maps slot k to pixel 2k+1 with dark spacers between.
//
// Every state's color and animation (solid / blink / pulse / off; config
// led_anim[LED_STATE_*]) is user-configured. Defaults:
//   empty seat       = off (empty_rgb black; only rendered while other pads
//                      are connected — the all-empty strip is the idle state)
//   connected        = solid slot color (slot_rgb; default blue #0000FF,
//                      always matches the pad's lightbar)
//   battery <= 40%   = solid yellow (lowbatt_rgb; discharging only)
//   battery <= 20%   = blinking red (critbatt_rgb; discharging only)
//   pairing overlay  = solid white (pairing_rgb, pairing_led_mask)
//   idle (no pads,   = off by default (idle_rgb, whole strip; set an
//    not pairing)      animation to get the old "breathing blue" back)
// Global brightness is capped at 5%.
//
// Rendered from the main loop at ~30 Hz via a PIO state machine (claimed
// dynamically so it can never collide with the CYW43 radio's PIO SPI).
// Never blocks the BT/USB hot paths.
//

#ifndef DS5_BRIDGE_LEDSTRIP_H
#define DS5_BRIDGE_LEDSTRIP_H

// Claim a free PIO state machine and start the WS2812 program. Call after
// cyw43_arch_init() so the radio's PIO claim is already in place.
void ledstrip_init();

// Render one frame if the frame interval elapsed. Call every main-loop
// iteration; cheap no-op between frames.
void ledstrip_tick();

// ---- Debug override (web UI "LED debug") ----
// While active, the debug frame replaces normal rendering. Auto-reverts to
// normal 60 s after the last debug command so a forgotten test can't stick.
// The 10% brightness cap and gamma apply to debug output too.

#include <cstdint>

// Set one pixel (0-based chain position) to r/g/b; pixel < 0 sets the whole
// chain. Enters/refreshes debug mode.
void ledstrip_debug_set_pixel(int pixel, uint8_t r, uint8_t g, uint8_t b);

// Single lit pixel walking the chain in the given color.
void ledstrip_debug_chase(uint8_t r, uint8_t g, uint8_t b);

// Per-slot state simulation, overlaid on the LIVE status display (other
// slots keep showing their real state). level: 0 = back to live, 1 = low
// (yellow blink, as at <=40%), 2 = critical (red blink, as at <=20%),
// 3 = connected (steady slot color, as with a pad attached). slot < 0
// applies the level to every slot. Same 60 s auto-revert as the other
// debug modes; ledstrip_debug_clear() also clears it.
void ledstrip_debug_slot_sim(int slot, int level);

// Simulate pairing mode: forces the pairing overlay (configured pixels
// blinking in the pairing color) without touching the radio, so the blink
// can be previewed while programming the strip layout. Same 60 s
// auto-revert; ledstrip_debug_clear() also clears it.
void ledstrip_debug_pairing_sim(bool on);

// Simulate the idle "waiting for a controller" breathing: forces the
// whole-strip breathe (config idle_rgb) as a background even while
// controllers are connected, so the color/cadence can be previewed anytime.
// Same 60 s auto-revert; ledstrip_debug_clear() also clears it.
void ledstrip_debug_idle_sim(bool on);

// Leave debug mode and resume normal status rendering.
void ledstrip_debug_clear();

// Paint the whole strip one solid color NOW (initializing the PIO first if
// needed), for dongle-level status when the Pico's own LED is hidden by the
// mounting. The pixels latch and hold the frame until new data arrives, so
// it persists across a reboot — including into the ROM UF2 bootloader,
// which runs no code of ours — and is cleared by the first normal frame of
// a healthy boot. Brightness-capped like everything else.
void ledstrip_hold_solid(uint8_t r, uint8_t g, uint8_t b);

// Boot-error indicator: whole strip solid red (see ledstrip_hold_solid for
// the latching/clearing behavior — red through a boot-loop until a healthy
// boot renders).
void ledstrip_panic_red();

// WiFi-onboarding indicator: a single teal pixel chases along the strip
// (fixed color — teal is reserved for setup mode; same dwell as the debug
// chase). Call every iteration of the AP onboarding loop instead of
// ledstrip_tick(): unlike the normal tick it reads NO BT state, which is
// never initialised in that mode. The first frame also clears a latched
// panic-red from a prior crash blink.
void ledstrip_setup_chase_tick();

#endif // DS5_BRIDGE_LEDSTRIP_H
