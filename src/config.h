//
// Created by awalol on 2026/5/4.
//

#ifndef DS5_BRIDGE_CONFIG_H
#define DS5_BRIDGE_CONFIG_H

#include <cstdint>

// User-assignable nicknames for paired controllers, shown in the web UI.
// Keyed by Bluetooth address; capacity mirrors btstack's NVM_NUM_LINK_KEYS (4).
#define CONFIG_MAX_BOND_NAMES 4
#define CONFIG_BOND_ADDR_LEN  6
#define CONFIG_BOND_NAME_LEN  16 // 15 chars + NUL

// Hard ceiling on configurable strip length (bitmask storage + frame buffer
// sizing). The UI warns that anything past 8 needs external 5 V power; 32 is
// plenty for rings/squares while keeping the render loop cheap.
#define LED_STRIP_MAX_PIXELS 32

//--------------------------------------------------------------------+
// Config_body layout is APPEND-ONLY. To stay compatible with configs
// already written to flash by older firmware, obey these rules:
//   * Only ever add new fields at the END of Config_body.
//   * Never reorder, resize, remove, or repurpose an existing field.
//   * When you add a field, give it a sane default in config_valid().
// Reads are migrated by size: newer firmware keeps every field an older
// blob contained and default-initializes the newly-appended tail (see
// config_load()). The offsetof() static_asserts in config.cpp pin the
// original field layout so an accidental mid-struct insertion fails the
// build instead of silently corrupting persisted config.
//
// CONFIG_VERSION is a *layout* number, NOT a reset trigger. Bump it only on
// a genuinely incompatible change (which append-only should make rare). To
// intentionally wipe settings, call config_factory_reset() -- do not abuse
// the version for that.
//--------------------------------------------------------------------+

struct __attribute__((packed)) BondName {
    uint8_t addr[CONFIG_BOND_ADDR_LEN]; // all-zero == empty slot
    char    name[CONFIG_BOND_NAME_LEN]; // NUL-terminated; "" == unnamed
};

