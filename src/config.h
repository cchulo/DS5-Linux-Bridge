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
