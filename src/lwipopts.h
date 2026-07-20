#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// lwIP runs NO_SYS; everything is serviced from the single main loop, so no
// OS/locking is needed. Transports: the TinyUSB NCM interface (usb_net_task)
// and -- in ENABLE_WIFI_WOL builds, during the NCM->WiFi migration -- the cyw43
// WiFi netif as well (cyw43_arch_poll + wifi_net_task). In WiFi builds the
// stack is initialised by cyw43_arch_init() (CYW43_LWIP=1), not usb_net.
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// Footprint is kept DELIBERATELY SMALL. This stack serves one ~5 KB config page
// over a single short-lived HTTP connection; it is NEVER a throughput path.
// lwIP is always up here (no time-share), so every byte of its static footprint
// permanently shrinks the heap shared with BTstack (~40 KB at boot) and the
// Opus codec runtime (~76 KB on controller-connect). The earlier web-scale
// sizing (MEM_SIZE 8000, PBUF_POOL 8 x 1460 B, 8xMSS windows) cost ~21 KB of
// BSS and OOM-panicked Opus the moment a controller connected. These values are
// the minimum that still streams the page: a small MSS keeps each pbuf small,
// and the extra round-trips are free on both the USB link and the local LAN.
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#ifdef ENABLE_WIFI_WOL
// WiFi tuning (values proven on HW by upstream kungaa): the cyw43 driver
// allocates each RX frame from PBUF_POOL at full MTU, and the STA feature set
// (DHCP client, mDNS, ARP) needs a little more arena than NCM alone. Upstream's
// first pass (MEM_SIZE 6000, PBUF_POOL 12) OOM-panicked against the BTstack +
// Opus heap; these are the sizes that survived. A small pool means the
// occasional dropped large inbound frame under load -- acceptable vs OOM.
#define MEM_SIZE                    2400
#define MEMP_NUM_PBUF               5
#define PBUF_POOL_SIZE              6   // ~6 * ~600 B = ~3.5 KB
#else
#define MEM_SIZE                    1600
#define MEMP_NUM_PBUF               4
#define PBUF_POOL_SIZE              4   // 4 * ~600 B (small MSS) ~= 2.4 KB
#endif
#define MEMP_NUM_TCP_SEG            14  // must be >= TCP_SND_QUEUELEN (see below)
#define MEMP_NUM_ARP_QUEUE          2
#ifdef ENABLE_WIFI_WOL
// Dual-transport UDP endpoints exceed either fork alone: NCM dhserver + (STA:
// DHCP client, mDNS, transient WOL send | AP: portal DHCP + DNS servers).
#define MEMP_NUM_UDP_PCB            6
#else
#define MEMP_NUM_UDP_PCB            3
#endif
#define MEMP_NUM_TCP_PCB            5   // active conns + a couple lingering TIME_WAIT
#define MEMP_NUM_TCP_PCB_LISTEN     1   // single httpd listener
#define TCP_MSL                     1000  // ms (default 60000); short TIME_WAIT linger

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1

#ifdef ENABLE_WIFI_WOL
// IP services for the WiFi transport. In STA mode the dongle is a DHCP *client*
// of the home router and uses mDNS for discovery (<hostname>.local), so it
// needs DHCP client + IGMP + DNS + the mDNS responder.
//
// CAUTION (migration): the NCM-era note below records that enabling IGMP once
// faulted the NCM setup into a watchdog reboot loop. mDNS is registered ONLY on
// the STA netif (wifi_net.cpp) and the cyw43 netif carries proper multicast
// support, but the NCM+IGMP combination is exactly what the dual-transport
// bring-up (plan Phase 3) must verify on hardware.
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
#else
#define LWIP_DHCP                   0   // we are the DHCP *server* (dhserver.c, raw UDP)
#define LWIP_DNS                    0   // deliberately no DNS: never hijack host lookups
#define LWIP_IGMP                   0   // no multicast: mDNS removed (never resolved here, see below)
#endif

// Let ip4_input accept link-layer-addressed packets (src 0.0.0.0) destined for
// UDP port 67: required for a DHCP *server* to see client DISCOVERs -- both the
// NCM transport's dhserver and the WiFi onboarding portal's dhcpserver
// (lib/portal). Harmless for the STA DHCP client (nothing is bound to 67 then).
#define LWIP_IP_ACCEPT_UDP_PORT(p) ((p) == PP_NTOHS(67))

// Small MSS keeps each pbuf-pool buffer small (PBUF_POOL_BUFSIZE tracks MSS), so
// the pool costs little BSS. The ~5 KB page streams across many small segments;
// over the low-latency USB link (or the local LAN) the extra round-trips are
// invisible.
#define TCP_MSS                     536
#define TCP_WND                     (4 * TCP_MSS)   // ~2.1 KB receive window
#define TCP_SND_BUF                 (3 * TCP_MSS)   // ~1.6 KB; QUEUELEN ~13, fits SEG=14
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_TCP_KEEPALIVE          1

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

// NCM-era mDNS note (still true for the NCM netif): without NETIF_FLAG_IGMP it
// could never join the multicast group, so ds5config.local never resolved here
// -- and enabling IGMP faulted this NCM setup into a watchdog reboot loop.
// Users reach the NCM page by IP (http://10.55.55.105/). The WiFi transport
// registers mDNS on the STA netif only (see the CAUTION above).

// HTTP server: all content is generated in fs_open_custom / the POST hooks
// (usb_net.cpp); the static fsdata table is empty (pico_fsdata.inc).
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
