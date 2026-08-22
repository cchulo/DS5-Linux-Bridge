//
// web_api.cpp -- the on-device web UI: config page + JSON API + captive
// portal, served by lwIP httpd. Transport-agnostic (see web_api.h): the same
// handlers answer over the USB-NCM netif and the CYW43 WiFi netif; lwIP has
// one httpd regardless of how many netifs feed it.
//
// Factored out of the retired NCM transport (migration phase 2), mirroring
// upstream kungaa's web_api split. The POST machinery carries upstream's
// hardening: a body-less POST can't strstr() a previous request's leftovers,
// and a partial body (client died mid-POST) is rejected instead of applied.
//

#include "web_api.h"

#ifdef ENABLE_WIFI_WOL

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lwip/apps/fs.h"
#include "lwip/apps/httpd.h"

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
#include "web_page.h"
#include "weblog.h"
#ifdef ENABLE_WIFI_WOL
#include "wifi_net.h"
#include "web_portal.h" // onboarding captive-portal page (served in AP mode)

// Results of the wifi provision/reset POSTs, reported via the synthetic
// /api/wifi_*_result routes. Set in the POST handlers (below fs_open_custom,
// which reads them), so declare here.
static bool provision_ok;
static bool wifi_reset_ok;
#endif

// Result of the most recent POST /api/wol ("Wake now"), reported via the
// synthetic /api/wol_result route (read in fs_open_custom, set below it).
static bool last_wol_ok;

//--------------------------------------------------------------------+
// Web access gate (latency guard)
//--------------------------------------------------------------------+
// lwIP's httpd cannot be stopped once started, so the server is gated
// per-request instead: outside AP onboarding, requests are served only while
// pairing mode is active (explicit pairing window open, or no controller
// bonded yet) or during a grace session armed by pairing mode and refreshed
// by served requests -- so an open config page stays alive, and dies ~10 min
// after the last request once pairing mode ends. Everything else gets a
// cheap refusal with no page/JSON work, and wifi_net.cpp withdraws the mDNS
// "<hostname>.local" record while the gate is closed, so in normal play the
// dongle spends no core0 time serving HTTP (input-latency guard) and is
// mDNS-invisible on the LAN. Pairing mode is enterable without the web UI:
// hold PS+Create ~3 s on a connected controller (main.cpp).
static constexpr uint64_t WEB_SESSION_US = 10ull * 60 * 1000 * 1000; // 10 min
static uint64_t web_session_until_us = 0;

bool web_api_access_allowed() {
    const uint64_t now = time_us_64();
#ifdef ENABLE_WIFI_WOL
    if (wifi_net_in_ap_mode()) return true;
#endif
    if (bt_pairing_mode_active()) {
        web_session_until_us = now + WEB_SESSION_US;
        return true;
    }
    return now < web_session_until_us;
}

// A request was actually served while the gate was open: keep the session
// alive so an active config-page visit doesn't expire mid-edit. (An open
// tab's status poll counts as activity; closing the tab lets the session
// lapse.)
static void web_session_touch() {
    const uint64_t until = time_us_64() + WEB_SESSION_US;
    if (until > web_session_until_us) web_session_until_us = until;
}

//--------------------------------------------------------------------+
// HTTP content: / (page), /api/config -- via fs_open_custom
//--------------------------------------------------------------------+

// Build a complete response (headers + body) into a malloc'd buffer owned by
// the fs_file (freed in fs_close_custom).
static int make_file(struct fs_file *file, const char *status, const char *content_type,
                     const char *body, int body_len) {
    const int hdr_max = 160;
    char *buf = (char *) malloc(hdr_max + body_len);
    if (!buf) return 0;
    int hdr_len = snprintf(buf, hdr_max,
                           "HTTP/1.1 %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
                           "Connection: close\r\nContent-Length: %d\r\n\r\n",
                           status, content_type, body_len);
    memcpy(buf + hdr_len, body, body_len);
    memset(file, 0, sizeof(*file));
    file->data = buf; // malloc'd; reclaimed in fs_close_custom via file->data
    file->len = (int) (hdr_len + body_len);
    file->index = file->len;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
    return 1;
}

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
                     "{\"max\":%u,\"connected\":%d,\"usb_exposed\":%u,\"audio_slot\":%u,\"audio_allowed\":%s,\"led\":%s,\"slots\":[",
                     BT_MAX_SLOTS, bt_connected_count(), usb_exposed_slot_count(),
                     tier_audio_slot(), tier_audio_allowed() ? "true" : "false", led_flag);
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

