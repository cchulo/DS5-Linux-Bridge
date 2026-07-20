//
// wifi_net.cpp -- onboard CYW43 Wi-Fi (STA) transport: config web UI + WOL.
//
// Ported from kungaa/DS5-Linux-Bridge (see wifi_net.h for the migration note).
// The single CYW43 radio carries BT (Opus audio, latency-critical) AND WiFi
// (config/WOL TCP) at once -- radio contention with audio is the thing to watch
// when touching this file.
//
// STRUCTURE:
//   - No manual netif. The SDK's cyw43_arch (CYW43_LWIP=1) creates and owns the
//     lwIP netif for the WiFi interface, runs its RX into lwIP, and is pumped by
//     cyw43_arch_poll() already in the main loop. We just join the WLAN and
//     start DHCP.
//   - lwIP is initialised by cyw43_arch_init() (lwip_nosys_init -> lwip_init),
//     so we MUST NOT call lwip_init() here, or lwIP double-inits. (usb_net.cpp
//     skips its own lwip_init() in this build for the same reason.)
//   - DUAL TRANSPORT (migration): the NCM netif keeps netif_default so its DHCP
//     server broadcasts keep working. Everything here must therefore address
//     the STA netif EXPLICITLY (sta_netif()) -- never netif_default.
//   - The ARP resolve must be a start/poll split serviced from the main loop,
//     never blocked inside the httpd POST callback (see the resolve section).
//

#include "wifi_net.h"

#ifdef ENABLE_WIFI_WOL

#include <cstdio>
#include <cstring>

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "pico/unique_id.h"

#include "hardware/watchdog.h"

#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/udp.h"
#include "lwip/apps/mdns.h"
#include "lwip/apps/httpd.h"

#include "dhcpserver.h"
#include "dnsserver.h"

#include "config.h"

// The CYW43 STA netif, addressed explicitly (see the dual-transport note).
static struct netif *sta_netif() { return &cyw43_state.netif[CYW43_ITF_STA]; }

//--------------------------------------------------------------------+
// Wake-on-LAN: 102-byte magic packet broadcast as UDP to 255.255.255.255:9.
// lwIP frames ethernet/IP/UDP; the cyw43 netif carries it out the radio.
//--------------------------------------------------------------------+

bool wifi_wol_send(const uint8_t mac[6]) {
    uint8_t magic[102];
    memset(magic, 0xFF, 6);
    for (int i = 0; i < 16; i++) memcpy(magic + 6 + i * 6, mac, 6);

    struct udp_pcb *pcb = udp_new();
    if (!pcb) return false;
    ip_set_option(pcb, SOF_BROADCAST);

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(magic), PBUF_RAM);
    if (!p) { udp_remove(pcb); return false; }
    memcpy(p->payload, magic, sizeof(magic));

    ip_addr_t bcast;
    IP4_ADDR(&bcast, 255, 255, 255, 255);
    // Explicit egress netif: a global broadcast would otherwise route via
    // netif_default, which the NCM transport owns during the migration.
    const err_t e = udp_sendto_if(pcb, p, &bcast, 9, sta_netif());

    pbuf_free(p);
    udp_remove(pcb);
    return e == ERR_OK;
}

static bool mac_is_zero(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) if (mac[i]) return false;
    return true;
}

// Fire a magic packet at every configured (non-zero) WOL target -- currently
// wol_target_mac (the PC) and wol_target_mac2 (e.g. a TV). Shared by the wake
// companion (wake_emit_wol) and the web UI's "Wake now" so both wake ALL stored
// targets. Returns true if at least one packet was sent. Skips silently in AP
// onboarding mode (no LAN uplink -- the caller usually gates this too).
bool wifi_wol_send_all(void) {
    if (wifi_net_in_ap_mode()) return false;
    const Config_body &c = get_config();
    const uint8_t *targets[2] = { c.wol_target_mac, c.wol_target_mac2 };
    bool any = false;
    for (const uint8_t *mac : targets) {
        if (mac_is_zero(mac)) continue;
        const bool sent = wifi_wol_send(mac);
        printf("[wifi] WOL %02X:%02X:%02X:%02X:%02X:%02X %s\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
               sent ? "sent" : "send failed");
        any = any || sent;
    }
    return any;
}

