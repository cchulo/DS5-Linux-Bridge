//
// usb_net.cpp -- CDC-NCM USB network interface + lwIP + config web UI.
//
// The dongle additionally enumerates as a USB network adapter. A small lwIP
// stack runs over it (NO_SYS, serviced from the main loop):
//   - dhserver (TinyUSB lib/networking) hands the host an address
//   - lwIP httpd serves the config page and a JSON API; all content is
//     generated in fs_open_custom / the POST hooks below (no static fsdata)
//
// The DHCP offer deliberately carries no gateway and no DNS server so the
// host never tries to route internet traffic or DNS lookups through us.
//
// Adapted from the GPLv3 PC-wake-dongle project (same author); the BLE
// scan/device-list config was replaced with the DS5 firmware settings.
//

#include "usb_net.h"

#ifdef ENABLE_WEBCONFIG

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "tusb.h"

#include "dhserver.h"
#include "lwip/apps/fs.h"
#include "lwip/apps/httpd.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#include "pico/unique_id.h"

#include "bt.h"
#include "tier.h"
#include "usb.h"
#ifdef ENABLE_LED_STRIP
#include "ledstrip.h"
#endif
#include "config.h"
#include "web_page.h"
#include "weblog.h"

//--------------------------------------------------------------------+
// TinyUSB network glue (pattern from examples/device/net_lwip_webserver)
//--------------------------------------------------------------------+

// MAC the host's NIC uses; the device-side netif uses the same address with
// the last bit flipped (both locally administered, derived from the flash
// unique id in usb_net_init).
uint8_t tud_network_mac_address[6];

static struct netif netif_data;

#define INIT_IP4(a, b, c, d) {PP_HTONL(LWIP_MAKEU32(a, b, c, d))}

// Vetted /29 subnets selectable from the web UI (Config_body.webconfig_subnet).
// Each is an obscure corner of RFC 1918 space (homes mostly use 192.168.0/1.x
// or 10.0.0.x), spread across all three private blocks so if one ever collides
// with the user's real LAN, another almost certainly won't. The dongle takes
// .105; the host PC gets .106 by DHCP. For a preset this is an *index* into a
// fixed table, so a "wrong" choice still lands on a valid, reachable subnet.
// A fourth selector value (WEBCONFIG_SUBNET_CUSTOM) instead uses a user-entered
// private IP (Config_body.webconfig_custom_ip) -- power-user escape hatch, still
// constrained to RFC-1918 host addresses so it can't point somewhere unroutable.
//
// IMPORTANT: keep this table and the web page <select> in sync with
// WEBCONFIG_SUBNET_COUNT (usb_net.h) -- the static_assert below enforces the
// table length, and config_valid() clamps against the same constant.
struct subnet_def { uint8_t a, b, c; };
static const subnet_def kSubnets[] = {
    {10,  55,  55},   // 0: 10.55.55.105   (default; distinct from PC-wake-dongle)
    {172, 31,  55},   // 1: 172.31.55.105  (top of 172.16/12, almost never used)
    {192, 168, 137},  // 2: 192.168.137.105 (Windows ICS default range)
};
static_assert(sizeof(kSubnets) / sizeof(kSubnets[0]) == WEBCONFIG_SUBNET_COUNT,
              "kSubnets size must match WEBCONFIG_SUBNET_COUNT (usb_net.h)");

// True if a.b.c.d is a usable *private* host address for the config page: inside
// RFC-1918 (10/8, 172.16/12, 192.168/16), and not a .0 network / .255 broadcast
// / .105+ overflow within its /29. This is the gate that keeps "custom IP" from
// bricking config-page reachability -- YOLO means "any private host", not "any
// 32 bits". Shared with config_valid() via webconfig_ip_is_valid().
bool webconfig_ip_is_valid(const uint8_t ip[4]) {
    const uint8_t a = ip[0], b = ip[1], d = ip[3];
    const bool rfc1918 =
        (a == 10) ||
        (a == 172 && b >= 16 && b <= 31) ||
        (a == 192 && b == 168);
    if (!rfc1918) return false;
    // Keep the dongle host octet in [1,252] so its /29 (network = d & ~7) has
    // room for the .+1..+3 host leases below without hitting .255 broadcast.
    if (d < 1 || d > 252) return false;
    const uint8_t net = d & 0xF8; // /29 network base
    if (d == net) return false;       // network address
    if (d == (net + 7)) return false; // /29 broadcast
    return true;
}

