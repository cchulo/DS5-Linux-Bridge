//
// Created by awalol on 2026/4/30.
//

#include "wake.h"

#ifdef ENABLE_WAKE_HID

#include <cstdio>
#include <cstring>
#include "tusb.h"
#include "device/dcd.h"
#include "pico/sync.h"
#include "pico/time.h"
#include "bt.h"
#include "usb.h"

// The boot keyboard is HID instance 1 in BOTH descriptor variants (a dummy
// placeholder HID holds instance 0 in minimal — see usb_descriptors.cpp), so
// this is stable across variant swaps.
#define WAKE_KBD_INSTANCE     (usb_kbd_hid_instance())
#define WAKE_KEYCODE_F15      0x68
// Post-resume timings tuned for "wake-and-resleep" Windows behavior: the host
// resumes USB, but if no HID input is consumed during the brief wake window
// the system can re-suspend within ~1 s. Bigger settles + a second F15 give
// Windows multiple polling cycles to pick the keystroke up.
#define WAKE_SETTLE_US        150000   // 150 ms — let host finish USB re-init
#define WAKE_KEY_HOLD_US       80000   // 80 ms keydown -> keyup gap
#define WAKE_KEY_UP_SETTLE_US 200000   // 200 ms between attempts (or before DONE)
#define WAKE_REQUEST_TIMEOUT_US 5000000
#define WAKE_KEY_ATTEMPTS     2

#ifdef WAKE_DEBUG
#  define WAKE_DBG(fmt, ...) printf("[wake] " fmt "\n", ##__VA_ARGS__)
static const char *wake_state_name(int s) {
    switch (s) {
    case 0: return "IDLE";
    case 1: return "PENDING_PRESS";
    case 2: return "REQUESTED";
    case 3: return "KEY_DOWN";
    case 4: return "KEY_UP_SENT";
    case 5: return "DONE";
    default: return "?";
    }
}
#else
#  define WAKE_DBG(fmt, ...) ((void)0)
#endif

typedef enum {
    WAKE_IDLE,
    WAKE_PENDING_PRESS,
    WAKE_REQUESTED,
    WAKE_KEY_DOWN,
    WAKE_KEY_UP_SENT,
    WAKE_DONE,
} wake_state_t;

static critical_section_t wake_cs;
static volatile bool host_suspended = false;
static volatile bool host_resumed_event = false;
static wake_state_t state = WAKE_IDLE;
static uint64_t state_entered_us = 0;
static uint8_t key_attempts = 0;
// Last-seen DualSense button bytes. Idle defaults: byte 7 = 0x08 (D-pad
// released), bytes 8 / 9 = 0 (no shoulders, no PS / touchpad / mute).
static uint8_t prev_b7 = 0x08;
static uint8_t prev_b8 = 0x00;
static uint8_t prev_b9 = 0x00;

static void enter_state(wake_state_t s) {
    state = s;
    state_entered_us = time_us_64();
}

// Wake-on-LAN companion send (ENABLE_WIFI_WOL). Weak no-op default; the WiFi
// transport provides the strong override (wifi_net.cpp). wake.cpp owns the
// decision of WHEN to wake; the transport owns HOW to emit the packet.
extern "C" __attribute__((weak)) bool wake_emit_wol(void) { return false; }

// Rate-limit WOL to once per suspend spell, with a hard time floor surviving
// the connect/disconnect churn during a single wake. We CANNOT distinguish S3
// from S4/S5 over USB on this hardware (the suspend callback fires for all,
// the device may or may not also unmount, with no signal telling them apart).
// So on every warranted wake-while-suspended we fire BOTH the USB
// remote-wakeup (wakes S3) AND a WOL packet (wakes S4/S5 via the NIC). A
// stray WOL during S3 is harmless.
static volatile bool     wol_fired_this_spell = false;
static volatile uint64_t last_wol_us = 0;
static constexpr uint64_t WAKE_WOL_MIN_INTERVAL_US = 10ULL * 1000000ULL; // 10 s