struct __attribute__((packed)) Config_body {
    uint8_t config_version; // Config Version
    float speaker_volume; // [-100,0]
    uint8_t inactive_time; // [5,60] min
    uint8_t disable_inactive_disconnect; // bool: 0 disable,1 enable
    uint8_t disable_pico_led; // bool
    uint8_t polling_rate_mode; // 0: 250Hz, 1: 500Hz, 2: real-time
    uint8_t audio_buffer_length; // [16,128]
    uint8_t controller_mode; // 0: DS5, 1: DSE, 2: Auto
    // Config-page address selector. 0..2 = vetted /29 presets; WEBCONFIG_SUBNET_CUSTOM
    // (3) = use webconfig_custom_ip below. See usb_net.cpp build_subnet().
    uint8_t webconfig_subnet;
    // Custom dongle IP (4 octets) used only when webconfig_subnet == CUSTOM. Must
    // be a private (RFC-1918) host address; validated in config_valid(). The host
    // DHCP lease lands in the same /29 (mirrors the preset scheme). 0.0.0.0 means
    // "unset" -> falls back to the default preset.
    uint8_t webconfig_custom_ip[4];
    BondName bond_names[CONFIG_MAX_BOND_NAMES]; // nicknames for paired controllers
    // --- append new fields BELOW this line only (see append-only note above) ---
    // RESERVED (was a designated-audio-slot selector; the tier policy now
    // gates audio to the single connected pad, so nothing reads this). Keep
    // the byte: the layout is append-only.
    uint8_t audio_slot;
    // One-time snapshot of a real controller's bind-time feature reports
    // (calibration 0x05, firmware info 0x20, pairing info 0x09), stored as
    // cached from BT (leading report-id byte included). The dongle now
    // enumerates the FULL descriptor from boot, so hid-playstation probes
    // every gamepad interface before any controller has connected; these
    // blobs answer those probes. Captured once from the first pad ever
    // paired (single flash write), served forever after.
    uint8_t feature_snapshot_valid;
    uint8_t feature_cal_len;
    uint8_t feature_cal[64];
    uint8_t feature_fw_len;
    uint8_t feature_fw[64];
    uint8_t feature_pair_len;
    uint8_t feature_pair[24];
    // Per-slot color, applied to both the controller's lightbar (at connect /
    // slot reset) and its strip LED. Always 4 entries regardless of
    // MULTI_SLOT_COUNT (like bond_names). All-zero means "unset" and is
    // defaulted to blue #0000FF in config_valid() (also covers configs
    // migrated from older firmware).
    uint8_t slot_rgb[4][3];
    // LED strip layout. led_count = how many pixels the attached strip has
    // (1..LED_STRIP_MAX_PIXELS; 0 = unset -> 8). slot_led_mask[k] bit i means
    // pixel i lights up for slot k, so any physical arrangement (line, ring,
    // square) can be mapped from the web UI. led_map_valid = 0 means the
    // masks were never saved (fresh/migrated config) and config_valid()
    // installs the classic alternating default (slot k -> pixel 2k+1);
    // 1 means the masks are authoritative, including deliberately empty ones.
    uint8_t  led_count;
    uint8_t  led_map_valid;
    uint32_t slot_led_mask[4];
    // Player-LED lock (multi-slot builds). While 2+ pads are connected, host
    // writes to the player indicators (white LEDs under the touchpad) are
    // ignored and each pad stays pinned to its slot pattern -- Steam Input
    // glitchily clears them, losing the seat identity. With a single pad the
    // host stays in control (stock behavior). Stored inverted so the
    // migrated/zero default means "lock on". Web UI toggle in Lights.
    uint8_t disable_player_led_lock; // 0 = lock active (default), 1 = host-controlled
    // Strip pixels to blink (white, ~2 Hz) while pairing mode is active,
    // chosen from the same clickable grid as the slot masks and governed by
    // the same led_map_valid flag. Default: the spacer pixels of the classic
    // alternating layout (0,2,4,6), so pairing lights the gaps between the
    // slot indicators.
    uint32_t pairing_led_mask;
    // Color of the pairing-mode blink on the strip. All-zero means "unset"
    // (fresh/migrated config) and defaults to white in config_valid().
    uint8_t pairing_rgb[3];
    // Lightbar override (multi-slot builds): when the host writes exactly
    // lightbar_filter_rgb to a pad's lightbar, repaint that slot's color
    // instead (see state_update()). Stored inverted so the migrated/zero
    // default keeps the override active. With the override disabled, every
    // host write -- including the filter color -- passes through untouched.
    uint8_t disable_lightbar_override; // 0 = override active (default), 1 = host controls
    // Color that triggers the override. Unlike pairing_rgb, all-zero is NOT
    // "unset" here: black IS the meaningful default (Steam/rumble-only
    // writers send zero-filled LED bytes).
    uint8_t lightbar_filter_rgb[3];
    // Color of the whole-strip "breathing" idle display shown while no
    // controller is connected and pairing mode is not active. All-zero means
    // "unset" (fresh/migrated config) and defaults to blue in config_valid().
    uint8_t idle_rgb[3];
};

struct __attribute__((packed)) Config {
    uint32_t magic;
    uint16_t version;   // layout version (see append-only note); NOT a reset trigger
    uint32_t crc32;     // crc32 of the first `size` bytes of body; set/verified on save
    uint16_t size;      // number of valid body bytes written == sizeof(Config_body) at save time
    Config_body body;
};

void config_default();
void config_load();
bool config_save();
// Reset every setting to defaults and persist. This is the deliberate wipe
// path (e.g. a web-UI "factory reset"); bumping CONFIG_VERSION is not.
bool config_factory_reset();
const Config_body& get_config();
void set_config(const uint8_t *new_config, const uint16_t len);
void config_valid();
void set_config(const Config_body &new_config);

// Bond-name table (nicknames keyed by Bluetooth address). These mutate the
// in-RAM config; the caller persists with config_save() when ready.

// Look up the nickname for `addr` (CONFIG_BOND_ADDR_LEN bytes). Returns the
// stored name (may be "") if a slot matches, or nullptr if none does.
const char *config_bond_name(const uint8_t *addr);

// Assign `name` to `addr`, reusing an existing slot for that address or the
// first empty slot. An empty/blank name clears the slot. Returns false if
// there was no slot free for a new address. Does NOT call config_save().
bool config_set_bond_name(const uint8_t *addr, const char *name);

// Clear the name slot for `addr` (e.g. when its bond is forgotten).
void config_clear_bond_name(const uint8_t *addr);

extern bool is_dse;

#endif //DS5_BRIDGE_CONFIG_H