//--------------------------------------------------------------------+
// Non-blocking ARP resolve (IP -> MAC) for the web UI's "Find MAC". The POST
// callback only records the target; wifi_net_task() drives the cache-check/
// query/timeout on the main loop. The split is mandatory: the POST callback
// runs nested inside lwIP's own tcp_input()/timeout processing, so pumping the
// stack from in there to wait for the ARP reply would re-enter non-reentrant
// lwIP internals and corrupt the in-flight connection.
//--------------------------------------------------------------------+

#define ARP_RESOLVE_BUDGET_MS 600

enum class ResolveState { IDLE, PENDING, DONE_OK, DONE_FAIL };
static ResolveState resolve_state = ResolveState::IDLE;
static ip4_addr_t resolve_target;
static uint8_t resolve_mac_out[6];
static bool resolve_queried = false;
static absolute_time_t resolve_deadline;

void wifi_resolve_mac_start(const uint8_t ip[4]) {
    IP4_ADDR(&resolve_target, ip[0], ip[1], ip[2], ip[3]);
    resolve_queried = false;
    resolve_deadline = make_timeout_time_ms(ARP_RESOLVE_BUDGET_MS);
    resolve_state = ResolveState::PENDING;
}

static void wifi_resolve_poll(void) {
    if (resolve_state != ResolveState::PENDING) return;
    struct netif *nif = sta_netif();
    if (!netif_is_up(nif)) { resolve_state = ResolveState::DONE_FAIL; return; }
    struct eth_addr *eth = nullptr;
    const ip4_addr_t *found = nullptr;
    if (etharp_find_addr(nif, &resolve_target, &eth, &found) >= 0 && eth) {
        memcpy(resolve_mac_out, eth->addr, 6);
        resolve_state = ResolveState::DONE_OK;
        return;
    }
    if (!resolve_queried) {
        etharp_query(nif, &resolve_target, nullptr);
        resolve_queried = true;
    }
    if (time_reached(resolve_deadline)) {
        resolve_state = ResolveState::DONE_FAIL;
    }
}

int wifi_resolve_mac_poll_result(uint8_t out_mac[6]) {
    switch (resolve_state) {
        case ResolveState::DONE_OK:
            memcpy(out_mac, resolve_mac_out, 6);
            return 1;
        case ResolveState::PENDING:
            return 0;
        default:
            return -1;
    }
}

// Wake companion hook, called from wake.cpp's request_host_wake() whenever a
// genuine host wake is warranted (wired up in the WOL migration phase; unused
// until then). wake.cpp owns WHEN (and the once-per-spell rate-limit); this
// just emits to the configured targets. Returns true if a packet was sent.
extern "C" bool wake_emit_wol(void) {
    // No LAN in onboarding mode -- the SoftAP carries only the local portal, so
    // a magic packet has nowhere to go. (wifi_wol_send_all() re-checks this too.)
    if (wifi_net_in_ap_mode()) return false;
    // Wake every configured target (PC + optional 2nd, e.g. a TV).
    return wifi_wol_send_all();
}

//--------------------------------------------------------------------+
// Mode + state
//--------------------------------------------------------------------+

// Two mutually-exclusive runtime modes (never both -- a device is either
// onboarding or operating):
//   STA: provisioned, joined the home WLAN, config page + WOL (normal use).
//   AP : unprovisioned (or forced), SoftAP + captive portal (onboarding).
static bool in_ap_mode = false;
static bool force_ap = false;          // set by wifi_net_request_ap_onboarding()
static bool wifi_mdns_added = false;   // STA: mDNS netif registered once

