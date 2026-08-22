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

// LED strip states with a configurable animation (indexes into led_anim[])
// and the animation modes themselves. Plain #defines so the header stays
// usable from C and C++ alike.
#define LED_STATE_IDLE     0 // whole strip; no pad connected, not pairing
#define LED_STATE_CONN     1 // a connected seat (color: slot_rgb[slot])
#define LED_STATE_EMPTY    2 // an empty seat while other pads are connected
#define LED_STATE_PAIRING  3 // pairing-mode overlay (color: pairing_rgb)
#define LED_STATE_LOWBATT  4 // <= 40% and discharging
#define LED_STATE_CRITBATT 5 // <= 20% and discharging
#define LED_STATE_COUNT    6

#define LED_ANIM_UNSET 0 // fresh/migrated config; config_valid() installs the default
#define LED_ANIM_SOLID 1
#define LED_ANIM_BLINK 2
#define LED_ANIM_PULSE 3 // raised-cosine breathe
#define LED_ANIM_OFF   4

// Network (DHCP) hostname the dongle reports to the router. User-set so two
// dongles on one LAN are distinguishable. Max 10 chars + NUL;
// validated to a DNS label (lowercase a-z, 0-9, hyphen; no leading/trailing
// hyphen) in config_valid(). See CONFIG_HOSTNAME_DEFAULT.
#define CONFIG_HOSTNAME_LEN     11
#define CONFIG_HOSTNAME_DEFAULT "ds5"

// Home-WLAN credentials for the Wake-on-LAN STA join (ENABLE_WIFI_WOL).
// Entered on the USB config page and persisted to flash. SSID is 32
// octets max (802.11) + NUL; a WPA2 PSK passphrase is 8..63 chars + NUL.
// Stored in every build so the flash layout is identical across transports
// (only the WiFi build reads them).
#define CONFIG_WIFI_SSID_LEN    33  // 32 chars + NUL
#define CONFIG_WIFI_PSK_LEN     64  // 63 chars + NUL (WPA2 passphrase max)

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
    // RESERVED (NCM-era config-page address selector + custom IP; the NCM
    // transport was removed in the WiFi migration and nothing reads these).
    // Keep the bytes: the layout is append-only. Do not reuse.
    uint8_t webconfig_subnet;       // reserved (NCM-era; unread)
    uint8_t webconfig_custom_ip[4]; // reserved (NCM-era; unread)
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
    // --- WiFi transport + Wake-on-LAN (v6; NCM->WiFi migration) ---
    // NOTE: our layout diverges from upstream kungaa here (both forks appended
    // different fields after the common ancestor) -- field ORDER below is ours
    // alone; never copy upstream's offsets.
    //
    // Network (DHCP) hostname reported to the router. Defaults to
    // CONFIG_HOSTNAME_DEFAULT; user-editable so multiple dongles on one LAN
    // are distinguishable in the client list. config_valid()
    // sanitizes it to a valid DNS label and re-defaults if empty.
    char hostname[CONFIG_HOSTNAME_LEN];
    // Home-WLAN credentials (Wake-on-LAN uplink). wifi_provisioned: 0 = no
    // creds -> WiFi idle; 1 = creds set -> STA join. wifi_ssid is NOT
    // necessarily a DNS label, so it is NUL-terminated and length-bounded but
    // not otherwise sanitized; the PSK is stored verbatim. config_valid()
    // forces termination and clears wifi_provisioned if the SSID is empty.
    // NEVER emitted back to the config page in cleartext (it only ever
    // writes them).
    uint8_t wifi_provisioned;            // bool: 0 = onboard via AP, 1 = STA creds set
    char    wifi_ssid[CONFIG_WIFI_SSID_LEN];
    char    wifi_psk[CONFIG_WIFI_PSK_LEN];
    // Wake-on-LAN targets (ENABLE_WIFI_WOL builds). wol_target_mac is the NIC
    // of the PC to wake; wol_target_mac2 an optional second device (e.g. a TV).
    // all-zero == unset (skipped). A wake fires a magic packet to every
    // configured (non-zero) target (wifi_wol_send_all()).
    uint8_t wol_target_mac[6];
    uint8_t wol_target_mac2[6];
    // --- LED strip per-state animation + colors (see ledstrip.cpp) ---
    // Animation mode per strip state, indexed by LED_STATE_*. Stored value is
    // LED_ANIM_*; 0 (LED_ANIM_UNSET, fresh/migrated config) resolves to the
    // per-state default in config_valid(): solid everywhere except critical
    // battery (blink) and idle (off).
    uint8_t led_anim[LED_STATE_COUNT];
    // Color of an empty seat while other pads are connected (the all-empty
    // strip is LED_STATE_IDLE's). Unlike the other colors, all-zero (black =
    // dark seat) IS the meaningful default, so no unset semantics.
    uint8_t empty_rgb[3];
    // Battery-warning colors. All-zero = unset -> yellow / red.
    uint8_t lowbatt_rgb[3];
    uint8_t critbatt_rgb[3];
    // Mute the controller's built-in speaker: its SetState speaker volume is
    // pinned to 0 (headphone jack, mic and haptics unaffected). 0 = speaker
    // on (default/migrated), 1 = muted. Web UI toggle in Controller.
    uint8_t mute_speaker;
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

// Store home-WLAN credentials in the in-RAM config (wifi_provisioned follows
// from a non-empty SSID). Empty ssid+psk clears provisioning. The caller
// persists with config_save() when ready.
void config_set_wifi_creds(const char *ssid, const char *psk);

extern bool is_dse;

#endif //DS5_BRIDGE_CONFIG_H
