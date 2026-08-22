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

// Every state's color AND animation (solid/blink/pulse/off) now comes from
// config: slot_rgb per seat, empty_rgb / lowbatt_rgb / critbatt_rgb /
// pairing_rgb / idle_rgb, and led_anim[LED_STATE_*] (defaults resolved in
// config_valid(): all solid except critical battery = blink, idle = off).
// Only the cadences stay fixed:

// Blink cadence (full period; 50% duty). Critical blinks faster: it's the
// "controller is about to die" signal. Pairing keeps its ~2 Hz identity.
constexpr uint32_t BLINK_MS         = 1000;
constexpr uint32_t BLINK_CRIT_MS    = 400;
constexpr uint32_t BLINK_PAIRING_MS = 500;

// Pulse ("breathe") full in+out cycle length, like a DS5 searching for its
// console.
constexpr uint32_t BREATHE_MS = 3000;

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
// forces the pairing overlay without touching the radio, and sim_idle
// forces the idle "waiting" breathing even while controllers are connected.
uint8_t  sim_level[BT_MAX_SLOTS] = {};
bool     sim_pairing = false;
bool     sim_idle = false;
bool     sim_any = false;
uint64_t sim_until_us = 0;

inline void sim_recompute_any() {
    sim_any = sim_pairing || sim_idle;
    for (auto s : sim_level) sim_any |= (s != 0);
}

// Gamma-correct then apply the global cap.
inline uint8_t shape(uint8_t v) {
    return (uint8_t) ((uint16_t) gamma_lut[v] * BRIGHTNESS_CAP / 255);
}

inline bool blink_on(uint32_t ms, uint32_t period) {
    return (ms % period) < period / 2;
}

// Animation envelope for this frame: 1 = full color, 0 = dark, in-between =
// pulse fade. `epoch_ms` anchors the pulse phase (pass `ms` for free-running).
inline float anim_lvl(uint8_t anim, uint32_t ms, uint32_t blink_period,
                      uint32_t epoch_ms = 0) {
    switch (anim) {
        case LED_ANIM_BLINK:
            return blink_on(ms, blink_period) ? 1.0f : 0.0f;
        case LED_ANIM_PULSE: {
            // Raised-cosine fade, scaled pre-gamma so the ramp looks even.
            constexpr float TWO_PI = 6.2831853f;
            return 0.5f - 0.5f * cosf((float) ((ms - epoch_ms) % BREATHE_MS) *
                                      (TWO_PI / (float) BREATHE_MS));
        }
        case LED_ANIM_OFF:
            return 0.0f;
        default: // LED_ANIM_SOLID
            return 1.0f;
    }
}

// paint_mask with the color scaled by an animation level. lvl 0 skips the
// paint entirely (underlying pixels show through, same as before).
inline void paint_mask_lvl(uint8_t frame[][3], uint32_t mask, int count,
                           const uint8_t *color, float lvl) {
    if (lvl <= 0.0f) return;
    const uint8_t scaled[3] = {
        (uint8_t) ((float) color[0] * lvl + 0.5f),
        (uint8_t) ((float) color[1] * lvl + 0.5f),
        (uint8_t) ((float) color[2] * lvl + 0.5f),
    };
    paint_mask(frame, mask, count, scaled);
}

