//
// web_api.cpp -- the adapter's configuration API: JSON GET routes + form POST
// handlers, transport-agnostic. The only transport today is the USB HID
// config tunnel (hid_config.cpp); the page that drives it is web/index.html.
// The lwIP httpd / mDNS / captive-portal transport was removed (config must
// never depend on the network); the route shapes are unchanged from that era
// so existing clients keep working.
//
// The POST machinery keeps upstream's hardening: bodies are NUL-terminated
// before parsing and every value is clamped to config_valid()'s ranges.
//

#include "web_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/time.h"

#include "bt.h"
#include "tier.h"
#include "usb.h"
#ifdef ENABLE_LED_STRIP
#include "ledstrip.h"
#endif
#include "config.h"
#include "weblog.h"
#include "wifi_net.h"

#ifdef ENABLE_WIFI_WOL
// Results of the wifi provision/reset POSTs, reported via the synthetic
// /api/wifi_*_result routes.
static bool provision_ok;
static bool wifi_reset_ok;
#endif

// Result of the most recent POST /api/wol ("Wake now"), reported via the
// synthetic /api/wol_result route.
static bool last_wol_ok;

//--------------------------------------------------------------------+
// GET routes
//--------------------------------------------------------------------+

// "AABBCCDDEEFF" (12 hex, no separators). Forward declaration; defined with
// the bond helpers below.
static void addr_to_hex(const uint8_t *a, char out[13]);

static int json_config(char *out, size_t cap) {
    const Config_body &c = get_config();
    char wol_hex[13], wol_hex2[13];
    addr_to_hex(c.wol_target_mac, wol_hex);
    addr_to_hex(c.wol_target_mac2, wol_hex2);
    return snprintf(out, cap,
                    "{\"version\":\"%s\","
                    "\"inactive_time\":%u,"
                    "\"disable_inactive_disconnect\":%u,"
                    "\"disable_pico_led\":%u,"
                    "\"polling_rate_mode\":%u,"
                    "\"audio_buffer_length\":%u,"
                    "\"controller_mode\":%u,"
                    // hostname is sanitized to [a-z0-9-] in config_valid(), so
                    // it never needs JSON string escaping here. MACs are
                    // "AABBCCDDEEFF"; all-zero == unset.
                    "\"hostname\":\"%s\","
                    "\"wol_target_mac\":\"%s\","
                    "\"wol_target_mac2\":\"%s\","
                    "\"slot_rgb\":[\"%02X%02X%02X\",\"%02X%02X%02X\","
                    "\"%02X%02X%02X\",\"%02X%02X%02X\"],"
                    "\"led_count\":%u,"
                    "\"led_max\":%u,"
                    "\"led_masks\":[\"%lX\",\"%lX\",\"%lX\",\"%lX\"],"
                    "\"pairing_mask\":\"%lX\","
                    "\"pairing_rgb\":\"%02X%02X%02X\","
                    "\"idle_rgb\":\"%02X%02X%02X\","
                    // Per-state strip animation modes, one digit per
                    // LED_STATE_* in index order (values are LED_ANIM_*).
                    "\"led_anim\":\"%u%u%u%u%u%u\","
                    "\"empty_rgb\":\"%02X%02X%02X\","
                    "\"lowbatt_rgb\":\"%02X%02X%02X\","
                    "\"critbatt_rgb\":\"%02X%02X%02X\","
                    "\"disable_player_led_lock\":%u,"
                    "\"disable_lightbar_override\":%u,"
                    "\"lightbar_filter_rgb\":\"%02X%02X%02X\","
                    "\"max_slots\":%u}",
                    PICO_PROGRAM_VERSION_STRING,
                    c.inactive_time,
                    c.disable_inactive_disconnect,
                    c.disable_pico_led,
                    c.polling_rate_mode,
                    c.audio_buffer_length,
                    c.controller_mode,
                    c.hostname,
                    wol_hex,
                    wol_hex2,
                    c.slot_rgb[0][0], c.slot_rgb[0][1], c.slot_rgb[0][2],
                    c.slot_rgb[1][0], c.slot_rgb[1][1], c.slot_rgb[1][2],
                    c.slot_rgb[2][0], c.slot_rgb[2][1], c.slot_rgb[2][2],
                    c.slot_rgb[3][0], c.slot_rgb[3][1], c.slot_rgb[3][2],
                    c.led_count,
                    LED_STRIP_MAX_PIXELS,
                    (unsigned long) c.slot_led_mask[0],
                    (unsigned long) c.slot_led_mask[1],
                    (unsigned long) c.slot_led_mask[2],
                    (unsigned long) c.slot_led_mask[3],
                    (unsigned long) c.pairing_led_mask,
                    c.pairing_rgb[0], c.pairing_rgb[1], c.pairing_rgb[2],
                    c.idle_rgb[0], c.idle_rgb[1], c.idle_rgb[2],
                    c.led_anim[0], c.led_anim[1], c.led_anim[2],
                    c.led_anim[3], c.led_anim[4], c.led_anim[5],
                    c.empty_rgb[0], c.empty_rgb[1], c.empty_rgb[2],
                    c.lowbatt_rgb[0], c.lowbatt_rgb[1], c.lowbatt_rgb[2],
                    c.critbatt_rgb[0], c.critbatt_rgb[1], c.critbatt_rgb[2],
                    c.disable_player_led_lock,
                    c.disable_lightbar_override,
                    c.lightbar_filter_rgb[0], c.lightbar_filter_rgb[1],
                    c.lightbar_filter_rgb[2],
                    BT_MAX_SLOTS);
}

