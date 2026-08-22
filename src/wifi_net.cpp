//
// wifi_net.cpp -- onboard CYW43 Wi-Fi (STA) uplink for Wake-on-LAN ONLY.
// Configuration happens over USB (hid_config.cpp); there is no web server,
// no mDNS and no captive portal. Credentials arrive via POST
// /api/wifi_provision on the USB config page.
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
//     so we MUST NOT call lwip_init() here, or lwIP double-inits.
//   - Everything here addresses the STA netif EXPLICITLY (sta_netif()) rather
//     than netif_default -- a habit from the NCM-migration era (another netif
//     owned default then) that stays because it is simply more precise.
//   - The ARP resolve must be a start/poll split serviced from the main loop,
//     never blocked inside the httpd POST callback (see the resolve section).
//

#include "wifi_net.h"

#ifdef ENABLE_WIFI_WOL

#include <cstdio>
#include <cstring>

#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "hardware/watchdog.h"

#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/udp.h"

#include "config.h"

// The CYW43 STA netif, addressed explicitly (see the header note).
static struct netif *sta_netif() { return &cyw43_state.netif[CYW43_ITF_STA]; }
// Unprovisioned: the WiFi half of the radio is parked (no STA netif, no join);
// set in wifi_net_init(). Every LAN path below must bail while this is set.
static bool sta_idle = false;

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
    // Explicit egress netif: a global broadcast routes via netif_default,
    // and pinning the STA netif keeps this correct no matter who owns default
    // (the AP netif does during onboarding, for instance).
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
    if (sta_idle) return false; // no LAN uplink
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
    // No LAN while unprovisioned. (wifi_wol_send_all() re-checks this too.)
    if (sta_idle) return false;
    // Wake every configured target (PC + optional 2nd, e.g. a TV).
    return wifi_wol_send_all();
}

//--------------------------------------------------------------------+
// Link state (for the USB config page's Network tab)
//--------------------------------------------------------------------+

static const char *wifi_state = "off";
static char wifi_state_detail[24] = "";

const char *wifi_net_state(void) {
    static char buf[40];
    if (wifi_state_detail[0]) snprintf(buf, sizeof(buf), "%s %s", wifi_state, wifi_state_detail);
    else snprintf(buf, sizeof(buf), "%s", wifi_state);
    return buf;
}

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

    // DHCP hostname (what the router's client list shows). User-set in config
    // so multiple dongles are distinguishable; config_valid() has already
    // sanitized it to a valid DNS label (and defaulted it if empty).
    const char *hostname = get_config().hostname;
#if LWIP_NETIF_HOSTNAME
    netif_set_hostname(sta_netif(), hostname);
#endif

    // Kick off the join asynchronously and return immediately -- do NOT block
    // boot on it (see wifi_start_join). main() proceeds straight to BT/audio/USB
    // init; wifi_net_task() drives the join to completion and retries on failure.
    wifi_start_join();

    wifi_state = "joining";
    printf("[wifi] STA uplink starting (hostname \"%s\")\n", hostname);
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
    // Defer the reboot a beat so the config page gets its "saved" reply
    // before the watchdog resets us. wifi_net_task() fires it.
    wifi_schedule_reboot(1200);
    return true;
}

bool wifi_reset_provisioning_apply() {
    config_set_wifi_creds("", "");
    watchdog_update();
    if (!config_save()) return false;
    printf("[wifi] WiFi credentials cleared; rebooting with WiFi off\n");
    wifi_schedule_reboot(1200);
    return true;
}

//--------------------------------------------------------------------+
// Init / service
//--------------------------------------------------------------------+

void wifi_net_init() {
    if (!get_config().wifi_provisioned) {
        // No credentials: stay a plain dongle (BT + USB up, WiFi half of the
        // radio idle). Credentials are entered on the USB config page
        // (hid_config.cpp -> POST /api/wifi_provision), which persists them
        // and reboots into STA.
        sta_idle = true;
        wifi_state = "off";
        printf("[wifi] unprovisioned: WiFi idle (set a network on the config page)\n");
        return;
    }
    wifi_sta_init(); // WOL uplink
}

void wifi_net_task() {
    // lwIP timers: nothing else pumps them (RX and the netif are serviced by
    // cyw43_arch_poll()).
    sys_check_timeouts();

    // Provisioning/reset reboots are deferred so the config page gets its
    // reply before the watchdog reset.
    if (reboot_pending && time_reached(reboot_at)) {
        watchdog_reboot(0, 0, 0);
        return;
    }

    // Unprovisioned: no STA netif exists, so there is no link to supervise,
    // no ARP to poll and nothing to retry. (The deferred-reboot check above
    // still runs, which is how provisioning over USB takes effect.)
    if (sta_idle) return;

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
        wifi_state = "up";
        snprintf(wifi_state_detail, sizeof(wifi_state_detail), "%s",
                 ip4addr_ntoa(netif_ip4_addr(sta_netif())));
        if (!reported_ip) {
            printf("[wifi] IP %s (WOL uplink ready)\n",
                   ip4addr_ntoa(netif_ip4_addr(sta_netif())));
            reported_ip = true;
        }
    } else if (link == CYW43_LINK_DOWN || link == CYW43_LINK_FAIL ||
               link == CYW43_LINK_NONET || link == CYW43_LINK_BADAUTH) {
        reported_ip = false;
        wifi_state_detail[0] = '\0';

        // Give up for this boot ONLY if we've never connected AND either the
        // password is clearly wrong (BADAUTH twice) or the budget ran out --
        // stop hammering the shared radio and report it on the config page
        // (credentials are KEPT; the user fixes them over USB or the next
        // boot retries). A device that connected before keeps retrying
        // forever instead, since a real network blip should self-heal.
        static bool gave_up = false;
        if (gave_up) return;
        if (!ever_connected &&
            (badauth_seen >= 2 || time_reached(verify_deadline))) {
            const bool badpw = badauth_seen >= 2;
            printf("[wifi] join failed (%s) -- giving up until next boot\n",
                   badpw ? "bad password" : "timeout");
            wifi_state = badpw ? "failed (bad password)" : "failed (no connection)";
            gave_up = true;
            return;
        }
        wifi_state = ever_connected ? "reconnecting" : "joining";

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
