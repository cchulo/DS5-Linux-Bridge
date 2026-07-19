//
// WS2812B controller-status LEDs. See ledstrip.h for the state table.
//

#include "ledstrip.h"

#include <cmath>
#include <cstdio>

#include "hardware/pio.h"
#include "pico/time.h"

#include "bt.h"
#include "config.h"
#include "ws2812.pio.h"

#ifndef LED_STRIP_GPIO
#define LED_STRIP_GPIO 28
#endif

// ---- Tunables ----
namespace {
// Strip geometry is user-configured: led_count pixels (default 8) and a
// per-slot pixel bitmask (default: slot k lights pixel 2k+1), so any layout
// -- line, ring, square -- maps from the web UI. MAX_PIXELS bounds buffers;
// every frame pushes the full MAX so shrinking led_count can't strand lit
// pixels beyond the new end.
constexpr int MAX_PIXELS = LED_STRIP_MAX_PIXELS;

inline int led_count() { return get_config().led_count; }

// Paint every mask pixel below `count` with `color`.
inline void paint_mask(uint8_t frame[][3], uint32_t mask, int count,
                       const uint8_t *color) {
    for (int p = 0; p < count; p++) {
        if (mask & (1u << p)) {
            frame[p][0] = color[0];
            frame[p][1] = color[1];
            frame[p][2] = color[2];
        }
    }
}

// ~30 Hz. The inter-frame gap also serves as the WS2812B latch/reset time:
// newer (V5) batches need >= 280 us, and 33 ms clears that easily.
constexpr uint64_t FRAME_INTERVAL_US = 33'000;
// Global brightness cap, /255. 13 = 5% per the hardware spec; also keeps
// worst-case draw from VBUS negligible (4 lit LEDs well under 30 mA).
constexpr uint16_t BRIGHTNESS_CAP = 13;
constexpr float    GAMMA          = 2.2f;

// Connected color comes from the per-slot config (slot_rgb, default blue
// #0000FF), matching the pad's lightbar. Warning blinks stay fixed.
constexpr uint8_t YELLOW[3] = {255, 200, 0};
constexpr uint8_t RED[3]    = {255, 0, 0};

// Blink cadence (full period; 50% duty). Red blinks faster: it's the
// "controller is about to die" signal.
constexpr uint32_t BLINK_YELLOW_MS = 1000;
constexpr uint32_t BLINK_RED_MS    = 400;

// Deliberately early warnings: charging only from 10% is hard on the cell,
// so nudge at 40% and insist at 20%.
constexpr uint8_t LOW_BATT_YELLOW_PCT = 40;
constexpr uint8_t LOW_BATT_RED_PCT    = 20;

PIO      led_pio;
uint     led_sm;
uint     led_offset;
bool     led_ready = false;
uint64_t next_frame_us = 0;
uint8_t  gamma_lut[256];

// Debug override (see ledstrip.h). dbg_chase < 0 means static pixel values.
constexpr uint64_t DEBUG_TIMEOUT_US  = 60'000'000;
constexpr uint32_t DEBUG_CHASE_MS    = 150; // per-pixel dwell
bool     dbg_active = false;
uint64_t dbg_until_us = 0;
int      dbg_chase = -1;
uint8_t  dbg_chase_rgb[3];
uint8_t  dbg_px[MAX_PIXELS][3];
// Per-slot simulation overlay: 0 = live status, 1 = low battery (yellow
// blink), 2 = critical (red blink), 3 = connected (steady slot color).
// Unlike the global debug modes above, this overlays the NORMAL rendering,
// so one slot can preview a state while the others keep showing their real
// state. Uses the exact production colors and cadence. sim_pairing likewise
// forces the pairing overlay without touching the radio.
uint8_t  sim_level[BT_MAX_SLOTS] = {};
bool     sim_pairing = false;
bool     sim_any = false;
uint64_t sim_until_us = 0;

inline void sim_recompute_any() {
    sim_any = sim_pairing;
    for (auto s : sim_level) sim_any |= (s != 0);
}

// Gamma-correct then apply the global cap.
inline uint8_t shape(uint8_t v) {
    return (uint8_t) ((uint16_t) gamma_lut[v] * BRIGHTNESS_CAP / 255);
}

inline bool blink_on(uint32_t ms, uint32_t period) {
    return (ms % period) < period / 2;
}
} // namespace