//--------------------------------------------------------------------+
// Paired-controller (bond) management API: GET /api/bonds (list) and
// POST /api/bonds (forget / forgetall / rename). Bonds live in BTstack's
// flash; nicknames live in our Config flash, reconciled here by address.
//--------------------------------------------------------------------+

// "AABBCCDDEEFF" (12 hex, no separators -- compact and trivial to parse).
static void addr_to_hex(const uint8_t *a, char out[13]) {
    static const char h[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        out[i * 2]     = h[(a[i] >> 4) & 0xf];
        out[i * 2 + 1] = h[a[i] & 0xf];
    }
    out[12] = '\0';
}

// Parse exactly 12 hex chars into a[6]. Returns true on success.
static bool hex_to_addr(const char *s, uint8_t a[6]) {
    if (!s) return false;
    uint8_t bytes[6];
    for (int i = 0; i < 6; i++) {
        char c = s[i * 2], d = s[i * 2 + 1];
        if (!c || !d) return false;
        auto nib = [](char x) -> int {
            if (x >= '0' && x <= '9') return x - '0';
            if (x >= 'a' && x <= 'f') return x - 'a' + 10;
            if (x >= 'A' && x <= 'F') return x - 'A' + 10;
            return -1;
        };
        int hi = nib(c), lo = nib(d);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = (uint8_t) ((hi << 4) | lo);
    }
    if (s[12] != '\0') return false; // trailing junk
    memcpy(a, bytes, 6);
    return true;
}

// Append a JSON string literal (with quotes) for `s`, escaping " and \.
// Returns chars written (0 if it wouldn't fit).
static int json_str(char *out, size_t cap, const char *s) {
    size_t i = 0;
    if (cap < 3) return 0;
    out[i++] = '"';
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\\') {
            if (i + 2 >= cap - 1) break;
            out[i++] = '\\';
            out[i++] = c;
        } else if ((unsigned char) c < 0x20) {
            continue; // drop control chars
        } else {
            if (i + 1 >= cap - 1) break;
            out[i++] = c;
        }
    }
    out[i++] = '"';
    out[i] = '\0';
    return (int) i;
}

static int json_bonds(char *out, size_t cap) {
    uint8_t list[CONFIG_MAX_BOND_NAMES][BT_ADDR_LEN];
    const int n = bt_bond_list(list, CONFIG_MAX_BOND_NAMES);

    uint8_t conn[BT_ADDR_LEN];
    const bool have_conn = bt_connected_addr(conn);
    char conn_hex[13] = "";
    if (have_conn) addr_to_hex(conn, conn_hex);

    bool dbg_tlv = false, dbg_iter = false;
    int dbg_keys = 0;
    bt_bond_diag(&dbg_tlv, &dbg_iter, &dbg_keys);
    int w = snprintf(out, cap,
                     "{\"ready\":%s,"
                     "\"dbg\":{\"tlv\":%s,\"iter\":%s,\"keys\":%d},"
                     "\"connected\":\"%s\",\"max\":%d,\"bonds\":[",
                     bt_stack_ready() ? "true" : "false",
                     dbg_tlv ? "true" : "false", dbg_iter ? "true" : "false",
                     dbg_keys,
                     conn_hex, CONFIG_MAX_BOND_NAMES);
    for (int i = 0; i < n && w < (int) cap; i++) {
        char hex[13];
        addr_to_hex(list[i], hex);
        const char *nm = config_bond_name(list[i]);
        if (!nm) nm = "";
        // Which controller slot (seat) this bond currently occupies, -1 if
        // not connected. The UI shows it as a colored "Slot N" badge.
        int bslot = -1;
        for (int k = 0; k < BT_MAX_SLOTS; k++) {
            BtStatus st;
            bt_get_status((uint8_t) k, &st);
            if (st.connected && memcmp(st.addr, list[i], BT_ADDR_LEN) == 0) {
                bslot = k;
                break;
            }
        }
        w += snprintf(out + w, cap - w, "%s{\"addr\":\"%s\",\"slot\":%d,\"name\":",
                      i ? "," : "", hex, bslot);
        if (w < (int) cap) w += json_str(out + w, cap - w, nm);
        if (w < (int) cap) w += snprintf(out + w, cap - w, "}");
    }
    if (w < (int) cap) w += snprintf(out + w, cap - w, "]}");
    return w;
}

// GET /api/status -- live controller health for the web UI and the Decky
// plugin (read-only; no config side effects). Cheap snapshot of state the
// firmware already tracks.
static int json_status(char *out, size_t cap) {
    BtStatus s;
    // Status of the USB-exposed slot (the Decky/web contract's single
    // controller view; a slots[] array will be added alongside it).
    bt_get_status(BT_USB_SLOT, &s);
    return snprintf(out, cap,
                    "{\"connected\":%s,"
                    "\"model\":\"%s\","
                    "\"battery_valid\":%s,"
                    "\"battery_pct\":%u,"
                    "\"charging\":%s}",
                    s.connected ? "true" : "false",
                    s.is_dse ? "DSE" : "DS5",
                    s.battery_valid ? "true" : "false",
                    s.battery_pct,
                    s.charging ? "true" : "false");
}

