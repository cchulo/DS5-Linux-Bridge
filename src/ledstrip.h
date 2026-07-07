//
// WS2812B controller-status LEDs (ENABLE_LED_STRIP, data on LED_STRIP_GPIO).
//
// The physical chain is 8 pixels with alternating spacers: even pixels
// (0,2,4,6) are always dark, odd pixels (1,3,5,7) indicate controller slots
// 1-4. Per slot:
//   off             = no controller connected
//   solid green     = connected
//   blinking yellow = battery <= 20% (discharging)
//   blinking red    = battery <= 10% (discharging; faster blink)
// Global brightness is capped at 10%.
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

// Simulate a low-battery state with the production colors and cadence:
// critical=false -> yellow blink (<=20%), critical=true -> red blink (<=10%,
// faster). pixel < 0 applies it to every slot-indicator pixel (spacers stay
// dark, matching what a real all-pads-dying strip would show).
void ledstrip_debug_lowbatt(int pixel, bool critical);

// Leave debug mode and resume normal status rendering.
void ledstrip_debug_clear();

#endif // DS5_BRIDGE_LEDSTRIP_H
