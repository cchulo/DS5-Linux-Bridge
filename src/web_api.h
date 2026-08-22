//
// web_api.h -- the on-device web UI: lwIP httpd content (config page, JSON
// API, captive portal) + POST handlers, served over the CYW43 WiFi netif
// (wifi_net.cpp). Factored out of the retired USB-NCM transport (migration
// phase 2) following upstream kungaa's web_api split; the NCM link layer was
// deleted in phase 4.
//

#ifndef DS5_BRIDGE_WEB_API_H
#define DS5_BRIDGE_WEB_API_H

#ifdef ENABLE_WIFI_WOL

// Start lwIP's httpd. Idempotent; called from wifi_net_init() (AP and STA).
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
