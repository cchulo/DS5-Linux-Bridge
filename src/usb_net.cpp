//
// usb_net.cpp -- CDC-NCM USB network LINK layer: TinyUSB net glue + netif +
// DHCP server. A small lwIP stack runs over it (NO_SYS, serviced from the
// main loop); the web content itself (config page, JSON API, POST handlers)
// lives in web_api.cpp, shared with the WiFi transport (NCM->WiFi migration
// phase 2).
//
//   - dhserver (TinyUSB lib/networking) hands the host an address
//   - the DHCP offer deliberately carries no gateway and no DNS server so the
//     host never tries to route internet traffic or DNS lookups through us
//
// Adapted from the GPLv3 PC-wake-dongle project (same author); the BLE
// scan/device-list config was replaced with the DS5 firmware settings.
//

#include "usb_net.h"

#ifdef ENABLE_WEBCONFIG

#include <cstdio>
#include <cstring>

#include "tusb.h"

#include "dhserver.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"

#include "pico/time.h"
#include "pico/unique_id.h"

#include "bt.h"  // bt_connected_count(): NCM bounce never yanks a live pad
#include "usb.h" // usb_request_rebind / usb_variant_swap_in_progress
#include "config.h"
#include "web_api.h"

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
        // HARD GUARD: if usb_net_init() never ran (AP onboarding skips it),
        // netif_data.input is NULL and calling it is a jump to address 0 --
        // hard fault -> watchdog -> bootloop that can also thrash the host's
        // USB stack (HW-observed on SteamOS). AP mode now keeps the device
        // off the bus entirely (main.cpp), so this is defense in depth: drop
        // the frame and re-arm delivery.
        if (netif_data.input == NULL) {
            tud_network_recv_renew();
            return true;
        }
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

#ifndef ENABLE_WIFI_WOL
    // WiFi builds run CYW43_LWIP=1: cyw43_arch_init() already ran lwip_init()
    // (and a second init would corrupt the running stack). NCM-only builds
    // still own the stack and must init it here.
    lwip_init();
#endif

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
    web_api_init(); // idempotent; wifi_net_init() may already have started httpd

    printf("[NET] config UI at http://%s/\n", ip4addr_ntoa(&ipaddr));
}

void usb_net_task() {
    sys_check_timeouts();

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
