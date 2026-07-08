//
// Created by awalol on 2026/5/15.
//

#include <cstddef>
#include <cstring>

#include "state_mgr.h"
#include "bt.h"
#include "config.h"
#include "tier.h"
#include "utils.h"

namespace {
    constexpr size_t kAudioControlOffset = offsetof(SetStateData, MuteLightMode) - sizeof(uint8_t);
    constexpr size_t kMuteControlOffset = offsetof(SetStateData, RightTriggerFFB) - sizeof(uint8_t);
    constexpr size_t kMotorPowerLevelOffset = offsetof(SetStateData, HostTimestamp) + sizeof(uint32_t);
    constexpr size_t kAudioControl2Offset = kMotorPowerLevelOffset + sizeof(uint8_t);
    constexpr size_t kHapticLowPassFilterOffset = offsetof(SetStateData, LightFadeAnimation) - 2 * sizeof(uint8_t);
    constexpr size_t kPlayerIndicatorsOffset = offsetof(SetStateData, LedRed) - sizeof(uint8_t);
    constexpr size_t kLedColorOffset = offsetof(SetStateData, LedRed);
}

static constexpr uint8_t state_init_data[63] = {
    0xfd, 0xf7, 0x0, 0x0,
    0x7f, 0x64, // Headphones, Speaker
    0xff, 0x9, 0x0, 0x0F, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xa,
    0x7, 0x0, 0x0, 0x2, 0x1,
    0x00,
    0xff, 0xd7, 0x00 // RGB LED: R, G, B (Nijika Color!)✨
};

#if BT_MAX_SLOTS > 1
// Slot identity, applied at connect (report 0x32) and whenever a slot
// resets: the user-configurable slot color (config slot_rgb, web UI; default
// blue #0000FF for every slot) plus the PS5-style player-indicator pattern
// (5-LED bar: center / 2 / 3 / 4 dots). Games override both via
// AllowLedColor / AllowPlayerIndicators as usual. Single-slot builds keep
// the fork's default color above so single-controller behavior stays
// regression-identical to upstream.
static constexpr uint8_t slot_player_leds[] = {
    0x04, // player 1: -- -- ## -- --
    0x0A, // player 2: -- ## -- ## --
    0x15, // player 3: ## -- ## -- ##
    0x1B, // player 4: ## ## -- ## ##
};
#endif

volatile bool g_firmware_mic_muted = false;
volatile bool g_host_hid_manages_mute = false;
volatile uint8_t g_last_uac_mute = 0xFF;

static uint8_t state[BT_MAX_SLOTS][63]{};

void state_reset_mute() {
    g_firmware_mic_muted = false;
    g_host_hid_manages_mute = false;
    g_last_uac_mute = 0xFF;
    state[tier_audio_slot()][8] = 0; // MuteLight::Off
    state[tier_audio_slot()][9] &= ~(1 << 4); // Clear MicMute bit (bit 4 of byte 9)
}

void state_set_local_mute(bool muted) {
    uint8_t *st = state[tier_audio_slot()];
    if (muted) {
        st[8] = 1; // MuteLight::On (solid orange)
        st[9] |= (1 << 4); // MicMute bit
    } else {
        st[8] = 0; // MuteLight::Off
        st[9] &= ~(1 << 4); // Clear MicMute bit
    }
}

// Player-LED lock (see config.h): active only on multi-slot builds with 2+
// pads connected, so single-pad behavior stays stock.
static bool player_led_lock_active() {
#if BT_MAX_SLOTS > 1
    return !get_config().disable_player_led_lock && bt_connected_count() > 1;
#else
    return false;
#endif
}

void state_force_player_leds(uint8_t slot) {
#if BT_MAX_SLOTS > 1
    if (slot >= BT_MAX_SLOTS) return;
    state[slot][kPlayerIndicatorsOffset] = slot_player_leds[slot];
#else
    (void) slot;
#endif
}