static void maybe_emit_wol(const char *reason) {
    (void) reason;
    // CRITICAL: only emit WOL when the host is actually suspended. Unlike the
    // USB tud_remote_wakeup() below -- which is a harmless no-op when the bus
    // is awake, so request_host_wake() calls it speculatively even from the
    // button-event path while the host is up -- a WOL packet is NOT a no-op:
    // it broadcasts onto the LAN. Without this gate, every controller button
    // press during normal gameplay (host awake) would fire a magic packet
    // (upstream observed exactly that).
    if (!host_suspended) return;
    const uint64_t now = time_us_64();
    if (wol_fired_this_spell) return;
    if (last_wol_us != 0 && (now - last_wol_us) < WAKE_WOL_MIN_INTERVAL_US) return;
    if (wake_emit_wol()) {
        wol_fired_this_spell = true;
        last_wol_us = now;
        WAKE_DBG("%s -> WOL sent", reason);
    }
}

// Issue a USB remote-wakeup to the host and, on success, advance the wake FSM
// to WAKE_REQUESTED so wake_task() drives the follow-up keystroke sequence.
// Shared by the button-event path (wake_on_bt_input) and the connect path
// (wake_on_bt_connect). Callers are responsible for the gating checks
// (armable state, !variant-swap, etc.) before calling.
//
// Also fires a Wake-on-LAN packet first (suspend-gated, once per suspend
// spell -- see maybe_emit_wol): USB remote-wakeup covers S3, WOL covers S4/S5.
static void request_host_wake(const char *reason) {
    (void)reason;
    maybe_emit_wol(reason);
    bool ok = tud_remote_wakeup();

    // Linux quirk: Sometimes Linux fails to set the REMOTE_WAKEUP feature
    // flag before the second suspend, causing TinyUSB to refuse to wake.
    // If we are suspended but ok is false, we force the wake signal.
    if (!ok && host_suspended) {
        WAKE_DBG("%s: tud_remote_wakeup()=0 but suspended. Forcing DCD wake.", reason);
        dcd_remote_wakeup(0);
        ok = true;
    }

    if (ok) {
        critical_section_enter_blocking(&wake_cs);
        state = WAKE_REQUESTED;
        state_entered_us = time_us_64();
        critical_section_exit(&wake_cs);
        WAKE_DBG("%s -> REQUESTED, tud_remote_wakeup()=1", reason);
    }
#ifdef WAKE_DEBUG
    else {
        static uint64_t last_log = 0;
        const uint64_t now = time_us_64();
        if (now - last_log > 5000000) {
            WAKE_DBG("%s, tud_remote_wakeup()=0 (USB bus not in suspend) -- 5s heartbeat", reason);
            last_log = now;
        }
    }
#endif
}

// Resume the USB bus WITHOUT advancing the wake-FSM keystroke sequence.
//
// This is the cold-boot autosuspend recovery path (issue #4). On some hosts the
// idle MINIMAL interface is selective-suspended right after enumeration and the
// host never re-mounts it on its own. When a controller then connects, the
// MINIMAL->FULL variant swap is requested but usb_variant_task() refuses to
// re-enumerate while host_suspended -- so the gamepad never appears (lightbar /
// audio path are up over BT, but the host sees no input device). request_host_wake()
// only fires ONCE at connect time; if that single attempt doesn't take, the swap
// stays gated forever. usb_variant_task() calls this repeatedly (rate-limited)
// while a swap is desired-but-gated, to keep nudging the host to resume the bus
// so tud_resume_cb/tud_mount_cb can clear the gate and let the swap proceed.
//
// Deliberately does NOT touch the F15 keystroke FSM: this is not a wake-from-S3
// (the host is awake, it just selective-suspended an idle interface), so there
// is no host to wake with a keypress -- we only need the bus back up. Returns
// true if a resume was issued.
bool wake_request_bus_resume(void) {
    bool ok = tud_remote_wakeup();

    // Same Linux quirk handled in request_host_wake(): the host may have
    // suspended us without arming REMOTE_WAKEUP, so tud_remote_wakeup() returns
    // false. Force the resume at the DCD level when we know we're suspended.
    if (!ok && host_suspended) {
        dcd_remote_wakeup(0);
        ok = true;
    }
    return ok;
}

void wake_init(void) {
    critical_section_init(&wake_cs);
}

