//
// web_api.h -- the on-device web UI: lwIP httpd content (config page, JSON
// API, captive portal) + POST handlers, served over the CYW43 WiFi netif
// (wifi_net.cpp). Factored out of the retired USB-NCM transport (migration
// phase 2) following upstream kungaa's web_api split; the NCM link layer was
// deleted in phase 4.
//

#ifndef DS5_BRIDGE_WEB_API_H
#define DS5_BRIDGE_WEB_API_H

#include <stddef.h>

#ifdef ENABLE_WIFI_WOL

// Start lwIP's httpd. Idempotent; called from wifi_net_init() (AP and STA).
void web_api_init();

// Service deferred web actions (currently: the POST /api/reboot BOOTSEL
// reboot, delayed ~500ms so the HTTP response reaches the browser first).
// Call every main-loop iteration; cheap no-op otherwise.
void web_api_task();

// --- Transport-agnostic route layer (shared by lwIP httpd and the USB HID
// config tunnel in hid_config.cpp) ---
// Largest route body (/api/log: frozen boot KB + marker + rolling tail).
#define WEB_API_RESP_CAP 2304
// Render a GET route's body into `out` (no HTTP headers). Returns the body
// length (clamped to cap), or -1 for an unknown route. *status is the
// HTTP-style outcome (200 / 404 / 409 / 500).
int web_api_get(const char *name, char *out, size_t cap, int *status);
// Apply a form-encoded POST body to a route (the body is parsed in place).
// Returns the synthetic result route to GET for the outcome (e.g.
// "/api/config" on success, "/api/save-failed"), or nullptr if `uri` is not
// a POST route.
const char *web_api_post(const char *uri, char *body);

#else
static inline void web_api_init() {}
static inline void web_api_task() {}
#endif

#endif // DS5_BRIDGE_WEB_API_H