extern "C" int fs_open_custom(struct fs_file *file, const char *name) {
#ifdef ENABLE_WIFI_WOL
    // Onboarding mode: serve the captive portal for essentially every GET.
    if (wifi_net_in_ap_mode()) {
        // In AP mode the network is OPEN and BT is never initialized, so only
        // the onboarding routes may reach the shared handlers. Everything else
        // under /api/ (config, bonds -- forgetall really erases the TLV,
        // status, slots, led) is 404'd so a nearby actor who joins
        // DS5-Setup-XXXX can't rewrite config or wipe bonds. The portal page
        // itself only ever calls wifi_scan + wifi_provision(_result).
        const bool is_portal_api =
            (strcmp(name, "/api/wifi_scan") == 0) ||
            (strcmp(name, "/api/wifi_provision") == 0) ||
            (strcmp(name, "/api/wifi_provision_result") == 0);
        const bool other_api = (strncmp(name, "/api/", 5) == 0);
        if (other_api && !is_portal_api) {
            // A non-onboarding API GET in AP mode: reject.
            static const char nf[] = "not found";
            return make_file(file, "404 Not Found", "text/plain", nf, sizeof(nf) - 1);
        }
        if (!is_portal_api) {
            // Serve the portal page itself (200) for the root, the OS captive-probe
            // URLs (Windows /connecttest.txt + /index.shtml; Apple
            // /hotspot-detect.html; Android /generate_204; ...), AND any other GET.
            // We deliberately do NOT 302-redirect: redirecting probe URLs on the
            // SAME host makes the captive mini-browser re-request in a tight loop
            // and never render. Returning the page body directly for every path
            // breaks that loop and makes the "Sign in" sheet show the form
            // immediately. (make_file copies the ~4.6 KB page per request --
            // fine in AP mode, where BT/audio never start and the heap is free.)
            return make_file(file, "200 OK", "text/html; charset=utf-8",
                             PORTAL_PAGE, (int) (sizeof(PORTAL_PAGE) - 1));
        }
    }
#endif
    if (!web_api_access_allowed()) {
        // Gate closed (normal play): cheap refusal, no page/JSON work. The
        // hint tells a user who bookmarked the page how to reopen it.
        static const char gated[] =
            "web UI is sleeping. Hold PS+Create ~3s on a connected controller "
            "(or re-enter pairing mode) to wake it.";
        return make_file(file, "404 Not Found", "text/plain", gated, sizeof(gated) - 1);
    }
    web_session_touch();
    if (strcmp(name, "/") == 0 || strcmp(name, "/index.html") == 0) {
        // Serve the page straight from flash (headers included) -- zero heap.
        memset(file, 0, sizeof(*file));
        file->data = WEB_PAGE_RESPONSE;
        file->len = (int) (sizeof(WEB_PAGE_RESPONSE) - 1);
        file->index = file->len;
        file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
        return 1;
    }
    // Shared JSON scratch: make_file() copies the body into its own malloc'd
    // buffer before returning, and httpd serves one custom file at a time, so a
    // single static buffer is safe for both JSON routes (saves BSS -> heap).
    static char body[1024]; // sized for /api/config (largest JSON route)
    if (strcmp(name, "/api/config") == 0) {
        const int len = json_config(body, sizeof(body));
        return make_file(file, "200 OK", "application/json", body, len);
    }
    if (strcmp(name, "/api/bonds") == 0) {
        const int len = json_bonds(body, sizeof(body));
        return make_file(file, "200 OK", "application/json", body, len);
    }
    if (strcmp(name, "/api/status") == 0) {
        const int len = json_status(body, sizeof(body));
        return make_file(file, "200 OK", "application/json", body, len);
    }
    if (strcmp(name, "/api/slots") == 0) {
        const int len = json_slots(body, sizeof(body));
        return make_file(file, "200 OK", "application/json", body, len);
    }
    // Firmware log (RAM ring of all printf diagnostics; see weblog.h).
    // Plain text so it reads directly in a browser tab.
    if (strcmp(name, "/api/log") == 0) {
        static char logbuf[2113]; // frozen boot KB + gap marker + recent KB
        const int len = weblog_snapshot(logbuf, sizeof(logbuf));
        return make_file(file, "200 OK", "text/plain; charset=utf-8", logbuf, len);
    }
    // ARP-resolve poll, kicked off by POST /api/resolve_mac. The POST only
    // starts the lookup; the browser polls this GET until "pending":false,
    // then either {"ok":true,"mac":"AABBCCDDEEFF"} or {"ok":false}.
    if (strcmp(name, "/api/resolve_mac") == 0) {
        uint8_t mac[6];
        const int r = wifi_resolve_mac_poll_result(mac);
        int len;
        if (r == 0) {
            len = snprintf(body, sizeof(body), "{\"pending\":true}");
        } else if (r > 0) {
            char hex[13];
            addr_to_hex(mac, hex);
            len = snprintf(body, sizeof(body),
                           "{\"pending\":false,\"ok\":true,\"mac\":\"%s\"}", hex);
        } else {
            len = snprintf(body, sizeof(body), "{\"pending\":false,\"ok\":false}");
        }
        return make_file(file, "200 OK", "application/json", body, len);
    }
    // Synthetic reply for POST /api/wol ("Wake now").
    if (strcmp(name, "/api/wol_result") == 0) {
        const int len = snprintf(body, sizeof(body), "{\"ok\":%s}",
                                 last_wol_ok ? "true" : "false");
        return make_file(file, "200 OK", "application/json", body, len);
    }
#ifdef ENABLE_WIFI_WOL
    if (strcmp(name, "/api/wifi_scan") == 0) {
        // A scan is kicked off on first hit; later hits are the portal's
        // rate-limited refresh (see wifi_scan_start guards). Only live in AP
        // mode (wifi_scan_start no-ops otherwise; the list is then empty).
        wifi_scan_start();
        static char scanbuf[1024];
        const int len = wifi_scan_json(scanbuf, sizeof(scanbuf));
        return make_file(file, "200 OK", "application/json", scanbuf, len);
    }
    if (strcmp(name, "/api/wifi_provision_result") == 0) {
        // Synthetic reply for the provision POST (see httpd_post_finished).
        // Reports whether the creds were accepted; the device reboots into STA
        // shortly after.
        const int len = snprintf(body, sizeof(body), "{\"ok\":%s}",
                                 provision_ok ? "true" : "false");
        return make_file(file, "200 OK", "application/json", body, len);
    }
    if (strcmp(name, "/api/wifi_reset_result") == 0) {
        // Synthetic reply for POST /api/wifi_reset. If ok, the device has
        // cleared its WiFi credentials and will reboot into AP onboarding.
        const int len = snprintf(body, sizeof(body), "{\"ok\":%s}",
                                 wifi_reset_ok ? "true" : "false");
        return make_file(file, "200 OK", "application/json", body, len);
    }
#endif
    // POST /api/config redirects here when config_save() failed to reach flash.
    // Returning a non-2xx status makes the page's `r.ok` check false so it shows
    // an error instead of "Saved ✓" for a change that never persisted.
    // POST /api/slots or /api/led redirects here when the action was refused
    // (bad indices, slot mid-setup, LED debug on a build without the strip).
    if (strcmp(name, "/api/action-failed") == 0) {
        static const char af[] = "action failed";
        return make_file(file, "409 Conflict", "text/plain", af, sizeof(af) - 1);
    }
    // POST /api/bonds action=pair redirects here when the pairing request was
    // refused (all bond seats occupied, or all slots connected on a multi-slot
    // build). Non-2xx so the page can show a specific message.
    if (strcmp(name, "/api/pair-rejected") == 0) {
        static const char pr[] = "pairing rejected: no free controller slot or bond seat";
        return make_file(file, "409 Conflict", "text/plain", pr, sizeof(pr) - 1);
    }
    // POST /api/reboot redirects here once the BOOTSEL reboot is armed; the
    // device drops off the bus ~0.5 s after this response is sent and
    // re-enumerates as the ROM UF2 mass-storage bootloader.
    if (strcmp(name, "/api/reboot-ok") == 0) {
        static const char rb[] = "rebooting to BOOTSEL";
        return make_file(file, "200 OK", "text/plain", rb, sizeof(rb) - 1);
    }
    if (strcmp(name, "/api/save-failed") == 0) {
        static const char sf[] = "config save failed: flash not written";
        return make_file(file, "500 Internal Server Error", "text/plain", sf, sizeof(sf) - 1);
    }
    if (strcmp(name, "/404.html") == 0) {
        static const char nf[] = "not found";
        return make_file(file, "404 Not Found", "text/plain", nf, sizeof(nf) - 1);
    }
    return 0;
}