// AP-mode IP plan: network 10.55.55.104/29, dongle (gateway) at 10.55.55.105,
// DHCP hands clients .106-.110 (see dhcpserver.h DHCPS_BASE_IP/DHCPS_MAX_IP,
// tuned to this /29). The /29 caps the pool at 5 client slots so nobody can
// cram a crowd of stations onto the single radio and starve BT/audio. The DNS
// server answers every lookup with .105 so the captive-portal sheet pops on the
// phone. (Same subnet as the NCM default preset -- never both live at once:
// AP mode skips usb_net_init().)
#define AP_GW_A 10
#define AP_GW_B 55
#define AP_GW_C 55
#define AP_GW_D 105

static dhcp_server_t ap_dhcp;
static dns_server_t  ap_dns;

bool wifi_net_in_ap_mode() { return in_ap_mode; }
void wifi_net_request_ap_onboarding() { force_ap = true; }

//--------------------------------------------------------------------+
// STA: join the home WLAN (non-blocking)
//--------------------------------------------------------------------+

// Kick off a join WITHOUT blocking. The blocking
// cyw43_arch_wifi_connect_timeout_ms stalls main() for up to ~20s when the
// 2.4GHz join is flaky -- that boot-time stall would starve BT/USB bring-up and
// destabilize DualSense enumeration. The async variant returns immediately; the
// join progresses in the cyw43 poll context (pumped by cyw43_arch_poll() in the
// main loop) and wifi_net_task() watches cyw43_tcpip_link_status to detect
// success, failure, and when to retry. So WiFi can never block the audio path.
static void wifi_start_join(void) {
    const Config_body &c = get_config();
    // WPA2_MIXED accepts WPA2-AES and WPA/WPA2-mixed routers; the driver treats
    // it like WPA2_AES for pure-WPA2 networks. An empty PSK means an open
    // network -> pass auth OPEN so the join isn't gated on a key.
    const uint32_t auth = (c.wifi_psk[0] == '\0') ? CYW43_AUTH_OPEN
                                                   : CYW43_AUTH_WPA2_MIXED_PSK;
    printf("[wifi] connecting to SSID \"%s\" (async)...\n", c.wifi_ssid);
    const int rc = cyw43_arch_wifi_connect_async(
        c.wifi_ssid, c.wifi_psk[0] ? c.wifi_psk : NULL, auth);
    if (rc) printf("[wifi] connect kickoff failed (rc=%d); will retry\n", rc);
}

static void wifi_sta_init(void) {
    // STA mode on. (cyw43_arch_init() already ran in main(); it brought up the
    // driver, BT, and lwIP together. We do NOT lwip_init() here.)
    cyw43_arch_enable_sta_mode();

    // Network hostname advertised as "<hostname>.local". User-set in config so
    // multiple dongles on one LAN don't collide on ds5.local. config_valid()
    // has already sanitized it to a valid DNS label (and defaulted it if empty),
    // so it's safe to use verbatim.
    const char *hostname = get_config().hostname;
#if LWIP_NETIF_HOSTNAME
    netif_set_hostname(sta_netif(), hostname);
#endif

#if LWIP_MDNS_RESPONDER
    // The mDNS responder can be initialised now; the per-netif registration that
    // actually advertises "<hostname>.local" is deferred to wifi_net_task() once
    // the link is up (adding a netif before it has a link/IP is pointless).
    mdns_resp_init();
#endif

    // Kick off the join asynchronously and return immediately -- do NOT block
    // boot on it (see wifi_start_join). main() proceeds straight to BT/audio/USB
    // init; wifi_net_task() drives the join to completion and retries on failure.
    wifi_start_join();

    printf("[wifi] STA transport starting (http://%s.local/ once a lease lands)\n", hostname);
}

//--------------------------------------------------------------------+
// AP: SoftAP + captive portal for onboarding
//--------------------------------------------------------------------+

// "DS5-Setup-XXXX" where XXXX is the last 2 bytes of the board unique id, so
// multiple dongles being set up in the same room have distinct AP names.
static char ap_ssid[20];
static void build_ap_ssid(void) {
    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);
    snprintf(ap_ssid, sizeof(ap_ssid), "DS5-Setup-%02X%02X",
             uid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2],
             uid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);
}

