//
// web_api.h -- the on-device web UI: lwIP httpd content (config page, JSON
// API, captive portal) + POST handlers. Transport-agnostic: serves identically
// over the USB-NCM netif (usb_net.cpp) and the CYW43 WiFi netif
// (wifi_net.cpp); both transports may be live on the one lwIP stack at once
// (the NCM->WiFi migration's bring-up state).
//
// Factored out of usb_net.cpp (phase 2 of the migration) following upstream
// kungaa's web_api split, so the NCM link layer can be deleted later without
// touching the web UI. Compiled when either transport exists.
//

#ifndef DS5_BRIDGE_WEB_API_H
#define DS5_BRIDGE_WEB_API_H

#if defined(ENABLE_WEBCONFIG) || defined(ENABLE_WIFI_WOL)

// Start lwIP's httpd. Idempotent: with both transports enabled each one's
// init path calls this (usb_net_init, wifi_net_init); only the first call
// starts the server -- there is one lwIP stack and one listener regardless.
void web_api_init();

// Service deferred web actions (currently: the POST /api/reboot BOOTSEL
// reboot, delayed ~500ms so the HTTP response reaches the browser first).
// Call every main-loop iteration; cheap no-op otherwise.
void web_api_task();

#else
static inline void web_api_init() {}
static inline void web_api_task() {}
#endif

#endif // DS5_BRIDGE_WEB_API_H