extern "C" void fs_close_custom(struct fs_file *file) {
    // Everything except the flash-resident page response is malloc'd by
    // make_file().
    if (file && file->data && file->data != WEB_PAGE_RESPONSE) {
        free(const_cast<char *>(file->data));
    }
    if (file) file->data = NULL;
}

extern "C" int fs_read_custom(struct fs_file *file, char *buffer, int count) {
    (void) file;
    (void) buffer;
    (void) count;
    return FS_READ_EOF; // all content is provided up front in fs_open_custom
}

//--------------------------------------------------------------------+
// POST /api/config -- form fields:
//   speaker_volume, inactive_time, disable_inactive_disconnect,
//   disable_pico_led, polling_rate_mode, audio_buffer_length, controller_mode
// Each value clamped to the same ranges config_valid() enforces, so a bad
// POST can never persist an out-of-range setting.
//--------------------------------------------------------------------+

// Must hold the ENTIRE /api/config save body (the page posts every field in
// one form body; ~550 B worst-case with the LED animation/color fields) --
// httpd_post_begin hard-rejects anything longer.
#define POST_BUFSIZE 768
static char post_buf[POST_BUFSIZE];
static u16_t post_pos;
static void *post_conn;
enum post_target_t { POST_CONFIG, POST_BONDS, POST_SLOTS, POST_LED, POST_REBOOT,
                     POST_WIFI_PROVISION, POST_WIFI_RESET, POST_WOL,
                     POST_RESOLVE_MAC };
