//
// wifi_net.h -- onboard CYW43 Wi-Fi (STA mode) uplink for Wake-on-LAN
// (ENABLE_WIFI_WOL). WiFi exists ONLY to carry magic packets: configuration
// happens over USB (hid_config.h), there is no web server, mDNS or captive
// portal. lwIP is initialised by cyw43_arch_init() (CYW43_LWIP=1).
//
// Needs NO extra hardware: the radio is the same CYW43 chip BTstack already
// drives, and its WiFi firmware is already linked into the image. BT and WiFi
// share one async_context (the poll context), both pumped by cyw43_arch_poll()
// in the main loop -- watch that radio sharing when changing anything
// audio-adjacent.
//

#ifndef DS5_BRIDGE_WIFI_NET_H
#define DS5_BRIDGE_WIFI_NET_H

#include <cstdint>

#ifdef ENABLE_WIFI_WOL
// Bring up the WiFi uplink: with credentials in flash, join the home WLAN
// (DHCP client); without, leave the WiFi half of the radio idle -- the dongle
// still boots fully (BT + USB). Call once after cyw43_arch_init() and
// config_load() (main.cpp). Do NOT lwip_init() (cyw43_arch_init did).
void wifi_net_init();

// Call every main-loop iteration: pumps lwIP timers + the ARP-resolve poll +
// join supervision. (RX and the netif are pumped by cyw43_arch_poll() already.)
void wifi_net_task();

// Human-readable uplink state for the config page: "off", "joining",
// "reconnecting", "up <ip>", "failed (bad password)", "failed (no connection)".
const char *wifi_net_state(void);

// Send a Wake-on-LAN magic packet to `mac` (6 bytes) as a broadcast UDP datagram
// to 255.255.255.255:9, out the STA netif. Returns true if queued.
bool wifi_wol_send(const uint8_t mac[6]);

// Fire a magic packet at every configured (non-zero) WOL target. Returns true
// if at least one packet was sent. No-op while unprovisioned.
bool wifi_wol_send_all(void);

// Kick off non-blocking ARP resolution of `ip` (4 bytes, local subnet).
// Start/poll split: the request handler must never pump lwIP itself.
void wifi_resolve_mac_start(const uint8_t ip[4]);

// Poll the result: 0 = pending, 1 = resolved (out_mac filled), -1 = failed.
int wifi_resolve_mac_poll_result(uint8_t out_mac[6]);

// Persist ssid/psk to flash and schedule a reboot into STA mode. Returns true
// if accepted (non-empty ssid, psk empty or 8..63 chars).
bool wifi_provision_apply(const char *ssid, const char *psk);

// Clear saved WiFi credentials, persist, and schedule a reboot (next boot:
// WiFi idle).
bool wifi_reset_provisioning_apply();
#else
static inline void wifi_net_init() {}
static inline void wifi_net_task() {}
static inline const char *wifi_net_state(void) { return "unavailable (firmware built without WiFi)"; }
static inline bool wifi_wol_send(const uint8_t *) { return false; }
static inline bool wifi_wol_send_all(void) { return false; }
static inline void wifi_resolve_mac_start(const uint8_t *) {}
static inline int wifi_resolve_mac_poll_result(uint8_t *) { return -1; }
static inline bool wifi_provision_apply(const char *, const char *) { return false; }
static inline bool wifi_reset_provisioning_apply() { return false; }
#endif // ENABLE_WIFI_WOL

#endif // DS5_BRIDGE_WIFI_NET_H