// Debounced DualSense power-off on host suspend.
// Armed in tud_suspend_cb, cancelled by tud_resume_cb / tud_mount_cb,
// fired by wake_task once the debounce window elapses. Debounce avoids
// killing the controller during brief suspend/resume blips on Linux S5
// wake, which leaves hid-playstation wedged until replug.
static volatile bool     power_off_armed = false;
static volatile uint64_t power_off_armed_at_us = 0;
static constexpr uint64_t POWER_OFF_DEBOUNCE_US = 10ULL * 1000000ULL; // 10 s

extern "C" void tud_suspend_cb(bool remote_wakeup_en) {
    WAKE_DBG("tud_suspend_cb remote_wakeup_en=%d prev_state=%s",
             (int)remote_wakeup_en, wake_state_name(state));
    host_suspended = true;
    wol_fired_this_spell = false; // new suspend spell -> allow one WOL again
    host_resumed_event = false;
    usb_set_host_suspended(true);

    // Arm the deferred DualSense power-off. wake_task() will fire it after
    // POWER_OFF_DEBOUNCE_US unless tud_resume_cb / tud_mount_cb cancel it
    // first. BTstack calls aren't safe from ISR context anyway. The
    // 64-bit timestamp write is paired with the wake_task reader under
    // wake_cs to prevent a torn read across the two 32-bit halves.
    critical_section_enter_blocking(&wake_cs);
    power_off_armed_at_us = time_us_64();
    power_off_armed = true;
    critical_section_exit(&wake_cs);

    // Unconditionally re-arm on suspend. If a previous wake attempt hung
    // (e.g. Linux ignored a keystroke and left the endpoint busy forever),
    // we must abort and reset so the NEXT wake attempt can trigger.
    state = WAKE_PENDING_PRESS;
    state_entered_us = time_us_64();
    prev_b7 = 0x08; prev_b8 = 0x00; prev_b9 = 0x00;
    key_attempts = 0;
    WAKE_DBG("-> PENDING_PRESS");
}

extern "C" void tud_resume_cb(void) {
    const bool swap = usb_variant_swap_in_progress();
    WAKE_DBG("tud_resume_cb state=%s armed=%d swap=%d",
             wake_state_name(state), (int)power_off_armed, (int)swap);
    // Bus-state bookkeeping always runs: host_suspended must reflect
    // reality so wake_on_bt_input / wake_task make correct decisions
    // even if a genuine host wake lands inside the variant-swap window
    // (the same PS press that triggered the swap can also be the press
    // that woke the host — observed on Linux S5).
    host_suspended = false;
    power_off_armed = false; // cancel pending power-off
    usb_set_host_suspended(false);
    // Only the FSM-arming flag is suppressed during a swap: this is the
    // resume our own tud_connect generated, not a real wake event, and
    // letting the FSM act on it caused the "fic" key spam.
    if (!swap) host_resumed_event = true;
}

extern "C" void tud_mount_cb(void) {
    const bool swap = usb_variant_swap_in_progress();
    WAKE_DBG("tud_mount_cb state=%s armed=%d swap=%d",
             wake_state_name(state), (int)power_off_armed, (int)swap);
    host_suspended = false;
    power_off_armed = false;
    usb_set_host_suspended(false);
    if (!swap) host_resumed_event = true;
}

void wake_on_bt_input(const uint8_t *hid_input, uint16_t len) {
    if (len < 10) return;
    // DualSense BT 0x31 input report layout (after main.cpp's `data + 3` skip):
    //   byte 7 low nibble: D-pad direction (0x08 idle); high nibble: face buttons
    //   byte 8: L1, R1, L2 click, R2 click, share, options, L3, R3
    //   byte 9: PS (bit 0), touchpad-click (bit 1), mute (bit 2)
    //
    // We trigger on ANY change in those three button bytes, not strictly on
    // the PS bit. Reasons:
    //   1. The DualSense's BT radio enters a low-power sniff mode after a
    //      period of inactivity. The PS button alone often does not wake
    //      the radio out of sniff -- shoulder buttons reliably do. So the
    //      first BT report after S3 is most likely whichever button the
    //      user happened to press to wake the radio. PS itself counts as
    //      "any button" too, so the single-press UX still works.
    //   2. We additionally call tud_remote_wakeup() speculatively even from
    //      WAKE_IDLE / WAKE_DONE state. TinyUSB returns true only when the
    //      host actually USB-suspended the bus; otherwise it's a no-op. This
    //      protects against the case where tud_suspend_cb didn't fire (e.g.
    //      a hub between the host and the dongle masking the suspend signal
    //      from downstream). On success the FSM transitions to REQUESTED and
    //      proceeds with the keystroke as normal.
    const uint8_t b7 = hid_input[7];
    const uint8_t b8 = hid_input[8];
    const uint8_t b9 = hid_input[9];

    critical_section_enter_blocking(&wake_cs);
    const bool changed = (b7 != prev_b7) || (b8 != prev_b8) || (b9 != prev_b9);
    const bool armable = (state == WAKE_IDLE || state == WAKE_DONE || state == WAKE_PENDING_PRESS);
    prev_b7 = b7; prev_b8 = b8; prev_b9 = b9;
    critical_section_exit(&wake_cs);

    // Don't try to wake while a variant swap is in flight — both
    // tud_remote_wakeup() and our subsequent F15 keystrokes would race
    // the re-enumeration the swap is doing.
    if (changed && armable && !usb_variant_swap_in_progress()) {
        request_host_wake("button event");
    }
}