static post_target_t post_target; // which endpoint the in-flight POST targets
static int post_content_len; // declared Content-Length; -1 = none sent
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

// POST /api/wifi_provision -- ssid=...&psk=... from the onboarding portal.
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
// next boot is unprovisioned, so it starts the AP captive portal.
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
// httpd POST callback runs nested inside lwIP's tcp_input, so it must never
// pump the stack); the browser polls GET /api/resolve_mac for the result once
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

extern "C" err_t httpd_post_begin(void *connection, const char *uri, const char *http_request,
                                  u16_t http_request_len, int content_len, char *response_uri,
                                  u16_t response_uri_len, u8_t *post_auto_wnd) {
    (void) http_request;
    (void) http_request_len;
    (void) response_uri;
    (void) response_uri_len;
    (void) post_auto_wnd;
    // Web access gate: refuse everything while closed (AP onboarding and
    // pairing mode pass; see web_api_access_allowed).
    if (!web_api_access_allowed()) return ERR_VAL;
    web_session_touch();
    post_target_t target;
    if (strcmp(uri, "/api/config") == 0) target = POST_CONFIG;
    else if (strcmp(uri, "/api/bonds") == 0) target = POST_BONDS;
    else if (strcmp(uri, "/api/slots") == 0) target = POST_SLOTS;
#ifdef ENABLE_LED_STRIP
    else if (strcmp(uri, "/api/led") == 0) target = POST_LED;
#endif
    else if (strcmp(uri, "/api/reboot") == 0) target = POST_REBOOT;
    else if (strcmp(uri, "/api/wol") == 0) target = POST_WOL;
    else if (strcmp(uri, "/api/resolve_mac") == 0) target = POST_RESOLVE_MAC;
#ifdef ENABLE_WIFI_WOL
    else if (strcmp(uri, "/api/wifi_provision") == 0) target = POST_WIFI_PROVISION;
    else if (strcmp(uri, "/api/wifi_reset") == 0) target = POST_WIFI_RESET;
#endif
    else return ERR_VAL;
#ifdef ENABLE_WIFI_WOL
    // In AP onboarding mode reject every POST except the provision flow. The
    // open AP + uninitialized BT means POST /api/bonds action=forgetall
    // (gap_delete_all_link_keys erases the TLV even with BT down) or
    // POST /api/config (rewrites+persists settings) must not be reachable.
    if (wifi_net_in_ap_mode() &&
        target != POST_WIFI_PROVISION && target != POST_WIFI_RESET) {
        return ERR_VAL;
    }
#endif
    if (content_len >= POST_BUFSIZE) return ERR_VAL;
    if (post_conn) return ERR_USE; // one POST at a time
    post_conn = connection;
    post_pos = 0;
    // Terminate now: a body-less POST never runs httpd_post_receive_data, and
    // the finished handler must not strstr() a previous request's leftovers.
    post_buf[0] = 0;
    post_target = target;
    post_content_len = content_len; // -1 when the client sent no Content-Length
    return ERR_OK;
}

