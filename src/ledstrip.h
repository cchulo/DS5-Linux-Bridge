//
// WS2812B controller-status LEDs (ENABLE_LED_STRIP, data on LED_STRIP_GPIO).
//
// The physical chain is 8 pixels with alternating spacers: even pixels
// (0,2,4,6) are always dark, odd pixels (1,3,5,7) indicate controller slots
// 1-4. Per slot:
//   off             = no controller connected
//   solid slot color= connected (config slot_rgb; default blue #0000FF,
//                     always matches the pad's lightbar)
//   blinking yellow = battery <= 20% (discharging)
//   blinking red    = battery <= 10% (discharging; faster blink)
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

// Per-slot battery-state simulation, overlaid on the LIVE status display
// (other slots keep showing their real state). level: 0 = back to live,
// 1 = low (yellow blink, as at <=20%), 2 = critical (red blink, as at
// <=10%). slot < 0 applies the level to every slot. Same 60 s auto-revert
// as the other debug modes; ledstrip_debug_clear() also clears it.
void ledstrip_debug_slot_sim(int slot, int level);

// Leave debug mode and resume normal status rendering.
void ledstrip_debug_clear();

#endif // DS5_BRIDGE_LEDSTRIP_H