void ledstrip_init() {
    for (int i = 0; i < 256; i++) {
        gamma_lut[i] = (uint8_t) (powf((float) i / 255.0f, GAMMA) * 255.0f + 0.5f);
    }
    // Dynamically claim a free state machine (any of the three PIO blocks on
    // RP2350); the CYW43 radio's PIO SPI claimed its SM in cyw43_arch_init(),
    // so this can never collide with it.
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(
            &ws2812_program, &led_pio, &led_sm, &led_offset, LED_STRIP_GPIO, 1, true)) {
        printf("[LED] No free PIO state machine; strip disabled\n");
        return;
    }
    ws2812_program_init(led_pio, led_sm, led_offset, LED_STRIP_GPIO, 800000.0f, false);
    led_ready = true;
    printf("[LED] WS2812B strip: %d pixels configured (max %d) on GP%d (PIO%d sm%d)\n",
           led_count(), MAX_PIXELS, LED_STRIP_GPIO, pio_get_index(led_pio), led_sm);
}

void ledstrip_debug_set_pixel(int pixel, uint8_t r, uint8_t g, uint8_t b) {
    if (!led_ready) return;
    if (!dbg_active || dbg_chase >= 0) {
        // Entering static debug mode: start from an all-dark canvas.
        for (auto &px : dbg_px) px[0] = px[1] = px[2] = 0;
    }
    dbg_chase = -1;
    if (pixel < 0) {
        for (auto &px : dbg_px) { px[0] = r; px[1] = g; px[2] = b; }
    } else if (pixel < MAX_PIXELS) {
        dbg_px[pixel][0] = r;
        dbg_px[pixel][1] = g;
        dbg_px[pixel][2] = b;
    }
    dbg_active = true;
    dbg_until_us = time_us_64() + DEBUG_TIMEOUT_US;
    printf("[LED] debug set pixel %d = %u,%u,%u\n", pixel, r, g, b);
}

void ledstrip_debug_slot_sim(int slot, int level) {
    if (!led_ready) return;
    if (level < 0 || level > 3) return;
    if (slot < 0) {
        for (auto &s : sim_level) s = (uint8_t) level;
    } else if (slot < BT_MAX_SLOTS) {
        sim_level[slot] = (uint8_t) level;
    } else {
        return;
    }
    sim_recompute_any();
    sim_until_us = time_us_64() + DEBUG_TIMEOUT_US;
    printf("[LED] debug slot sim: slot %d level %d\n", slot, level);
}

void ledstrip_debug_pairing_sim(bool on) {
    if (!led_ready) return;
    sim_pairing = on;
    sim_recompute_any();
    sim_until_us = time_us_64() + DEBUG_TIMEOUT_US;
    printf("[LED] debug pairing sim: %s\n", on ? "on" : "off");
}

void ledstrip_debug_chase(uint8_t r, uint8_t g, uint8_t b) {
    if (!led_ready) return;
    dbg_chase = 1;
    dbg_chase_rgb[0] = r;
    dbg_chase_rgb[1] = g;
    dbg_chase_rgb[2] = b;
    dbg_active = true;
    dbg_until_us = time_us_64() + DEBUG_TIMEOUT_US;
    printf("[LED] debug chase %u,%u,%u\n", r, g, b);
}

void ledstrip_debug_clear() {
    dbg_active = false;
    for (auto &s : sim_level) s = 0;
    sim_pairing = false;
    sim_any = false;
    printf("[LED] debug cleared\n");
}