// Setup-AP password. Deliberately NOT secret -- it's printed in the docs, the
// config page and the UART log; WPA2 here is a doorman, not a vault. It keeps
// drive-by devices and neighbors from auto-joining an open network (and
// keeps the portal traffic encrypted); the API allowlist in AP mode remains
// the real guard on what a joined client can do. Overridable at configure
// time (CMake WIFI_SETUP_PSK); must be 8..63 chars (WPA2).
#ifndef WIFI_AP_SETUP_PSK
#define WIFI_AP_SETUP_PSK "dualsense"
#endif
static_assert(sizeof(WIFI_AP_SETUP_PSK) - 1 >= 8,
              "WPA2 passphrase must be at least 8 characters");
static_assert(sizeof(WIFI_AP_SETUP_PSK) - 1 <= 63,
              "WPA2 passphrase must be at most 63 characters");

static void wifi_ap_init(void) {
    in_ap_mode = true;
    build_ap_ssid();

    // WPA2 with the fixed, documented password above. (Upstream ships this AP
    // open; we diverge -- see the password comment.) The captive-portal
    // sign-in sheet still pops after joining: detection is HTTP-probe based,
    // independent of the network's auth.
    cyw43_arch_enable_ap_mode(ap_ssid, WIFI_AP_SETUP_PSK, CYW43_AUTH_WPA2_AES_PSK);

    // Give the AP netif a fixed address. Address it explicitly via
    // cyw43_state.netif[CYW43_ITF_AP] rather than netif_default: the SDK sets
    // netif_default per-netif and in a mixed setup it may point at the STA netif,
    // so relying on it here is fragile (this is also what the pico-examples AP
    // demo does).
    ip4_addr_t gw, mask;
    IP4_ADDR(&gw, AP_GW_A, AP_GW_B, AP_GW_C, AP_GW_D);
    // /29 (255.255.255.248): 8 addresses .104-.111, usable hosts .105-.110.
    // Gateway/dongle at .105, DHCP pool .106-.110 (see dhcpserver.h). Small on
    // purpose -- 5 client slots max on the single BT/WiFi radio.
    IP4_ADDR(&mask, 255, 255, 255, 248);
    struct netif *apn = &cyw43_state.netif[CYW43_ITF_AP];
    netif_set_addr(apn, &gw, &mask, &gw);
    // Force broadcast routing out the AP netif. The DHCP server replies to
    // 255.255.255.255 (the client has no IP yet); lwIP routes a global broadcast
    // via netif_default. With both the STA netif (created by cyw43_arch_init even
    // though we never joined) and the AP netif present, netif_default can be the
    // wrong (STA, link-down) interface -> the ACK never reaches the client and it
    // loops REQUEST forever. Pinning default to the AP netif fixes egress for the
    // DHCP + DNS replies. (usb_net_init() is skipped in AP mode, so the NCM netif
    // never competes for default here.)
    netif_set_default(apn);

    // DHCP + DNS servers so a phone gets a lease and every lookup resolves to us
    // (captive-portal detection -> the OS pops the "Sign in" sheet).
    dhcp_server_init(&ap_dhcp, &gw, &mask);
    dns_server_init(&ap_dns, &gw);

    // usb_net_init() is skipped in AP mode, so start httpd here; the handlers
    // (fs_open_custom + POST hooks, usb_net.cpp) serve the portal page when
    // wifi_net_in_ap_mode() is true.
    httpd_init();

    printf("[wifi] AP onboarding: join \"%s\" (password \"%s\") then browse to http://%u.%u.%u.%u/\n",
           ap_ssid, WIFI_AP_SETUP_PSK, AP_GW_A, AP_GW_B, AP_GW_C, AP_GW_D);
}

//--------------------------------------------------------------------+
// WiFi scan (AP mode, for the portal's network dropdown)
//--------------------------------------------------------------------+