// Resolved at init from the config index.
static ip4_addr_t ipaddr;
static ip4_addr_t netmask;
static ip4_addr_t gateway;
static dhcp_entry_t dhcp_entries[3];
static dhcp_config_t dhcp_config;

// Populate ipaddr/netmask/dhcp from the selected subnet (called in usb_net_init).
// For presets the dongle is .105 and the host leases are .106-.108. For a custom
// IP the dongle takes the user octets and the host leases are the other usable
// addresses in the same /29 (skipping the dongle's own host octet).
static void build_subnet(uint8_t idx, const uint8_t custom_ip[4]) {
    uint8_t da, db, dc, dd; // dongle address octets
    if (idx == WEBCONFIG_SUBNET_CUSTOM && webconfig_ip_is_valid(custom_ip)) {
        da = custom_ip[0]; db = custom_ip[1]; dc = custom_ip[2]; dd = custom_ip[3];
    } else {
        if (idx >= WEBCONFIG_SUBNET_COUNT) idx = 0;
        const subnet_def &s = kSubnets[idx];
        da = s.a; db = s.b; dc = s.c; dd = 105;
    }
    IP4_ADDR(&ipaddr,  da, db, dc, dd);
    IP4_ADDR(&netmask, 255, 255, 255, 248); // /29
    IP4_ADDR(&gateway, 0, 0, 0, 0);         // link-local only, no routing
    // Host leases: the three usable /29 host octets other than the dongle's own.
    const uint8_t net = dd & 0xF8;
    int n = 0;
    for (uint8_t off = 1; off <= 6 && n < 3; off++) {
        const uint8_t host = net + off;
        if (host == dd) continue; // don't lease the dongle's own address
        IP4_ADDR(&dhcp_entries[n].addr, da, db, dc, host);
        dhcp_entries[n].lease = 24 * 60 * 60;
        n++;
    }
    IP4_ADDR(&dhcp_config.router, 0, 0, 0, 0); // none
    dhcp_config.port = 67;
    IP4_ADDR(&dhcp_config.dns, 0, 0, 0, 0);    // none -- never hijack host lookups
    dhcp_config.domain = nullptr;
    dhcp_config.num_entry = 3;
    dhcp_config.entries = dhcp_entries;
}

static err_t linkoutput_fn(struct netif *netif, struct pbuf *p) {
    (void) netif;
    // Bounded wait: spin only briefly for the NCM endpoint to drain, then drop
    // the frame. An unbounded loop here deadlocks the main loop if the host is
    // not draining (e.g. an unsolicited broadcast sent before the host has
    // anything queued). Replies to host traffic always free up quickly; a
    // dropped unsolicited frame is harmless.
    const absolute_time_t deadline = make_timeout_time_ms(50);
    while (tud_ready()) {
        if (tud_network_can_xmit(p->tot_len)) {
            tud_network_xmit(p, 0);
            return ERR_OK;
        }
        if (time_reached(deadline)) return ERR_WOULDBLOCK;
        tud_task(); // service USB until the transmit path frees up
    }
    return ERR_USE;
}

static err_t netif_init_cb(struct netif *netif) {
    LWIP_ASSERT("netif != NULL", (netif != NULL));
    netif->mtu = CFG_TUD_NET_MTU;
    // No NETIF_FLAG_IGMP: no multicast on this link (mDNS removed). The config
    // page is reached by IP (http://10.55.55.105/).
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->state = NULL;
    netif->name[0] = 'u';
    netif->name[1] = 's';
    netif->linkoutput = linkoutput_fn;
    netif->output = etharp_output;
    return ERR_OK;
}

// Any host->device ethernet frame ever received this boot. Zero long after
// enumeration means the host never activated the NCM link (see the
// link-recovery bounce in usb_net_task).
static uint32_t net_rx_frames = 0;