// GET /api/slots -- per-slot status for multi-controller operation, plus the
// tier state (see tier.h). Additive endpoint: /api/status keeps its original
// single-controller shape for existing clients (Decky plugin).
static int json_slots(char *out, size_t cap) {
#ifdef ENABLE_LED_STRIP
    const char *led_flag = "true";
#else
    const char *led_flag = "false";
#endif
    int w = snprintf(out, cap,
                     "{\"max\":%u,\"connected\":%d,\"usb_exposed\":%u,\"audio_slot\":%u,\"audio_allowed\":%s,\"led\":%s,\"wifi\":\"%s\",\"slots\":[",
                     BT_MAX_SLOTS, bt_connected_count(), usb_exposed_slot_count(),
                     tier_audio_slot(), tier_audio_allowed() ? "true" : "false", led_flag,
                     wifi_net_state());
    for (int i = 0; i < BT_MAX_SLOTS && w < (int) cap; i++) {
        BtStatus s;
        bt_get_status((uint8_t) i, &s);
        char hex[13] = "";
        if (s.connected) addr_to_hex(s.addr, hex);
        const char *nm = s.connected ? config_bond_name(s.addr) : nullptr;
        w += snprintf(out + w, cap - w,
                      "%s{\"slot\":%d,\"connected\":%s,\"model\":\"%s\","
                      "\"battery_valid\":%s,\"battery_pct\":%u,\"charging\":%s,"
                      "\"addr\":\"%s\",\"name\":",
                      i ? "," : "",
                      i,
                      s.connected ? "true" : "false",
                      s.is_dse ? "DSE" : "DS5",
                      s.battery_valid ? "true" : "false",
                      s.battery_pct,
                      s.charging ? "true" : "false",
                      hex);
        if (w < (int) cap) w += json_str(out + w, cap - w, nm ? nm : "");
        if (w < (int) cap) w += snprintf(out + w, cap - w, "}");
    }
    if (w < (int) cap) w += snprintf(out + w, cap - w, "]}");
    return w;
}

//--------------------------------------------------------------------+
// Transport-agnostic route layer
//
// Every GET route renders its body into a caller-supplied buffer and reports
// an HTTP-style status; every POST dispatches and returns the synthetic
// result route whose GET carries the outcome (see web_api_post below).
// The USB HID config tunnel (hid_config.cpp) is a thin adapter over these
// two functions; any future transport should be too, so JSON shapes, form
// parsing and validation stay shared byte-for-byte.
//--------------------------------------------------------------------+

static int put_text(char *out, size_t cap, const char *s) {
    const size_t n = strlen(s);
    const size_t m = n < cap ? n : cap;
    memcpy(out, s, m);
    return (int) m;
}

int web_api_get(const char *name, char *out, size_t cap, int *status) {
    *status = 200;
    int len = -1;
    if (strcmp(name, "/api/config") == 0) {
        len = json_config(out, cap);
    } else if (strcmp(name, "/api/bonds") == 0) {
        len = json_bonds(out, cap);
    } else if (strcmp(name, "/api/status") == 0) {
        len = json_status(out, cap);
    } else if (strcmp(name, "/api/slots") == 0) {
        len = json_slots(out, cap);
    } else if (strcmp(name, "/api/log") == 0) {
        // Firmware log (RAM ring of all printf diagnostics; see weblog.h).
        // Plain text so it reads directly in a browser tab.
        len = weblog_snapshot(out, (int) cap);
    } else if (strcmp(name, "/api/resolve_mac") == 0) {
        // ARP-resolve poll, kicked off by POST /api/resolve_mac. The POST only
        // starts the lookup; the client polls this GET until "pending":false,
        // then either {"ok":true,"mac":"AABBCCDDEEFF"} or {"ok":false}.
        uint8_t mac[6];
        const int r = wifi_resolve_mac_poll_result(mac);
        if (r == 0) {
            len = snprintf(out, cap, "{\"pending\":true}");
        } else if (r > 0) {
            char hex[13];
            addr_to_hex(mac, hex);
            len = snprintf(out, cap, "{\"pending\":false,\"ok\":true,\"mac\":\"%s\"}", hex);
        } else {
            len = snprintf(out, cap, "{\"pending\":false,\"ok\":false}");
        }
    } else if (strcmp(name, "/api/wol_result") == 0) {
        // Synthetic reply for POST /api/wol ("Wake now").
        len = snprintf(out, cap, "{\"ok\":%s}", last_wol_ok ? "true" : "false");
#ifdef ENABLE_WIFI_WOL
    } else if (strcmp(name, "/api/wifi_provision_result") == 0) {
        // Synthetic reply for the provision POST: whether the creds were
        // accepted; the device reboots to join shortly after.
        len = snprintf(out, cap, "{\"ok\":%s}", provision_ok ? "true" : "false");
    } else if (strcmp(name, "/api/wifi_reset_result") == 0) {
        // Synthetic reply for POST /api/wifi_reset: if ok, the stored WiFi
        // credentials are cleared and the device reboots.
        len = snprintf(out, cap, "{\"ok\":%s}", wifi_reset_ok ? "true" : "false");
#endif
    } else if (strcmp(name, "/api/action-failed") == 0) {
        // POST /api/slots or /api/led land here when the action was refused
        // (bad indices, slot mid-setup, LED debug on a build without the
        // strip). Non-2xx so the client's `ok` check is false.
        *status = 409;
        len = put_text(out, cap, "action failed");
    } else if (strcmp(name, "/api/pair-rejected") == 0) {
        // POST /api/bonds action=pair lands here when pairing was refused (all
        // bond seats occupied, or all slots connected on a multi-slot build).
        *status = 409;
        len = put_text(out, cap, "pairing rejected: no free controller slot or bond seat");
    } else if (strcmp(name, "/api/reboot-ok") == 0) {
        // POST /api/reboot lands here once the BOOTSEL reboot is armed; the
        // device drops off the bus ~0.5 s later and re-enumerates as the ROM
        // UF2 mass-storage bootloader.
        len = put_text(out, cap, "rebooting to BOOTSEL");
    } else if (strcmp(name, "/api/save-failed") == 0) {
        // POST /api/config lands here when config_save() failed to reach
        // flash. Non-2xx so the client shows an error instead of "Saved".
        *status = 500;
        len = put_text(out, cap, "config save failed: flash not written");
    } else if (strcmp(name, "/404.html") == 0) {
        *status = 404;
        len = put_text(out, cap, "not found");
    }
    if (len > (int) cap) len = (int) cap; // snprintf reports the untruncated size
    return len;
}