#define SCAN_MAX 16
struct ScanEntry {
    char ssid[33];
    int16_t rssi;
    uint8_t secure;
};
static ScanEntry scan_list[SCAN_MAX];
static int scan_count = 0;
static bool scan_in_progress = false;
static bool scan_ever_started = false;   // has a scan ever been kicked off?
static absolute_time_t scan_min_next = {0}; // earliest a NEW scan may start (rate-limit)

// Driver callback (cyw43 poll context). De-dup by SSID, keep the strongest RSSI.
static int scan_result_cb(void *env, const cyw43_ev_scan_result_t *r) {
    (void) env;
    if (!r || r->ssid_len == 0 || r->ssid_len > 32) return 0; // skip hidden/garbage
    char ssid[33];
    memcpy(ssid, r->ssid, r->ssid_len);
    ssid[r->ssid_len] = '\0';

    for (int i = 0; i < scan_count; i++) {
        if (strcmp(scan_list[i].ssid, ssid) == 0) {
            if (r->rssi > scan_list[i].rssi) scan_list[i].rssi = r->rssi;
            return 0; // already have it
        }
    }
    if (scan_count < SCAN_MAX) {
        strcpy(scan_list[scan_count].ssid, ssid);
        scan_list[scan_count].rssi = r->rssi;
        scan_list[scan_count].secure = (r->auth_mode != 0) ? 1 : 0;
        scan_count++;
    }
    return 0;
}

// Kick off a scan, but only when appropriate. The portal polls GET
// /api/wifi_scan repeatedly to refresh the dropdown while a scan runs; calling
// this on every poll would RESTART the scan each time -> "scanning" never
// goes false -> the page polls forever and the list flickers. Guards:
//   - only one scan at a time (scan_in_progress / cyw43_wifi_scan_active);
//   - a NEW scan may only begin after scan_min_next (rate-limit ~8s), so a busy
//     poll loop can't re-trigger back-to-back scans;
//   - results ACCUMULATE across scans (no scan_count reset) so the dropdown is
//     stable and only grows; the dedup in scan_result_cb keeps it clean.
// The very first call auto-starts (portal just loaded); later calls are the
// rate-limited refresh / the rescan button.
void wifi_scan_start(void) {
    if (!in_ap_mode) return;            // scan only matters during onboarding
    if (scan_in_progress) return;
    if (cyw43_wifi_scan_active(&cyw43_state)) return;
    if (scan_ever_started && !time_reached(scan_min_next)) return; // rate-limit
    cyw43_wifi_scan_options_t opts = {0};
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result_cb) == 0) {
        scan_in_progress = true;
        scan_ever_started = true;
        scan_min_next = make_timeout_time_ms(8000);
        printf("[wifi] scan started\n");
    }
}

// JSON array of networks for the portal, strongest first.
int wifi_scan_json(char *out, int cap) {
    // Insertion sort by RSSI desc (tiny list).
    for (int i = 1; i < scan_count; i++) {
        ScanEntry e = scan_list[i];
        int j = i - 1;
        while (j >= 0 && scan_list[j].rssi < e.rssi) {
            scan_list[j + 1] = scan_list[j];
            j--;
        }
        scan_list[j + 1] = e;
    }
    int w = snprintf(out, cap, "{\"scanning\":%s,\"nets\":[",
                     scan_in_progress ? "true" : "false");
    for (int i = 0; i < scan_count; i++) {
        // Bound this entry's worst-case serialized length so we never emit a
        // half-written network: SSID up to 2x (every byte escaped) + the fixed
        // {"ssid":""..."rssi":-nnn,"secure":n} scaffolding + the "," separator,
        // and leave room for the closing "]}". If it wouldn't fit, stop here --
        // the list is sorted strongest-first, so we keep the networks the user
        // most likely wants and still close the JSON cleanly.
        int need = 1 /*,*/ + 10 /*{"ssid":"*/ + 2 * (int)strlen(scan_list[i].ssid) +
                   1 /*"*/ + 34 /*,"rssi":-nnn,"secure":n}*/ + 2 /*]}*/;
        if (w + need > cap) break;
        w += snprintf(out + w, cap - w, "%s{\"ssid\":\"", i ? "," : "");
        for (const char *p = scan_list[i].ssid; *p; p++) {
            if (*p == '"' || *p == '\\') out[w++] = '\\';
            out[w++] = *p;
        }
        w += snprintf(out + w, cap - w, "\",\"rssi\":%d,\"secure\":%u}",
                      scan_list[i].rssi, scan_list[i].secure);
    }
    w += snprintf(out + w, cap - w, "]}");
    return w;
}