extern "C" err_t httpd_post_receive_data(void *connection, struct pbuf *p) {
    if (connection == post_conn && p) {
        const u16_t space = POST_BUFSIZE - 1 - post_pos;
        const u16_t take = p->tot_len < space ? p->tot_len : space;
        post_pos += pbuf_copy_partial(p, post_buf + post_pos, take, 0);
        post_buf[post_pos] = 0;
    }
    if (p) pbuf_free(p);
    return ERR_OK;
}

extern "C" void httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len) {
    if (connection != post_conn) return;
    post_conn = nullptr;

    // lwIP httpd also calls this when a POST connection dies mid-body. If the
    // client declared a Content-Length and we didn't receive all of it, the
    // body is partial -- applying it can persist a silently-wrong value and
    // burn a flash erase/program cycle for a request the client never
    // finished. Reject without applying. (content_len == -1 means the client
    // sent no length, so we can't tell; fall through to the old behavior.)
    if (post_content_len >= 0 && post_pos != (u16_t) post_content_len) {
        snprintf(response_uri, response_uri_len, "/api/save-failed");
        return;
    }

    switch (post_target) {
        case POST_BONDS:
            apply_bonds_post(post_buf);
            snprintf(response_uri, response_uri_len,
                     last_pair_rejected ? "/api/pair-rejected"
                     : (last_save_ok ? "/api/bonds" : "/api/save-failed"));
            break;
        case POST_SLOTS:
            apply_slots_post(post_buf);
            snprintf(response_uri, response_uri_len,
                     last_action_ok ? "/api/slots" : "/api/action-failed");
            break;
#ifdef ENABLE_LED_STRIP
        case POST_LED:
            apply_led_post(post_buf);
            snprintf(response_uri, response_uri_len,
                     last_action_ok ? "/api/slots" : "/api/action-failed");
            break;
#endif
        case POST_REBOOT:
            apply_reboot_post(post_buf);
            snprintf(response_uri, response_uri_len,
                     last_action_ok ? "/api/reboot-ok" : "/api/action-failed");
            break;
        case POST_WOL:
            apply_wol_post(post_buf);
            snprintf(response_uri, response_uri_len, "/api/wol_result");
            break;
        case POST_RESOLVE_MAC:
            apply_resolve_mac_post(post_buf);
            snprintf(response_uri, response_uri_len, "/api/resolve_mac");
            break;
#ifdef ENABLE_WIFI_WOL
        case POST_WIFI_PROVISION:
            apply_wifi_provision_post(post_buf);
            // The reply is served from the synthetic result route, which reports
            // provision_ok set just above. (The reboot is deferred ~1.2s by
            // wifi_net.cpp so this response reaches the phone first.)
            snprintf(response_uri, response_uri_len, "/api/wifi_provision_result");
            break;
        case POST_WIFI_RESET:
            apply_wifi_reset_post();
            snprintf(response_uri, response_uri_len, "/api/wifi_reset_result");
            break;
#endif
        case POST_CONFIG:
        default:
            apply_post(post_buf);
            snprintf(response_uri, response_uri_len,
                     last_save_ok ? "/api/config" : "/api/save-failed");
            break;
    }
}

//--------------------------------------------------------------------+
// Init / service
//--------------------------------------------------------------------+

void web_api_init() {
    // One lwIP stack, one httpd listener; guarded in case a future second
    // caller joins wifi_net_init().
    static bool started = false;
    if (started) return;
    started = true;
    httpd_init();
}

void web_api_task() {
    // Deferred BOOTSEL reboot (POST /api/reboot). The 500 ms grace lets the
    // HTTP response reach the browser before the device leaves the bus; the
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

#endif // ENABLE_WIFI_WOL