// Paint one seat according to a configured state (color + animation). Used
// by both live status and the web-UI simulation overlay, so previews always
// match production exactly.
inline void paint_seat_state(uint8_t frame[][3], uint32_t mask, int count,
                             uint32_t ms, int state, uint8_t slot) {
    const Config_body &cfg = get_config();
    const uint8_t *color = cfg.slot_rgb[slot];
    uint32_t period = BLINK_MS;
    switch (state) {
        case LED_STATE_EMPTY:    color = cfg.empty_rgb; break;
        case LED_STATE_LOWBATT:  color = cfg.lowbatt_rgb; break;
        case LED_STATE_CRITBATT: color = cfg.critbatt_rgb; period = BLINK_CRIT_MS; break;
        default: break; // LED_STATE_CONN: slot color
    }
    paint_mask_lvl(frame, mask, count, color,
                   anim_lvl(cfg.led_anim[state], ms, period));
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

void ledstrip_debug_idle_sim(bool on) {
    if (!led_ready) return;
    sim_idle = on;
    sim_recompute_any();
    sim_until_us = time_us_64() + DEBUG_TIMEOUT_US;
    printf("[LED] debug idle sim: %s\n", on ? "on" : "off");
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
    sim_idle = false;
    sim_any = false;
    printf("[LED] debug cleared\n");
}

void ledstrip_hold_solid(uint8_t r, uint8_t g, uint8_t b) {
    if (!led_ready) {
        // Callers can run before the normal init (e.g. radio never came
        // up). PIO-only, so it works regardless of the radio's state.
        ledstrip_init();
        if (!led_ready) return;
    }
    // All MAX_PIXELS, not led_count(): the config may not be loaded yet,
    // and a status this important should be visible on every physically
    // attached pixel.
    const uint32_t grb = ((uint32_t) shape(g) << 16) |
                         ((uint32_t) shape(r) << 8) |
                         (uint32_t) shape(b);
    for (int i = 0; i < MAX_PIXELS; i++) {
        pio_sm_put_blocking(led_pio, led_sm, grb << 8u);
    }
    // Let the FIFO drain before a caller reboots: blocking puts only
    // guarantee QUEUED (FIFO is 8 deep at ~30 us/pixel). The pixels then
    // hold this frame until someone sends new data — it survives a reboot
    // (and the ROM bootloader, which never touches the data pin) and is
    // cleared by the first normal ledstrip_tick() frame of a healthy boot.
    sleep_ms(1);
}

void ledstrip_panic_red() { ledstrip_hold_solid(255, 0, 0); }

void ledstrip_setup_chase_tick() {
    if (!led_ready) return;
    const uint64_t now = time_us_64();
    if (now < next_frame_us) return;
    next_frame_us = now + FRAME_INTERVAL_US;
    const uint32_t ms = (uint32_t) (now / 1000);

    // Deliberately self-contained: the WiFi-onboarding loop is the only
    // caller, and there BT was never initialised, so unlike ledstrip_tick()
    // this must not read bt_get_status()/bt_pairing_mode_active(). Teal is
    // reserved for setup mode (no other status uses it), fixed rather than
    // configurable so it is recognizable even on a fresh/factory-reset config.
    // Full-scale channels: every other status color drives at least one
    // channel at 255, and the shared gamma curve crushes mid-range values --
    // the earlier {0,96,128} gamma'd down to ~1/5 the brightness of the
    // red/blue indicators and read as faint. Same 3:4 green:blue hue.
    constexpr uint8_t SETUP_TEAL[3] = {0, 191, 255};

    // Single teal pixel walking the chain — same dwell as the debug chase, a
    // deliberately different motion from every status animation so setup mode
    // can't be mistaken for the idle breathe.
    const int count = led_count();
    const int lit = (int) ((ms / DEBUG_CHASE_MS) % count);
    // Push the full MAX like ledstrip_tick(): pixels past led_count stay
    // dark, and a stale held frame (e.g. latched panic red from a prior
    // crash blink) is fully overwritten from the first frame.
    for (int i = 0; i < MAX_PIXELS; i++) {
        const bool on = i == lit;
        const uint32_t grb = ((uint32_t) shape(on ? SETUP_TEAL[1] : 0) << 16) |
                             ((uint32_t) shape(on ? SETUP_TEAL[0] : 0) << 8) |
                             (uint32_t) shape(on ? SETUP_TEAL[2] : 0);
        pio_sm_put_blocking(led_pio, led_sm, grb << 8u);
    }
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
            sim_idle = false;
            sim_any = false;
            printf("[LED] slot sim timed out, back to live status\n");
        }

        const bool pairing_now =
            bt_pairing_mode_active() || (sim_any && sim_pairing);

        // Idle "waiting for a controller" breathing: shown when there is
        // nothing else to show (no pad connected, no sim overlay, not
        // pairing) — or forced by the idle sim, which paints it as a
        // background under whatever else is live so the color/cadence can
        // be previewed anytime. Painted first, so connected slots and the
        // pairing overlay always win their pixels.
        bool idle_now = !pairing_now && !sim_any;
        if (idle_now) {
            for (uint8_t slot = 0; slot < BT_MAX_SLOTS && idle_now; slot++) {
                BtStatus st;
                bt_get_status(slot, &st);
                idle_now = !st.connected;
            }
        }
        const bool idle_show = idle_now || (sim_any && sim_idle);
        // Phase-anchor a pulsing idle to the moment it becomes active, so it
        // always starts from dark and fades in — free-running uptime phase
        // would make the strip jump to whatever brightness the cycle
        // happened to be at (an abrupt solid-color pop).
        static uint32_t idle_epoch_ms = 0;
        static bool idle_prev = false;
        if (idle_show && !idle_prev) idle_epoch_ms = ms;
        idle_prev = idle_show;
        if (idle_show) {
            // Whole strip in the configured idle color + animation (default:
            // off -- the strip stays dark until a controller connects).
            const float lvl = anim_lvl(get_config().led_anim[LED_STATE_IDLE],
                                       ms, BLINK_MS, idle_epoch_ms);
            const uint32_t all = (count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1);
            paint_mask_lvl(frame, all, count, get_config().idle_rgb, lvl);
        }

        for (uint8_t slot = 0; slot < BT_MAX_SLOTS; slot++) {
            const uint32_t mask = get_config().slot_led_mask[slot];

            // Simulation overlay: preview this slot's state (real colors and
            // animation, via the shared paint_seat_state) while other slots
            // keep live status. 3 = connected, 1/2 = low/critical battery.
            if (sim_any && sim_level[slot] != 0) {
                const int st = sim_level[slot] == 3   ? LED_STATE_CONN
                               : sim_level[slot] == 2 ? LED_STATE_CRITBATT
                                                      : LED_STATE_LOWBATT;
                paint_seat_state(frame, mask, count, ms, st, slot);
                continue;
            }

            BtStatus st;
            bt_get_status(slot, &st);
            if (!st.connected) {
                // Empty seat. Only rendered while something else is on the
                // strip (idle owns the all-empty case); default color is
                // black, so out of the box this stays dark as before.
                if (!idle_now) {
                    paint_seat_state(frame, mask, count, ms, LED_STATE_EMPTY, slot);
                }
                continue;
            }

            // Connected: the slot's configured color (same as its lightbar),
            // overridden by the battery warnings while discharging: a
            // charging pad is recovering, not dying, so it shows its steady
            // slot color.
            int state = LED_STATE_CONN;
            if (st.battery_valid && !st.charging) {
                if (st.battery_pct <= LOW_BATT_RED_PCT) state = LED_STATE_CRITBATT;
                else if (st.battery_pct <= LOW_BATT_YELLOW_PCT) state = LED_STATE_LOWBATT;
            }
            paint_seat_state(frame, mask, count, ms, state, slot);
        }

        // Pairing-mode overlay: the configured pixels in the configured color
        // (default white, solid) while the dongle is searching for a
        // controller; painted last so it wins shared pixels. Independent of
        // disable_pico_led -- that switch only covers the onboard LED.
        if (pairing_now) {
            paint_mask_lvl(frame, get_config().pairing_led_mask, count,
                           get_config().pairing_rgb,
                           anim_lvl(get_config().led_anim[LED_STATE_PAIRING],
                                    ms, BLINK_PAIRING_MS));
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