//--------------------------------------------------------------------+
// Provisioning: save creds + reboot into STA
//--------------------------------------------------------------------+

static bool reboot_pending = false;
static absolute_time_t reboot_at;

static void wifi_schedule_reboot(uint32_t delay_ms) {
    reboot_at = make_timeout_time_ms(delay_ms);
    reboot_pending = true;
}

bool wifi_provision_apply(const char *ssid, const char *psk) {
    if (!ssid) ssid = "";
    if (!psk) psk = "";
    const size_t ssid_len = strlen(ssid);
    const size_t psk_len = strlen(psk);
    if (ssid_len == 0 || ssid_len >= CONFIG_WIFI_SSID_LEN ||
        psk_len >= CONFIG_WIFI_PSK_LEN ||
        (psk_len > 0 && psk_len < 8)) {
        printf("[wifi] provisioning rejected (ssid=%u bytes, psk=%u bytes)\n",
               (unsigned) ssid_len, (unsigned) psk_len);
        return false;
    }
    config_set_wifi_creds(ssid, psk);
    watchdog_update();        // sector erase blocks with interrupts off
    if (!config_save()) return false;
    printf("[wifi] provisioned SSID \"%s\"; rebooting into STA mode\n", ssid);
    // Defer the reboot a beat so the HTTP "saved" response can flush to the
    // phone before the watchdog resets us. wifi_net_task() fires it.
    wifi_schedule_reboot(1200);
    return true;
}

bool wifi_reset_provisioning_apply() {
    config_set_wifi_creds("", "");
    watchdog_update();
    if (!config_save()) return false;
    printf("[wifi] WiFi credentials cleared; rebooting to AP onboarding\n");
    wifi_schedule_reboot(1200);
    return true;
}

//--------------------------------------------------------------------+
// Init / service
//--------------------------------------------------------------------+

void wifi_net_init() {
    const bool provisioned = get_config().wifi_provisioned;
    if (force_ap || !provisioned) {
        wifi_ap_init();       // onboarding
    } else {
        wifi_sta_init();      // normal operation
    }
}