// Wake the host when the DualSense establishes its BT connection while the
// host is suspended (turn-on-controller-to-wake, no button press needed).
//
// Safe vs the S5 churn that the power-off debounce guards against: on connect
// we call usb_request_variant_full(), but the variant-swap state machine is
// gated on !host_suspended (usb_variant_task returns early while suspended),
// so the re-enumeration is DEFERRED until the host actually wakes. That means
// usb_variant_swap_in_progress() is still false here, and waking the host is
// exactly what lets the deferred swap proceed. We additionally require
// host_suspended so a normal (host-awake) reconnect never triggers a wake.
void wake_on_bt_connect(void) {
    critical_section_enter_blocking(&wake_cs);
    const bool armable = (state == WAKE_IDLE || state == WAKE_DONE ||
                          state == WAKE_PENDING_PRESS);
    critical_section_exit(&wake_cs);

    if (host_suspended && armable && !usb_variant_swap_in_progress()) {
        request_host_wake("BT connect while suspended");
    }
}

void wake_on_bt_disconnect(void) {
    critical_section_enter_blocking(&wake_cs);
    state = WAKE_IDLE;
    prev_b7 = 0x08; prev_b8 = 0x00; prev_b9 = 0x00;
    key_attempts = 0;
    critical_section_exit(&wake_cs);
}

// Called by the USB variant-swap orchestrator before it bounces the
// bus. Any in-flight wake keystroke would land in the wrong
// enumeration, and the FSM's hid_n_ready waits would fire spurious
// F15s after the re-enumeration completes. Reset to IDLE so the FSM
// re-arms cleanly on the next genuine suspend.
void wake_reset_for_variant_swap(void) {
    critical_section_enter_blocking(&wake_cs);
    state = WAKE_IDLE;
    key_attempts = 0;
    host_resumed_event = false;
    critical_section_exit(&wake_cs);
}

