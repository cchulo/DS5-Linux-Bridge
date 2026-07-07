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
// Physical chain: alternating spacer/indicator pixels. Even pixels stay
// dark; slot k lights pixel 2k+1.
constexpr int PIXEL_COUNT = BT_MAX_SLOTS * 2;
constexpr int slot_pixel(int slot) { return slot * 2 + 1; }

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
uint8_t  dbg_px[PIXEL_COUNT][3];
// Per-slot low-battery simulation overlay: 0 = live status, 1 = low
// (yellow blink), 2 = critical (red blink). Unlike the global debug modes
// above, this overlays the NORMAL rendering, so one slot can preview a dying
// pad while the others keep showing their real state. Uses the exact
// production colors and cadence.
uint8_t  sim_level[BT_MAX_SLOTS] = {};
bool     sim_any = false;
uint64_t sim_until_us = 0;

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
    printf("[LED] WS2812B strip: %d pixels (%d slot indicators) on GP%d (PIO%d sm%d)\n",
           PIXEL_COUNT, BT_MAX_SLOTS, LED_STRIP_GPIO, pio_get_index(led_pio), led_sm);
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
    } else if (pixel < PIXEL_COUNT) {
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
    if (level < 0 || level > 2) return;
    if (slot < 0) {
        for (auto &s : sim_level) s = (uint8_t) level;
    } else if (slot < BT_MAX_SLOTS) {
        sim_level[slot] = (uint8_t) level;
    } else {
        return;
    }
    sim_any = false;
    for (auto s : sim_level) sim_any |= (s != 0);
    sim_until_us = time_us_64() + DEBUG_TIMEOUT_US;
    printf("[LED] debug slot sim: slot %d level %d\n", slot, level);
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
    sim_any = false;
    printf("[LED] debug cleared\n");
}

void ledstrip_tick() {
    if (!led_ready) return;
    const uint64_t now = time_us_64();
    if (now < next_frame_us) return;
    next_frame_us = now + FRAME_INTERVAL_US;

    const uint32_t ms = (uint32_t) (now / 1000);

    uint8_t frame[PIXEL_COUNT][3] = {}; // all dark, spacers stay that way

    if (dbg_active && now >= dbg_until_us) {
        dbg_active = false; // debug timed out; fall through to normal
        printf("[LED] debug timed out, back to normal\n");
    }
    if (dbg_active) {
        if (dbg_chase >= 0) {
            const int lit = (int) ((ms / DEBUG_CHASE_MS) % PIXEL_COUNT);
            frame[lit][0] = dbg_chase_rgb[0];
            frame[lit][1] = dbg_chase_rgb[1];
            frame[lit][2] = dbg_chase_rgb[2];
        } else {
            for (int i = 0; i < PIXEL_COUNT; i++) {
                frame[i][0] = dbg_px[i][0];
                frame[i][1] = dbg_px[i][1];
                frame[i][2] = dbg_px[i][2];
            }
        }
        for (int i = 0; i < PIXEL_COUNT; i++) {
            const uint32_t grb = ((uint32_t) shape(frame[i][1]) << 16) |
                                 ((uint32_t) shape(frame[i][0]) << 8) |
                                 (uint32_t) shape(frame[i][2]);
            pio_sm_put_blocking(led_pio, led_sm, grb << 8u);
        }
        return;
    }

    if (sim_any && now >= sim_until_us) {
        for (auto &s : sim_level) s = 0;
        sim_any = false;
        printf("[LED] slot sim timed out, back to live status\n");
    }

    for (uint8_t slot = 0; slot < BT_MAX_SLOTS; slot++) {
        // Simulation overlay: preview this slot's low/critical blink (real
        // colors and cadence) while the other slots keep live status.
        if (sim_any && sim_level[slot] != 0) {
            const bool crit = sim_level[slot] == 2;
            if (blink_on(ms, crit ? BLINK_RED_MS : BLINK_YELLOW_MS)) {
                const uint8_t *color = crit ? RED : YELLOW;
                uint8_t *px = frame[slot_pixel(slot)];
                px[0] = color[0];
                px[1] = color[1];
                px[2] = color[2];
            }
            continue;
        }

        BtStatus st;
        bt_get_status(slot, &st);
        if (!st.connected) continue; // off

        // Steady color = the slot's configured color (same as its lightbar).
        const uint8_t *color = get_config().slot_rgb[slot];
        bool on = true;
        // Low-battery blinks only while discharging: a charging pad at 10%
        // is recovering, not dying, so it shows its steady slot color.
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

        uint8_t *px = frame[slot_pixel(slot)];
        px[0] = color[0];
        px[1] = color[1];
        px[2] = color[2];
    }

    for (int i = 0; i < PIXEL_COUNT; i++) {
        const uint32_t grb = ((uint32_t) shape(frame[i][1]) << 16) |
                             ((uint32_t) shape(frame[i][0]) << 8) |
                             (uint32_t) shape(frame[i][2]);
        // TX FIFO is joined (8 deep) and drains at 30 us/pixel; at 8 pixels
        // per 33 ms frame this never meaningfully blocks.
        pio_sm_put_blocking(led_pio, led_sm, grb << 8u);
    }
}
