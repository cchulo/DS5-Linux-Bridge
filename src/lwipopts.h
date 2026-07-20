#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// lwIP runs NO_SYS over the cyw43 WiFi netif; everything is serviced from the
// single main loop (cyw43_arch_poll + wifi_net_task), so no OS/locking is
// needed. The stack is initialised by cyw43_arch_init() (CYW43_LWIP=1). Only
// ENABLE_WIFI_WOL builds compile lwIP at all -- the NCM-era USB transport is
// gone (NCM->WiFi migration phase 4).
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// Footprint is kept DELIBERATELY SMALL. This stack serves one ~5 KB config
// page over a single short-lived HTTP connection; it is NEVER a throughput
// path. lwIP is always up here (no time-share), so every byte of its static
// footprint permanently shrinks the heap shared with BTstack (~40 KB at boot)
// and the Opus codec runtime (~76 KB on controller-connect). Sizes are the
// ones proven on HW (upstream kungaa's WiFi tuning + our phase-3 validation):
// the cyw43 driver allocates each RX frame from PBUF_POOL at full MTU, so a
// small pool means the occasional dropped large inbound frame under load --
// an acceptable trade vs OOM.
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    2400
#define MEMP_NUM_PBUF               5
#define PBUF_POOL_SIZE              6   // ~6 * ~600 B = ~3.5 KB
#define MEMP_NUM_TCP_SEG            14  // must be >= TCP_SND_QUEUELEN (see below)
#define MEMP_NUM_ARP_QUEUE          2
// UDP endpoints: STA (DHCP client, mDNS, transient WOL send) or AP (portal
// DHCP + DNS servers), with headroom.
#define MEMP_NUM_UDP_PCB            6
#define MEMP_NUM_TCP_PCB            5   // active conns + a couple lingering TIME_WAIT
#define MEMP_NUM_TCP_PCB_LISTEN     1   // single httpd listener
#define TCP_MSL                     1000  // ms (default 60000); short TIME_WAIT linger

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1

// IP services. In STA mode the dongle is a DHCP *client* of the home router
// and uses mDNS for discovery (<hostname>.local), so it needs DHCP client +
// IGMP + DNS + the mDNS responder.
#define LWIP_DHCP                   1   // DHCP *client*: lease from the home router
#define LWIP_DNS                    1
#define LWIP_IGMP                   1   // multicast for mDNS on the LAN link
#define LWIP_MDNS_RESPONDER         1
#define LWIP_NUM_NETIF_CLIENT_DATA  1   // mDNS stores per-netif client data
// AP-mode DHCP server unicasts its OFFER/ACK to the offered IP (cyw43 SoftAP
// doesn't reliably flood broadcast to stations), which needs a static ARP entry
// injected for the not-yet-configured client -> requires static ARP support.
#define ETHARP_SUPPORT_STATIC_ENTRIES 1
// Timeout pool. The auto-computed default (LWIP_NUM_SYS_TIMEOUT_INTERNAL)
// under-provisions once a DHCP lease lands: DHCP coarse + fine + T1 + T2
// timers, the ARP table timer, IGMP, mDNS announce/probe, DNS, and TCP
// slow/fast timers can all be armed at once -> "MEMP_SYS_TIMEOUT is empty"
// panic the instant the lease is acquired. Size it explicitly with headroom.
#define MEMP_NUM_SYS_TIMEOUT        12
// Do NOT queue out-of-order segments (lwIP's recommended low-memory setting).
// Upstream HW-captured a deadlock where segments queued behind one lost WiFi
// segment held most of the pbuf pool and collapsed the receive window below
// one MSS -- a permanent stall. With ooseq off, later segments are dropped,
// the window stays open, and the sender retransmits from the gap (go-back-N):
// slower under loss, can't wedge.
#define TCP_QUEUE_OOSEQ             0

// Onboarding AP mode runs a tiny DHCP *server* (lib/portal/dhcpserver.c) so a
// phone can get a lease and reach the captive portal. The server's bound UDP
// socket must see client DISCOVERs, which arrive link-layer-addressed from src
// 0.0.0.0 to port 67; without this, ip4_input drops them and no lease is
// handed out. Harmless in STA mode (the dongle is then a DHCP client; nothing
// is bound to 67).
#define LWIP_IP_ACCEPT_UDP_PORT(p) ((p) == PP_NTOHS(67))

// Small MSS keeps each pbuf-pool buffer small (PBUF_POOL_BUFSIZE tracks MSS),
// so the pool costs little BSS. The ~5 KB page streams across many small
// segments; on the local LAN the extra round-trips are invisible.
#define TCP_MSS                     536
#define TCP_WND                     (4 * TCP_MSS)   // ~2.1 KB receive window
#define TCP_SND_BUF                 (3 * TCP_MSS)   // ~1.6 KB; QUEUELEN ~13, fits SEG=14
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_TCP_KEEPALIVE          1

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

// HTTP server: all content is generated in fs_open_custom / the POST hooks
// (web_api.cpp); the static fsdata table is empty (pico_fsdata.inc).
#define LWIP_HTTPD_CUSTOM_FILES     1
#define LWIP_HTTPD_DYNAMIC_HEADERS  0     // responses carry their own headers
#define LWIP_HTTPD_SUPPORT_POST     1
#define LWIP_HTTPD_SSI              0
#define LWIP_HTTPD_CGI              0
#define HTTPD_FSDATA_FILE           "pico_fsdata.inc"

#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0

#define LWIP_CHKSUM_ALGORITHM       3

#endif /* LWIPOPTS_H */