void state_apply_slot_color(uint8_t slot) {
#if BT_MAX_SLOTS > 1
    if (slot >= BT_MAX_SLOTS) return;
    const uint8_t *rgb = get_config().slot_rgb[slot];
    state[slot][kLedColorOffset]     = rgb[0];
    state[slot][kLedColorOffset + 1] = rgb[1];
    state[slot][kLedColorOffset + 2] = rgb[2];
#else
    (void) slot;
#endif
}

void state_slot_reset(uint8_t slot) {
    if (slot >= BT_MAX_SLOTS) return;
    memcpy(state[slot], state_init_data, sizeof(state_init_data));
#if BT_MAX_SLOTS > 1
    state_apply_slot_color(slot);
    state[slot][kPlayerIndicatorsOffset] = slot_player_leds[slot];
#endif
}

void state_init() {
    for (uint8_t slot = 0; slot < BT_MAX_SLOTS; slot++) {
        state_slot_reset(slot);
    }
    state_reset_mute();
}

void state_get(uint8_t slot, uint8_t *data, const uint8_t size) {
    if (slot >= BT_MAX_SLOTS) return;
    if (size > sizeof(state[slot])) {
        // state[] rows are 63 bytes; copying more would OOB-read state and
        // OOB-write caller's buffer. Refuse rather than memcpy past the source.
        printf("[StateMgr] Error: state_get size %u > %u; refused\n",
               size, static_cast<unsigned>(sizeof(state[slot])));
        return;
    }
    memcpy(data, state[slot], size);
}