void wifi_net_task() {
    // Provisioning/reset reboots are deferred so the HTTP response can flush
    // before the watchdog reset. This must work from AP onboarding and from the
    // normal STA config page. (lwIP timers are pumped by usb_net_task() in the
    // dual-transport build; in AP mode usb_net is skipped, so pump them here.)
    if (in_ap_mode) sys_check_timeouts();

    if (reboot_pending && time_reached(reboot_at)) {
        watchdog_reboot(0, 0, 0);
        return;
    }

    if (in_ap_mode) {
        // Track scan completion so the portal can stop polling.
        if (scan_in_progress && !cyw43_wifi_scan_active(&cyw43_state)) {
            scan_in_progress = false;
            printf("[wifi] scan done (%d networks)\n", scan_count);
        }
        return; // no STA link tracking / WOL while onboarding
    }

    wifi_resolve_poll();

    // ~1s status poll: log link state transitions + the DHCP lease once it
    // lands, and retry the join if we never associated (or dropped).
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;
    next = make_timeout_time_ms(1000);

    const int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    static int prev_link = -2;
    if (link != prev_link) {
        const char *s = (link == CYW43_LINK_UP) ? "UP"
                      : (link == CYW43_LINK_JOIN) ? "JOINING"
                      : (link == CYW43_LINK_NOIP) ? "NO-IP"
                      : (link == CYW43_LINK_FAIL) ? "FAIL"
                      : (link == CYW43_LINK_NONET) ? "NO-NET"
                      : (link == CYW43_LINK_BADAUTH) ? "BADAUTH" : "DOWN";
        printf("[wifi] link %s\n", s);
        prev_link = link;
    }

    // STA join supervision. The CYW43 commonly fails the first join or two
    // (transient FAIL/NONET), so we must retry patiently -- but we must ALSO not
    // strand the user on a permanently-failing STA (wrong password, or a network
    // that's gone), or they'd have no way back without BOOTSEL. Strategy:
    //   - Once we connect successfully even ONCE, mark ever_connected and retry
    //     forever on any later drop (a real network blip should self-heal).
    //   - Until the FIRST successful connect, run a verification budget: keep
    //     retrying for ~45s; if BADAUTH (wrong key) is seen twice, or the budget
    //     expires with no connection, the credentials are bad -> clear
    //     wifi_provisioned in flash and reboot back into AP onboarding so the user
    //     can re-enter them. (BT/audio are up in STA mode, so we can't just flip to
    //     AP live -- a reboot is the clean path, and it lands in AP because the
    //     provisioned flag is now clear.)
    static bool ever_connected = false;
    static bool verify_started = false;
    static absolute_time_t verify_deadline;
    static int badauth_seen = 0;
    static bool badauth_latched = false;
    if (!verify_started) {
        verify_started = true;
        verify_deadline = make_timeout_time_ms(45000); // first-join budget
    }

    // Count BADAUTH episodes, not 1 Hz samples. The latch must reset on JOINING
    // or NO-IP between failed attempts; otherwise repeated wrong-key failures
    // look like one long BADAUTH and only the 45s timeout can recover.
    if (link == CYW43_LINK_BADAUTH) {
        if (!badauth_latched) badauth_seen++;
        badauth_latched = true;
    } else {
        badauth_latched = false;
    }

    static bool reported_ip = false;
    if (link == CYW43_LINK_UP &&
        !ip4_addr_isany_val(*netif_ip4_addr(sta_netif()))) {
        ever_connected = true;
#if LWIP_MDNS_RESPONDER
        // Advertise "<hostname>.local" now that we have a link + IP. Done once.
        if (!wifi_mdns_added) {
            mdns_resp_add_netif(sta_netif(), get_config().hostname);
            wifi_mdns_added = true;
        }
#endif
        if (!reported_ip) {
            printf("[wifi] IP %s -- http://%s/ (or http://%s.local/)\n",
                   ip4addr_ntoa(netif_ip4_addr(sta_netif())),
                   ip4addr_ntoa(netif_ip4_addr(sta_netif())),
                   get_config().hostname);
            reported_ip = true;
        }
    } else if (link == CYW43_LINK_DOWN || link == CYW43_LINK_FAIL ||
               link == CYW43_LINK_NONET || link == CYW43_LINK_BADAUTH) {
        reported_ip = false;

        // Give up -> re-onboard ONLY if we've never connected this boot AND
        // either the password is clearly wrong (BADAUTH twice) or the budget ran
        // out. A device that connected before keeps retrying forever instead.
        if (!ever_connected &&
            (badauth_seen >= 2 || time_reached(verify_deadline))) {
            printf("[wifi] join failed (%s) -- clearing creds, rebooting to AP onboarding\n",
                   badauth_seen >= 2 ? "bad password" : "timeout");
            // Clear provisioning so the next boot comes up in AP + portal.
            config_set_wifi_creds("", "");
            watchdog_update();
            config_save();
            sleep_ms(150); // let UART flush
            watchdog_reboot(0, 0, 0);
            return;
        }

        // Otherwise retry the join (async, non-blocking) on a slow cadence so a
        // flaky router eventually associates without ever stalling the main loop.
        // NOT retried while CYW43_LINK_JOIN/NOIP (mid-join). The ~5s spacing
        // avoids hammering the radio (shared with BT/audio) every tick.
        static absolute_time_t next_retry = {0};
        if (time_reached(next_retry)) {
            next_retry = make_timeout_time_ms(5000);
            wifi_start_join();
        }
    }
}

#endif // ENABLE_WIFI_WOL
