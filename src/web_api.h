//
// web_api.h -- the adapter's configuration API: JSON GET routes + form POST
// handlers, transport-agnostic. Driven over USB by the HID config tunnel
// (hid_config.h); the page is web/index.html. Route shapes are the ones the
// retired WiFi web page used, so existing clients keep working.
//

#ifndef DS5_BRIDGE_WEB_API_H
#define DS5_BRIDGE_WEB_API_H

#include <stddef.h>

// Service deferred actions (currently: the POST /api/reboot BOOTSEL reboot,
// delayed ~500ms so the reply reaches the config page first). Call every
// main-loop iteration; cheap no-op otherwise.
void web_api_task();

// Largest route body (/api/log: frozen boot KB + marker + rolling tail).
#define WEB_API_RESP_CAP 2304
// Render a GET route's body into `out`. Returns the body length (clamped to
// cap), or -1 for an unknown route. *status is the HTTP-style outcome
// (200 / 404 / 409 / 500).
int web_api_get(const char *name, char *out, size_t cap, int *status);
// Apply a form-encoded POST body to a route (the body is parsed in place).
// Returns the synthetic result route to GET for the outcome (e.g.
// "/api/config" on success, "/api/save-failed"), or nullptr if `uri` is not
// a POST route.
const char *web_api_post(const char *uri, char *body);

#endif // DS5_BRIDGE_WEB_API_H