void state_update(uint8_t slot, const uint8_t *data, const uint8_t size) {
    if (slot >= BT_MAX_SLOTS) return;
    uint8_t *st = state[slot];

    // macOS sends a shorter SetStateData (47 bytes) than the full struct; the
    // trailing fields it omits are unused here, so accept anything >= 47 and let
    // the memcpy below over-read into zero-init padding. Rejecting short reports
    // broke rumble/LED control from macOS hosts.
    // (Ported from upstream awalol/DS5Dongle c47b7ed.)
    if (size < 47) {
        printf(
            "[StateMgr] Error: SetStateData needs at least 47 bytes, got %u\n",
            static_cast<unsigned>(size)
        );
        return;
    }

    SetStateData update{};
    memcpy(&update, data, sizeof(update));

    const auto copy_if_allowed = [&](const bool allowed, const size_t offset, const size_t length) {
        if (allowed) {
            memcpy(st + offset, data + offset, length);
        }
    };
    auto set_bit = [](uint8_t &byte, const int bit, const bool value) {
        byte = (byte & ~(1 << bit)) | (value << bit);
    };

    // Some games (e.g. Ninja Gaiden 4) send non-zero rumble values without
    // setting UseRumbleNotHaptics/EnableRumbleEmulation. Force rumble mode on
    // in that case so the emulation values below aren't gated out and dropped.
    // (Ported from upstream awalol/DS5Dongle 45b5a4f.)
    if (update.RumbleEmulationLeft > 0 || update.RumbleEmulationRight > 0) {
        update.UseRumbleNotHaptics = true;
    }
    set_bit(st[0], 0, update.EnableRumbleEmulation);
    set_bit(st[0], 1, update.UseRumbleNotHaptics);
    set_bit(st[38], 2, update.EnableImprovedRumbleEmulation);
    copy_if_allowed(
        update.UseRumbleNotHaptics ||
            update.EnableRumbleEmulation ||
            update.EnableImprovedRumbleEmulation,
        offsetof(SetStateData, RumbleEmulationRight),
        2
    );

    /*copy_if_allowed(
        update.AllowHeadphoneVolume,
        offsetof(SetStateData, VolumeHeadphones),
        sizeof(update.VolumeHeadphones)
    );*/
    /*copy_if_allowed(
        update.AllowSpeakerVolume,
        offsetof(SetStateData, VolumeSpeaker),
        sizeof(update.VolumeSpeaker)
    );*/
    /*copy_if_allowed(
        update.AllowMicVolume,
        offsetof(SetStateData, VolumeMic),
        sizeof(update.VolumeMic)
    );*/
    /*copy_if_allowed(
        update.AllowAudioControl,
        kAudioControlOffset,
        sizeof(uint8_t)
    );*/

    // Hybrid mute only applies to the designated audio slot (the one whose
    // audio path is live); other slots take the mute-light bytes verbatim.
    if (slot == tier_audio_slot()) {
        if ((update.AllowMuteLight && update.MuteLightMode == MuteLight::On) ||
            (update.AllowAudioMute && update.MicMute)) {
            g_host_hid_manages_mute = true;
        }

        if (g_host_hid_manages_mute) {
            copy_if_allowed(
                update.AllowMuteLight,
                offsetof(SetStateData, MuteLightMode),
                sizeof(update.MuteLightMode)
            );

            copy_if_allowed(
                update.AllowAudioMute,
                kMuteControlOffset,
                sizeof(uint8_t)
            );

            if (update.AllowMuteLight) {
                g_firmware_mic_muted = (update.MuteLightMode == MuteLight::On);
            } else if (update.AllowAudioMute) {
                g_firmware_mic_muted = (update.MicMute != 0);
            }
        }
    } else {
        copy_if_allowed(
            update.AllowMuteLight,
            offsetof(SetStateData, MuteLightMode),
            sizeof(update.MuteLightMode)
        );
        copy_if_allowed(
            update.AllowAudioMute,
            kMuteControlOffset,
            sizeof(uint8_t)
        );
    }

    copy_if_allowed(
        update.AllowRightTriggerFFB,
        offsetof(SetStateData, RightTriggerFFB),
        sizeof(update.RightTriggerFFB)
    );
    copy_if_allowed(
        update.AllowLeftTriggerFFB,
        offsetof(SetStateData, LeftTriggerFFB),
        sizeof(update.LeftTriggerFFB)
    );

    /*copy_if_allowed(
        update.AllowMotorPowerLevel,
        kMotorPowerLevelOffset,
        sizeof(uint8_t)
    );*/
    /*copy_if_allowed(
        update.AllowAudioControl2,
        kAudioControl2Offset,
        sizeof(uint8_t)
    );*/
    /*copy_if_allowed(
        update.AllowHapticLowPassFilter,
        kHapticLowPassFilterOffset,
        sizeof(uint8_t)
    );*/

    copy_if_allowed(
        update.AllowColorLightFadeAnimation,
        offsetof(SetStateData, LightFadeAnimation),
        sizeof(update.LightFadeAnimation)
    );
    copy_if_allowed(
        update.AllowLightBrightnessChange,
        offsetof(SetStateData, LightBrightness),
        sizeof(update.LightBrightness)
    );
    // Player-LED lock: with 2+ pads connected the white LEDs are the slot
    // number; Steam Input glitchily clears them, so ignore host writes and
    // re-pin the slot pattern (this also self-heals junk a host wrote while
    // the lock was inactive, on its next write). Single-pad behavior and the
    // toggle-off state stay stock.
    if (player_led_lock_active()) {
        state_force_player_leds(slot);
    } else {
        copy_if_allowed(
            update.AllowPlayerIndicators,
            kPlayerIndicatorsOffset,
            sizeof(uint8_t)
        );
    }
    bool led_copy = update.AllowLedColor;
#if BT_MAX_SLOTS > 1
    // IGNORE pure-black host writes: keep whatever color the lightbar has.
    // Black arrives in two ways that must both be handled: (a) Steam with
    // its LED brightness at 0% (the deliberate "stop overriding my slot
    // colors" trick -- the connect-time slot color survives), and (b) as
    // don't-care zero filler in reports from writers that only mean to
    // rumble (e.g. the kernel driver alongside Steam). An earlier version
    // SUBSTITUTED the slot color on black, and (b) then repainted the slot
    // color over a color the user had just set in SteamOS. Ignoring black
    // preserves both: deliberate colors stick, black never darkens a seat.
    // Single-slot builds keep stock upstream behavior.
    if (led_copy && update.LedRed == 0 && update.LedGreen == 0 &&
        update.LedBlue == 0) {
        led_copy = false;
    }
#endif
    copy_if_allowed(
        led_copy,
        offsetof(SetStateData, LedRed),
        sizeof(update.LedRed) * 3
    );
}