// Process the frame inline (pattern from the TinyUSB 0.20 example): the NCM
// driver delivers one datagram at a time and recv_renew re-arms delivery.
extern "C" bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (size) {
        net_rx_frames++;
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (!p) return false;
        pbuf_take(p, src, size);
        if (netif_data.input(p, &netif_data) != ERR_OK) {
            pbuf_free(p);
        }
        tud_network_recv_renew();
    }
    return true;
}

extern "C" uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    struct pbuf *p = (struct pbuf *) ref;
    (void) arg;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

extern "C" void tud_network_init_cb(void) {
    // frames are processed inline in tud_network_recv_cb; nothing to reset
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

static int json_config(char *out, size_t cap) {
    const Config_body &c = get_config();
    return snprintf(out, cap,
                    "{\"version\":\"%s\","
                    "\"inactive_time\":%u,"
                    "\"disable_inactive_disconnect\":%u,"
                    "\"disable_pico_led\":%u,"
                    "\"polling_rate_mode\":%u,"
                    "\"audio_buffer_length\":%u,"
                    "\"controller_mode\":%u,"
                    "\"webconfig_subnet\":%u,"
                    "\"webconfig_custom_ip\":\"%u.%u.%u.%u\","
                    "\"slot_rgb\":[\"%02X%02X%02X\",\"%02X%02X%02X\","
                    "\"%02X%02X%02X\",\"%02X%02X%02X\"],"
                    "\"led_count\":%u,"
                    "\"led_max\":%u,"
                    "\"led_masks\":[\"%lX\",\"%lX\",\"%lX\",\"%lX\"],"
                    "\"pairing_mask\":\"%lX\","
                    "\"pairing_rgb\":\"%02X%02X%02X\","
                    "\"idle_rgb\":\"%02X%02X%02X\","
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
                    c.webconfig_subnet,
                    c.webconfig_custom_ip[0], c.webconfig_custom_ip[1],
                    c.webconfig_custom_ip[2], c.webconfig_custom_ip[3],
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
                     "{\"max\":%u,\"connected\":%d,\"audio_slot\":%u,\"audio_allowed\":%s,\"led\":%s,\"slots\":[",
                     BT_MAX_SLOTS, bt_connected_count(), tier_audio_slot(),
                     tier_audio_allowed() ? "true" : "false", led_flag);
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
    static char body[768]; // sized for /api/slots at 4 slots
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

#define POST_BUFSIZE 512
static char post_buf[POST_BUFSIZE];
static u16_t post_pos;
static void *post_conn;
enum post_target_t { POST_CONFIG, POST_BONDS, POST_SLOTS, POST_LED, POST_REBOOT };
static post_target_t post_target; // which endpoint the in-flight POST targets
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
        } else if (strcmp(tok, "webconfig_subnet") == 0) {
            c.webconfig_subnet = (uint8_t) clampi(val, 0, WEBCONFIG_SUBNET_MAX);
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
        } else if (strcmp(tok, "webconfig_custom_ip") == 0) {
            // Dotted-quad "a.b.c.d" (dots aren't URL-encoded). Parse leniently;
            // config_valid() is the real gate and rejects non-private addresses.
            unsigned a = 0, b = 0, cc = 0, d = 0;
            if (sscanf(eq, "%u.%u.%u.%u", &a, &b, &cc, &d) == 4 &&
                a <= 255 && b <= 255 && cc <= 255 && d <= 255) {
                c.webconfig_custom_ip[0] = (uint8_t) a;
                c.webconfig_custom_ip[1] = (uint8_t) b;
                c.webconfig_custom_ip[2] = (uint8_t) cc;
                c.webconfig_custom_ip[3] = (uint8_t) d;
            }
        }
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
// The reboot itself happens in usb_net_task().
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

extern "C" err_t httpd_post_begin(void *connection, const char *uri, const char *http_request,
                                  u16_t http_request_len, int content_len, char *response_uri,
                                  u16_t response_uri_len, u8_t *post_auto_wnd) {
    (void) http_request;
    (void) http_request_len;
    (void) response_uri;
    (void) response_uri_len;
    (void) post_auto_wnd;
    post_target_t target;
    if (strcmp(uri, "/api/config") == 0) target = POST_CONFIG;
    else if (strcmp(uri, "/api/bonds") == 0) target = POST_BONDS;
    else if (strcmp(uri, "/api/slots") == 0) target = POST_SLOTS;
#ifdef ENABLE_LED_STRIP
    else if (strcmp(uri, "/api/led") == 0) target = POST_LED;
#endif
    else if (strcmp(uri, "/api/reboot") == 0) target = POST_REBOOT;
    else return ERR_VAL;
    if (content_len >= POST_BUFSIZE) return ERR_VAL;
    if (post_conn) return ERR_USE; // one POST at a time
    post_conn = connection;
    post_pos = 0;
    post_target = target;
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

void usb_net_init() {
    // Stable locally-administered MAC derived from the flash unique id.
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    tud_network_mac_address[0] = 0x02;
    memcpy(tud_network_mac_address + 1, board_id.id + 3, 5);

    // Resolve the selected subnet (web-UI configurable) before bringing up lwIP.
    const Config_body &cfg = get_config();
    build_subnet(cfg.webconfig_subnet, cfg.webconfig_custom_ip);

    lwip_init();

    netif_data.hwaddr_len = 6;
    memcpy(netif_data.hwaddr, tud_network_mac_address, 6);
    netif_data.hwaddr[5] ^= 0x01; // device side must differ from host side

    netif_add(&netif_data, &ipaddr, &netmask, &gateway, NULL, netif_init_cb, ethernet_input);
#if LWIP_NETIF_HOSTNAME
    netif_set_hostname(&netif_data, "ds5config");
#endif
    netif_set_default(&netif_data);
    netif_set_up(&netif_data);

    if (dhserv_init(&dhcp_config) != ERR_OK) {
        printf("[NET] dhcp server init failed\n");
    }
    httpd_init();

    printf("[NET] config UI at http://%s/\n", ip4addr_ntoa(&ipaddr));
}

void usb_net_task() {
    sys_check_timeouts();
    // Deferred BOOTSEL reboot (POST /api/reboot). The 500 ms grace lets the
    // HTTP response reach the browser before the USB link disappears; the main
    // loop keeps feeding the watchdog until then.
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

#ifdef ENABLE_WAKE_HID
    // NCM link-recovery bounce. After a warm reboot (UF2 flash / watchdog)
    // macOS sometimes re-binds the whole composite -- HID and audio work --
    // but never activates the NCM data interface: its ethernet interface
    // sits "inactive" (link down, alt setting 0), no DHCP happens, and the
    // config page is unreachable until the dongle is replugged. A
    // tud_disconnect()/tud_connect() bounce IS a replug as far as the host
    // can tell, so: if the host has never sent a single ethernet frame by
    // NET_QUIET_BOUNCE_MS after enumeration, request one rebind (at most
    // twice per boot). A live host always talks within a couple of seconds
    // (DHCP/ARP/mDNS); a host with no NCM driver at all just sees at most
    // two extra re-enumerations. Never fires while a controller is
    // connected -- the config page is not worth yanking a gamepad mid-game.
    constexpr uint32_t NET_QUIET_BOUNCE_MS = 8000;
    static absolute_time_t net_quiet_deadline = nil_time;
    static uint8_t net_bounces = 0;
    if (!tud_mounted()) {
        net_quiet_deadline = nil_time; // re-arms on the next mount
    } else if (is_nil_time(net_quiet_deadline)) {
        net_quiet_deadline = make_timeout_time_ms(NET_QUIET_BOUNCE_MS);
    } else if (net_rx_frames == 0 && net_bounces < 2 &&
               time_reached(net_quiet_deadline) &&
               !tud_suspended() && !usb_variant_swap_in_progress() &&
               bt_connected_count() == 0) {
        net_bounces++;
        printf("[NET] no host network traffic since enumeration -> USB rebind (%u/2)\n",
               net_bounces);
        usb_request_rebind();
        net_quiet_deadline = nil_time;
    }
#endif
}

#endif // ENABLE_WEBCONFIG