void ledstrip_tick() {
    if (!led_ready) return;
    const uint64_t now = time_us_64();
    if (now < next_frame_us) return;
    next_frame_us = now + FRAME_INTERVAL_US;

    const uint32_t ms = (uint32_t) (now / 1000);
    const int count = led_count();

    uint8_t frame[MAX_PIXELS][3] = {}; // all dark; unmapped pixels stay dark

    if (dbg_active && now >= dbg_until_us) {
        dbg_active = false; // debug timed out; fall through to normal
        printf("[LED] debug timed out, back to normal\n");
    }
    if (dbg_active) {
        if (dbg_chase >= 0) {
            const int lit = (int) ((ms / DEBUG_CHASE_MS) % count);
            frame[lit][0] = dbg_chase_rgb[0];
            frame[lit][1] = dbg_chase_rgb[1];
            frame[lit][2] = dbg_chase_rgb[2];
        } else {
            for (int i = 0; i < MAX_PIXELS; i++) {
                frame[i][0] = dbg_px[i][0];
                frame[i][1] = dbg_px[i][1];
                frame[i][2] = dbg_px[i][2];
            }
        }
    } else {
        if (sim_any && now >= sim_until_us) {
            for (auto &s : sim_level) s = 0;
            sim_pairing = false;
            sim_any = false;
            printf("[LED] slot sim timed out, back to live status\n");
        }

        for (uint8_t slot = 0; slot < BT_MAX_SLOTS; slot++) {
            const uint32_t mask = get_config().slot_led_mask[slot];

            // Simulation overlay: preview this slot's state (real colors and
            // cadence) while other slots keep live status. 3 = connected
            // (steady slot color), 1/2 = low/critical battery blink.
            if (sim_any && sim_level[slot] != 0) {
                if (sim_level[slot] == 3) {
                    paint_mask(frame, mask, count, get_config().slot_rgb[slot]);
                } else {
                    const bool crit = sim_level[slot] == 2;
                    if (blink_on(ms, crit ? BLINK_RED_MS : BLINK_YELLOW_MS)) {
                        paint_mask(frame, mask, count, crit ? RED : YELLOW);
                    }
                }
                continue;
            }

            BtStatus st;
            bt_get_status(slot, &st);
            if (!st.connected) continue; // off

            // Steady color = the slot's configured color (same as its lightbar).
            const uint8_t *color = get_config().slot_rgb[slot];
            bool on = true;
            // Low-battery blinks only while discharging: a charging pad is
            // recovering, not dying, so it shows its steady slot color.
            if (st.battery_valid && !st.charging) {
                if (st.battery_pct <= LOW_BATT_RED_PCT) {
                    color = RED;
                    on = blink_on(ms, BLINK_RED_MS);
                } else if (st.battery_pct <= LOW_BATT_YELLOW_PCT) {
                    color = YELLOW;
                    on = blink_on(ms, BLINK_YELLOW_MS);
                }
            }
            if (!on) continue;

            paint_mask(frame, mask, count, color);
        }

        // Pairing-mode overlay: blink the configured pixels in the
        // configured color (default white) while the dongle is searching
        // for a controller (~2 Hz, like the onboard LED's pairing blink;
        // painted last so it wins shared pixels). Independent of
        // disable_pico_led -- that switch only covers the onboard LED.
        if ((bt_pairing_mode_active() || (sim_any && sim_pairing)) &&
            blink_on(ms, 500)) {
            paint_mask(frame, get_config().pairing_led_mask, count,
                       get_config().pairing_rgb);
        }
    }

    // Always push the full MAX so a reduced led_count immediately darkens
    // pixels past the new end. TX FIFO is joined (8 deep) and drains at
    // 30 us/pixel; 32 pixels block well under 1 ms per 33 ms frame.
    for (int i = 0; i < MAX_PIXELS; i++) {
        const uint32_t grb = ((uint32_t) shape(frame[i][1]) << 16) |
                             ((uint32_t) shape(frame[i][0]) << 8) |
                             (uint32_t) shape(frame[i][2]);
        pio_sm_put_blocking(led_pio, led_sm, grb << 8u);
    }
}