void wake_task(void) {
    const uint64_t now = time_us_64();

    // Fire the deferred DualSense power-off if the debounce window has
    // elapsed without a resume cancelling it. Checked before the early-return
    // on idle FSM states so it still fires regardless of wake-FSM state.
    // bt_dualsense_power_off is a no-op if no controller is connected.
    // Snapshot armed+timestamp atomically under wake_cs so the ISR setter
    // can't tear the 64-bit timestamp across our comparison.
    critical_section_enter_blocking(&wake_cs);
    const bool     armed_now    = power_off_armed;
    const uint64_t armed_at_now = power_off_armed_at_us;
    critical_section_exit(&wake_cs);
    if (armed_now && (now - armed_at_now) >= POWER_OFF_DEBOUNCE_US) {
        power_off_armed = false;
        bt_dualsense_power_off();
        WAKE_DBG("dispatched DualSense power-off (debounce %llu ms elapsed)",
                 (unsigned long long)(POWER_OFF_DEBOUNCE_US / 1000));
    }

    critical_section_enter_blocking(&wake_cs);
    const wake_state_t s = state;
    const uint64_t entered = state_entered_us;
    critical_section_exit(&wake_cs);

    switch (s) {
        case WAKE_IDLE:
        case WAKE_PENDING_PRESS:
        case WAKE_DONE:
            return;

        case WAKE_REQUESTED: {
            if (host_resumed_event || !host_suspended) {
                host_resumed_event = false;
                if (now - entered < WAKE_SETTLE_US) return;
                if (!tud_hid_n_ready(WAKE_KBD_INSTANCE)) {
#ifdef WAKE_DEBUG
                    static uint64_t last_log = 0;
                    if (now - last_log > 1000000) {
                        WAKE_DBG("REQUESTED waiting: hid_n_ready=0 (heartbeat 1Hz)");
                        last_log = now;
                    }
#endif
                    return;
                }
                uint8_t rpt[8] = { 0, 0, WAKE_KEYCODE_F15, 0, 0, 0, 0, 0 };
                const bool sent = tud_hid_n_report(WAKE_KBD_INSTANCE, 0, rpt, sizeof(rpt));
                WAKE_DBG("REQUESTED: sent keydown 0x%02X -> %d", WAKE_KEYCODE_F15, (int)sent);
                if (sent) {
                    critical_section_enter_blocking(&wake_cs);
                    enter_state(WAKE_KEY_DOWN);
                    critical_section_exit(&wake_cs);
                }
            } else if (now - entered > WAKE_REQUEST_TIMEOUT_US) {
                WAKE_DBG("REQUESTED timeout 5s -> DONE (no resume signaling; may have already woken)");
                critical_section_enter_blocking(&wake_cs);
                enter_state(WAKE_DONE);
                critical_section_exit(&wake_cs);
            }
            return;
        }

        case WAKE_KEY_DOWN: {
            if (now - entered < WAKE_KEY_HOLD_US) return;
            if (!tud_hid_n_ready(WAKE_KBD_INSTANCE)) {
#ifdef WAKE_DEBUG
                static uint64_t last_log = 0;
                if (now - last_log > 1000000) {
                    WAKE_DBG("KEY_DOWN waiting: hid_n_ready=0 (heartbeat 1Hz)");
                    last_log = now;
                }
#endif
                return;
            }
            uint8_t up[8] = { 0 };
            const bool sent = tud_hid_n_report(WAKE_KBD_INSTANCE, 0, up, sizeof(up));
            WAKE_DBG("KEY_DOWN: sent keyup -> %d", (int)sent);
            if (sent) {
                critical_section_enter_blocking(&wake_cs);
                enter_state(WAKE_KEY_UP_SENT);
                critical_section_exit(&wake_cs);
            }
            return;
        }

        case WAKE_KEY_UP_SENT: {
            if (now - entered < WAKE_KEY_UP_SETTLE_US) return;
            key_attempts++;
            if (key_attempts < WAKE_KEY_ATTEMPTS) {
                // Retry: do NOT re-enter WAKE_REQUESTED (which gates on a
                // fresh tud_resume_cb event). We already established the
                // host woke once; just send another keydown directly. If the
                // host has dipped back into suspend, tud_hid_n_ready will be
                // false and we'll heartbeat from KEY_DOWN until it returns.
                if (!tud_hid_n_ready(WAKE_KBD_INSTANCE)) {
#ifdef WAKE_DEBUG
                    static uint64_t last_log = 0;
                    if (now - last_log > 1000000) {
                        WAKE_DBG("KEY_UP_SENT retry waiting: hid_n_ready=0 (heartbeat 1Hz)");
                        last_log = now;
                    }
#endif
                    return;
                }
                uint8_t rpt[8] = { 0, 0, WAKE_KEYCODE_F15, 0, 0, 0, 0, 0 };
                const bool sent = tud_hid_n_report(WAKE_KBD_INSTANCE, 0, rpt, sizeof(rpt));
                WAKE_DBG("KEY_UP_SENT: retrying F15 (attempt %d/%d) -> %d",
                         (int)key_attempts + 1, (int)WAKE_KEY_ATTEMPTS, (int)sent);
                if (sent) {
                    critical_section_enter_blocking(&wake_cs);
                    enter_state(WAKE_KEY_DOWN);
                    critical_section_exit(&wake_cs);
                }
            } else {
                WAKE_DBG("KEY_UP_SENT settle done -> DONE");
                critical_section_enter_blocking(&wake_cs);
                enter_state(WAKE_DONE);
                key_attempts = 0;
                critical_section_exit(&wake_cs);
            }
            return;
        }
    }
}

#endif // ENABLE_WAKE_HID
