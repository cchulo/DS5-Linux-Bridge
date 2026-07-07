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

#endif // DS5_BRIDGE_LEDSTRIP_H