//--------------------------------------------------------------------+
// POST /api/config -- form fields:
//   speaker_volume, inactive_time, disable_inactive_disconnect,
//   disable_pico_led, polling_rate_mode, audio_buffer_length, controller_mode
// Each value clamped to the same ranges config_valid() enforces, so a bad
// POST can never persist an out-of-range setting.
//--------------------------------------------------------------------+

enum post_target_t { POST_CONFIG, POST_BONDS, POST_SLOTS, POST_LED, POST_REBOOT,
                     POST_WIFI_PROVISION, POST_WIFI_RESET, POST_WOL,
                     POST_RESOLVE_MAC };
static bool last_save_ok = true; // result of the most recent config_save()
static bool last_action_ok = true; // result of the most recent slots/led action
static bool last_pair_rejected = false; // pair action refused (no free bond/slot)
static bool bootsel_pending = false;   // armed by POST /api/reboot
static absolute_time_t bootsel_at;     // when to actually drop into BOOTSEL

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// In-place URL-decode (%XX and '+' -> space) of a form field value.
static void url_decode(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            auto nib = [](char x) -> int {
                if (x >= '0' && x <= '9') return x - '0';
                if (x >= 'a' && x <= 'f') return x - 'a' + 10;
                if (x >= 'A' && x <= 'F') return x - 'A' + 10;
                return -1;
            };
            int hi = nib(r[1]), lo = nib(r[2]);
            if (hi >= 0 && lo >= 0) { *w++ = (char) ((hi << 4) | lo); r += 2; }
            else *w++ = *r;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static void apply_post(char *body) {
    // Deliberate settings wipe. Resets Config to defaults and persists; leaves
    // BT bonds untouched (forgetting controllers is the /api/bonds "forgetall"
    // action). This is the sanctioned reset path -- CONFIG_VERSION is layout
    // metadata, not a reset knob.
    if (strstr(body, "factory_reset=1")) {
        watchdog_update();
        last_save_ok = config_factory_reset();
        // Push the freshly-defaulted slot colors / player LEDs to connected
        // pads too -- resetting only the stored settings left the pads'
        // lightbars showing the old colors until they reconnected.
        bt_slot_colors_refresh();
        bt_player_led_lock_refresh();
        printf("[NET] factory reset via web UI: %s\n", last_save_ok ? "OK" : "FAILED");
        return;
    }

    Config_body c = get_config(); // start from current, overwrite parsed fields

    for (char *tok = strtok(body, "&"); tok; tok = strtok(nullptr, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        const int val = atoi(eq);
        if (strcmp(tok, "inactive_time") == 0) {
            c.inactive_time = (uint8_t) clampi(val, 5, 60);
        } else if (strcmp(tok, "disable_inactive_disconnect") == 0) {
            c.disable_inactive_disconnect = val ? 1 : 0;
        } else if (strcmp(tok, "disable_pico_led") == 0) {
            c.disable_pico_led = val ? 1 : 0;
        } else if (strcmp(tok, "polling_rate_mode") == 0) {
            c.polling_rate_mode = (uint8_t) clampi(val, 0, 2);
        } else if (strcmp(tok, "audio_buffer_length") == 0) {
            c.audio_buffer_length = (uint8_t) clampi(val, 16, 128);
        } else if (strcmp(tok, "controller_mode") == 0) {
            c.controller_mode = (uint8_t) clampi(val, 0, 2);
        } else if (strcmp(tok, "wol_target_mac") == 0) {
            // 12 hex chars, no separators (the page strips ':'/'-' client-side).
            // Reject anything malformed so a bad POST can't store a junk MAC; an
            // all-zero MAC is the canonical "unset" and is allowed (clears it).
            uint8_t mac[6];
            if (hex_to_addr(eq, mac)) memcpy(c.wol_target_mac, mac, 6);
        } else if (strcmp(tok, "wol_target_mac2") == 0) {
            // Second WOL target (e.g. a TV). Same parse/unset convention as #1.
            uint8_t mac[6];
            if (hex_to_addr(eq, mac)) memcpy(c.wol_target_mac2, mac, 6);
        } else if (strcmp(tok, "hostname") == 0) {
            // mDNS / netif hostname. url_decode then copy raw; set_config() ->
            // config_valid() -> sanitize_hostname() folds case and strips any
            // non-DNS-label chars (and re-defaults if empty), so we don't
            // filter here. Takes effect on the next boot (the netif/mDNS name
            // is set at init).
            url_decode(eq);
            strncpy(c.hostname, eq, CONFIG_HOSTNAME_LEN - 1);
            c.hostname[CONFIG_HOSTNAME_LEN - 1] = '\0';
        } else if (strcmp(tok, "disable_player_led_lock") == 0) {
            c.disable_player_led_lock = val ? 1 : 0;
        } else if (strcmp(tok, "disable_lightbar_override") == 0) {
            c.disable_lightbar_override = val ? 1 : 0;
        } else if (strcmp(tok, "lightbar_filter_rgb") == 0) {
            // "RRGGBB" (the page strips the '#').
            if (strlen(eq) == 6) {
                char *end = nullptr;
                const uint32_t v = (uint32_t) strtoul(eq, &end, 16);
                if (end == eq + 6) {
                    c.lightbar_filter_rgb[0] = (uint8_t) (v >> 16);
                    c.lightbar_filter_rgb[1] = (uint8_t) (v >> 8);
                    c.lightbar_filter_rgb[2] = (uint8_t) v;
                }
            }
        } else if (strcmp(tok, "led_count") == 0) {
            c.led_count = (uint8_t) clampi(val, 1, LED_STRIP_MAX_PIXELS);
        } else if (strncmp(tok, "led_mask", 8) == 0 &&
                   tok[8] >= '0' && tok[8] <= '3' && tok[9] == '\0') {
            // led_mask0..led_mask3 = pixel bitmask in hex.
            char *end = nullptr;
            const uint32_t v = (uint32_t) strtoul(eq, &end, 16);
            if (end && end != eq && *end == '\0') {
                c.slot_led_mask[tok[8] - '0'] = v;
                c.led_map_valid = 1; // masks are now authoritative
            }
        } else if (strcmp(tok, "pairing_mask") == 0) {
            char *end = nullptr;
            const uint32_t v = (uint32_t) strtoul(eq, &end, 16);
            if (end && end != eq && *end == '\0') {
                c.pairing_led_mask = v;
                c.led_map_valid = 1;
            }
        } else if (strcmp(tok, "pairing_rgb") == 0) {
            // "RRGGBB" (the page strips the '#').
            if (strlen(eq) == 6) {
                char *end = nullptr;
                const uint32_t v = (uint32_t) strtoul(eq, &end, 16);
                if (end == eq + 6) {
                    c.pairing_rgb[0] = (uint8_t) (v >> 16);
                    c.pairing_rgb[1] = (uint8_t) (v >> 8);
                    c.pairing_rgb[2] = (uint8_t) v;
                }
            }
        } else if (strcmp(tok, "idle_rgb") == 0) {
            if (strlen(eq) == 6) {
                char *end = nullptr;
                const uint32_t v = (uint32_t) strtoul(eq, &end, 16);
                if (end == eq + 6) {
                    c.idle_rgb[0] = (uint8_t) (v >> 16);
                    c.idle_rgb[1] = (uint8_t) (v >> 8);
                    c.idle_rgb[2] = (uint8_t) v;
                }
            }
        } else if (strcmp(tok, "empty_rgb") == 0 ||
                   strcmp(tok, "lowbatt_rgb") == 0 ||
                   strcmp(tok, "critbatt_rgb") == 0) {
            if (strlen(eq) == 6) {
                char *end = nullptr;
                const uint32_t v = (uint32_t) strtoul(eq, &end, 16);
                if (end == eq + 6) {
                    uint8_t *rgb = tok[0] == 'e'   ? c.empty_rgb
                                   : tok[0] == 'l' ? c.lowbatt_rgb
                                                   : c.critbatt_rgb;
                    rgb[0] = (uint8_t) (v >> 16);
                    rgb[1] = (uint8_t) (v >> 8);
                    rgb[2] = (uint8_t) v;
                }
            }
        } else if (strcmp(tok, "led_anim") == 0) {
            // One digit per LED_STATE_* in index order (values LED_ANIM_*;
            // config_valid() re-defaults anything out of range).
            if (strlen(eq) == LED_STATE_COUNT) {
                for (int i = 0; i < LED_STATE_COUNT; i++) {
                    if (eq[i] >= '0' && eq[i] <= '9') {
                        c.led_anim[i] = (uint8_t) (eq[i] - '0');
                    }
                }
            }
        } else if (strncmp(tok, "slot_rgb", 8) == 0 &&
                   tok[8] >= '0' && tok[8] <= '3' && tok[9] == '\0') {
            // slot_rgb0..slot_rgb3 = "RRGGBB" (the page strips the '#').
            if (strlen(eq) == 6) {
                char *end = nullptr;
                const uint32_t v = (uint32_t) strtoul(eq, &end, 16);
                if (end == eq + 6) {
                    const int idx = tok[8] - '0';
                    c.slot_rgb[idx][0] = (uint8_t) (v >> 16);
                    c.slot_rgb[idx][1] = (uint8_t) (v >> 8);
                    c.slot_rgb[idx][2] = (uint8_t) v;
                }
            }
        }
        // webconfig_subnet / webconfig_custom_ip are no longer accepted: the
        // NCM transport is gone and the fields are reserved bytes (config.h).
    }

    const bool lock_was_disabled = get_config().disable_player_led_lock;
    set_config(c); // validates + stores in RAM
    // Slot colors take effect immediately: re-apply to every connected pad's
    // lightbar (the strip reads the config directly each frame).
    bt_slot_colors_refresh();
    // Enabling the player-LED lock takes effect immediately too: re-pin every
    // connected pad's slot pattern (no-op unless 2+ pads are connected).
    if (lock_was_disabled && !get_config().disable_player_led_lock) {
        bt_player_led_lock_refresh();
    }
    // The sector erase blocks with interrupts off; feed the watchdog first.
    watchdog_update();
    // config_save() can fail (core1 won't park -> flash write skipped). If it
    // does, RAM holds the new values but flash does not, so the change would
    // silently vanish on the next boot. Record the result so the response can
    // tell the user instead of falsely reporting success.
    last_save_ok = config_save();
    printf("[NET] config save via web UI: %s\n", last_save_ok ? "OK" : "FAILED");
}

// POST /api/bonds -- form fields:
//   action = forget | forgetall | rename
//   addr   = 12 hex chars (forget, rename)
//   name   = nickname (rename; URL-encoded, clamped to CONFIG_BOND_NAME_LEN-1)
// Forgetting also clears any stored nickname so the name table can't outlive
// the bond. Everything is reconciled against the live BTstack bond list.
static void apply_bonds_post(char *body) {
    char action[16] = "";
    char addr_hex[16] = "";
    char name[CONFIG_BOND_NAME_LEN] = "";
    last_save_ok = true; // actions that don't persist (e.g. pair) leave this true
    last_pair_rejected = false;

    for (char *tok = strtok(body, "&"); tok; tok = strtok(nullptr, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (strcmp(tok, "action") == 0) {
            strncpy(action, eq, sizeof(action) - 1);
        } else if (strcmp(tok, "addr") == 0) {
            strncpy(addr_hex, eq, sizeof(addr_hex) - 1);
        } else if (strcmp(tok, "name") == 0) {
            url_decode(eq);
            strncpy(name, eq, sizeof(name) - 1);
        }
    }

    if (strcmp(action, "pair") == 0) {
        // Open a fresh inquiry to add another controller. Normally the dongle
        // only inquires when nothing is bonded; this is the deliberate opt-in.
        // Rejected (false) when every bond seat is occupied, or on a
        // multi-slot build when all slots are connected.
        last_pair_rejected = !bt_start_pairing();
        printf("[NET] start pairing (open inquiry) via web UI: %s\n",
               last_pair_rejected ? "REJECTED" : "OK");
        return;
    }

    if (strcmp(action, "forgetall") == 0) {
        bt_bond_forget_all();
        // Wipe all nicknames too (no bonds left to name).
        Config_body c = get_config();
        memset(c.bond_names, 0, sizeof(c.bond_names));
        set_config(c);
        watchdog_update();
        last_save_ok = config_save();
        printf("[NET] forget all bonds via web UI: %s\n", last_save_ok ? "OK" : "FAILED");
        return;
    }

    uint8_t addr[6];
    if (!hex_to_addr(addr_hex, addr)) {
        printf("[NET] bonds POST: bad addr '%s'\n", addr_hex);
        return;
    }

    if (strcmp(action, "forget") == 0) {
        bt_bond_forget(addr);
        config_clear_bond_name(addr);
        watchdog_update();
        last_save_ok = config_save();
        printf("[NET] forget bond via web UI: %s\n", last_save_ok ? "OK" : "FAILED");
    } else if (strcmp(action, "rename") == 0) {
        config_set_bond_name(addr, name);
        watchdog_update();
        last_save_ok = config_save();
        printf("[NET] rename bond via web UI: %s\n", last_save_ok ? "OK" : "FAILED");
    }
}

// POST /api/slots -- form fields: action=swap&a=<slot>&b=<slot>. Moves a
// controller between seats (bt_slot_swap); moving to an empty seat is fine.
static void apply_slots_post(char *body) {
    char action[16] = "";
    int a = -1, b = -1;
    for (char *tok = strtok(body, "&"); tok; tok = strtok(nullptr, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (strcmp(tok, "action") == 0) strncpy(action, eq, sizeof(action) - 1);
        else if (strcmp(tok, "a") == 0) a = atoi(eq);
        else if (strcmp(tok, "b") == 0) b = atoi(eq);
    }
    last_action_ok = false;
    if (strcmp(action, "swap") == 0 && a >= 0 && b >= 0) {
        last_action_ok = bt_slot_swap((uint8_t) a, (uint8_t) b);
    }
    printf("[NET] slots %s a=%d b=%d via web UI: %s\n", action, a, b,
           last_action_ok ? "OK" : "REJECTED");
}

// POST /api/led -- form fields: action=set|chase|sim|pairsim|idlesim|clear,
// rgb=RRGGBB, pixel=<n>|all, for sim: slot=<n>|all,
// level=normal|connected|low|critical (per-slot state preview overlaid on
// live status), and for pairsim/idlesim: state=on|off (force the pairing
// blink / idle breathing without touching the radio). Drives the LED debug
// override (auto-reverts after 60 s).
#ifdef ENABLE_LED_STRIP
static void apply_led_post(char *body) {
    char action[8] = "";
    char rgbhex[8] = "";
    char pixel[8] = "all";
    char slot[8] = "all";
    char level[12] = "";
    char state[8] = "";
    for (char *tok = strtok(body, "&"); tok; tok = strtok(nullptr, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (strcmp(tok, "action") == 0) strncpy(action, eq, sizeof(action) - 1);
        else if (strcmp(tok, "rgb") == 0) strncpy(rgbhex, eq, sizeof(rgbhex) - 1);
        else if (strcmp(tok, "pixel") == 0) strncpy(pixel, eq, sizeof(pixel) - 1);
        else if (strcmp(tok, "slot") == 0) strncpy(slot, eq, sizeof(slot) - 1);
        else if (strcmp(tok, "level") == 0) strncpy(level, eq, sizeof(level) - 1);
        else if (strcmp(tok, "state") == 0) strncpy(state, eq, sizeof(state) - 1);
    }
    uint8_t r = 0, g = 0, b = 0;
    if (strlen(rgbhex) == 6) {
        const uint32_t v = (uint32_t) strtoul(rgbhex, nullptr, 16);
        r = (uint8_t) (v >> 16);
        g = (uint8_t) (v >> 8);
        b = (uint8_t) v;
    }
    last_action_ok = true;
    if (strcmp(action, "set") == 0) {
        ledstrip_debug_set_pixel(strcmp(pixel, "all") == 0 ? -1 : atoi(pixel), r, g, b);
    } else if (strcmp(action, "chase") == 0) {
        ledstrip_debug_chase(r, g, b);
    } else if (strcmp(action, "sim") == 0) {
        int lvl = -1;
        if (strcmp(level, "normal") == 0) lvl = 0;
        else if (strcmp(level, "low") == 0) lvl = 1;
        else if (strcmp(level, "critical") == 0) lvl = 2;
        else if (strcmp(level, "connected") == 0) lvl = 3;
        if (lvl < 0) {
            last_action_ok = false;
        } else {
            ledstrip_debug_slot_sim(strcmp(slot, "all") == 0 ? -1 : atoi(slot), lvl);
        }
    } else if (strcmp(action, "pairsim") == 0) {
        if (strcmp(state, "on") == 0) ledstrip_debug_pairing_sim(true);
        else if (strcmp(state, "off") == 0) ledstrip_debug_pairing_sim(false);
        else last_action_ok = false;
    } else if (strcmp(action, "idlesim") == 0) {
        if (strcmp(state, "on") == 0) ledstrip_debug_idle_sim(true);
        else if (strcmp(state, "off") == 0) ledstrip_debug_idle_sim(false);
        else last_action_ok = false;
    } else if (strcmp(action, "clear") == 0) {
        ledstrip_debug_clear();
    } else {
        last_action_ok = false;
    }
    printf("[NET] led %s rgb=%s pixel=%s via web UI\n", action, rgbhex, pixel);
}
#endif

// POST /api/reboot -- form field: action=bootsel. Arms a deferred reboot into
// the ROM UF2 bootloader so new firmware can be flashed without reaching the
// BOOTSEL button (the pico is screwed into the case). Deferred rather than
// immediate because rebooting here would kill the USB link before the HTTP
// response goes out and the page would show a spinner instead of "flash mode".
// The reboot itself happens in web_api_task().
static void apply_reboot_post(char *body) {
    char action[16] = "";
    for (char *tok = strtok(body, "&"); tok; tok = strtok(nullptr, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (strcmp(tok, "action") == 0) strncpy(action, eq, sizeof(action) - 1);
    }
    last_action_ok = strcmp(action, "bootsel") == 0;
    if (last_action_ok) {
        bootsel_pending = true;
        bootsel_at = make_timeout_time_ms(500);
    }
    printf("[NET] reboot '%s' via web UI: %s\n", action,
           last_action_ok ? "ARMED (BOOTSEL in 500ms)" : "REJECTED");
}

// POST /api/wifi_provision -- ssid=...&psk=... from the config page (over USB).
// Saves the home-WLAN credentials and schedules a reboot into STA mode
// (wifi_provision_apply owns the persist + deferred reset). The JSON reply is
// sent before the reboot fires so the phone sees success.
#ifdef ENABLE_WIFI_WOL
static void apply_wifi_provision_post(char *body) {
    char ssid[CONFIG_WIFI_SSID_LEN] = "";
    char psk[CONFIG_WIFI_PSK_LEN] = "";
    bool too_long = false;
    for (char *tok = strtok(body, "&"); tok; tok = strtok(nullptr, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (strcmp(tok, "ssid") == 0) {
            url_decode(eq);
            if (strlen(eq) >= sizeof(ssid)) too_long = true;
            else strcpy(ssid, eq);
        } else if (strcmp(tok, "psk") == 0) {
            url_decode(eq);
            if (strlen(eq) >= sizeof(psk)) too_long = true;
            else strcpy(psk, eq);
        }
    }
    provision_ok = !too_long && wifi_provision_apply(ssid, psk);
    printf("[NET] wifi provision %s\n", provision_ok ? "accepted" : "rejected");
}

// POST /api/wifi_reset -- from the normal config page after the device is on
// the home WLAN. Clears stored WiFi credentials and schedules a reboot; the
// next boot is unprovisioned, so WiFi stays idle.
static void apply_wifi_reset_post(void) {
    wifi_reset_ok = wifi_reset_provisioning_apply();
    printf("[NET] wifi reset %s\n", wifi_reset_ok ? "accepted" : "rejected");
}
#endif

// POST /api/wol -- action=wake[&mac=AABBCCDDEEFF]. With an explicit mac, wakes
// exactly that target. With no mac (the page's "Wake now" button), fires EVERY
// stored target (wol_target_mac + wol_target_mac2).
static void apply_wol_post(char *body) {
    char action[16] = "";
    uint8_t mac[6];
    bool have_mac = false;
    for (char *tok = strtok(body, "&"); tok; tok = strtok(nullptr, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (strcmp(tok, "action") == 0) strncpy(action, eq, sizeof(action) - 1);
        else if (strcmp(tok, "mac") == 0 && hex_to_addr(eq, mac)) have_mac = true;
    }
    last_wol_ok = false;
    if (strcmp(action, "wake") != 0) return;
    last_wol_ok = have_mac ? wifi_wol_send(mac) : wifi_wol_send_all();
    printf("[NET] WOL via web UI: %s\n", last_wol_ok ? "sent" : "send failed");
}

// POST /api/resolve_mac -- ip=A.B.C.D. Kicks off an ARP lookup for that
// address so the UI can auto-fill the WOL target MAC instead of the user
// hunting it down by hand. Non-blocking: this only STARTS the lookup (the
// handler must never pump the lwIP stack itself); the page polls GET
// /api/resolve_mac for the result once
// wifi_net_task() has driven the ARP query.
static void apply_resolve_mac_post(char *body) {
    unsigned a = 0, b = 0, cc = 0, d = 0;
    char *eq = strchr(body, '=');
    const bool parsed = eq &&
             sscanf(eq + 1, "%u.%u.%u.%u", &a, &b, &cc, &d) == 4 &&
             a <= 255 && b <= 255 && cc <= 255 && d <= 255;
    if (!parsed) {
        printf("[NET] resolve_mac POST: bad ip\n");
        return;
    }
    const uint8_t ip[4] = {(uint8_t) a, (uint8_t) b, (uint8_t) cc, (uint8_t) d};
    wifi_resolve_mac_start(ip);
    printf("[NET] resolving %u.%u.%u.%u...\n", a, b, cc, d);
}

// Resolve a POST URI to its handler; false if unknown.
static bool post_target_for(const char *uri, post_target_t *target) {
    if (strcmp(uri, "/api/config") == 0) *target = POST_CONFIG;
    else if (strcmp(uri, "/api/bonds") == 0) *target = POST_BONDS;
    else if (strcmp(uri, "/api/slots") == 0) *target = POST_SLOTS;
#ifdef ENABLE_LED_STRIP
    else if (strcmp(uri, "/api/led") == 0) *target = POST_LED;
#endif
    else if (strcmp(uri, "/api/reboot") == 0) *target = POST_REBOOT;
    else if (strcmp(uri, "/api/wol") == 0) *target = POST_WOL;
    else if (strcmp(uri, "/api/resolve_mac") == 0) *target = POST_RESOLVE_MAC;
#ifdef ENABLE_WIFI_WOL
    else if (strcmp(uri, "/api/wifi_provision") == 0) *target = POST_WIFI_PROVISION;
    else if (strcmp(uri, "/api/wifi_reset") == 0) *target = POST_WIFI_RESET;
#endif
    else return false;
    return true;
}

// Apply a POST body (form-encoded, mutated in place by the strtok parsers)
// and return the synthetic result route whose GET reports the outcome.
static const char *dispatch_post(post_target_t target, char *body) {
    switch (target) {
        case POST_BONDS:
            apply_bonds_post(body);
            return last_pair_rejected ? "/api/pair-rejected"
                     : (last_save_ok ? "/api/bonds" : "/api/save-failed");
        case POST_SLOTS:
            apply_slots_post(body);
            return last_action_ok ? "/api/slots" : "/api/action-failed";

#ifdef ENABLE_LED_STRIP
        case POST_LED:
            apply_led_post(body);
            return last_action_ok ? "/api/slots" : "/api/action-failed";

#endif
        case POST_REBOOT:
            apply_reboot_post(body);
            return last_action_ok ? "/api/reboot-ok" : "/api/action-failed";
        case POST_WOL:
            apply_wol_post(body);
            return "/api/wol_result";
        case POST_RESOLVE_MAC:
            apply_resolve_mac_post(body);
            return "/api/resolve_mac";

#ifdef ENABLE_WIFI_WOL
        case POST_WIFI_PROVISION:
            apply_wifi_provision_post(body);
            // The reply is served from the synthetic result route, which reports
            // provision_ok set just above. (The reboot is deferred ~1.2s by
            // wifi_net.cpp so this response reaches the phone first.)
            return "/api/wifi_provision_result";
        case POST_WIFI_RESET:
            apply_wifi_reset_post();
            return "/api/wifi_reset_result";

#endif
        case POST_CONFIG:
        default:
            apply_post(body);
            return last_save_ok ? "/api/config" : "/api/save-failed";
    }
    return "/api/save-failed"; // unreachable: every case returns
}

const char *web_api_post(const char *uri, char *body) {
    post_target_t target;
    if (!post_target_for(uri, &target)) return nullptr;
    return dispatch_post(target, body);
}

//--------------------------------------------------------------------+
// Init / service
//--------------------------------------------------------------------+

void web_api_task() {
    // Deferred BOOTSEL reboot (POST /api/reboot). The 500 ms grace lets the
    // reply reach the config page before the device leaves the bus; the
    // main loop keeps feeding the watchdog until then.
    if (bootsel_pending && time_reached(bootsel_at)) {
        printf("[NET] entering BOOTSEL (UF2 flash mode)\n");
#ifdef ENABLE_LED_STRIP
        // Solid orange, held for the whole flash-mode session: the ROM
        // bootloader runs no code of ours (so no animation is possible),
        // but the pixels latch this frame until the freshly flashed
        // firmware renders its first normal frame.
        ledstrip_hold_solid(255, 165, 0);
#endif
        rom_reset_usb_boot_extra(-1, 0, false); // does not return
    }
}
