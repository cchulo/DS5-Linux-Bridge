//
// RAM ring buffer that captures stdio (all printf diagnostics) so the
// firmware log can be read from a browser at /api/log -- no UART adapter
// needed. Keeps the most recent ~4 KB; older output rolls off.
//

#ifndef DS5_BRIDGE_WEBLOG_H
#define DS5_BRIDGE_WEBLOG_H

// Register the capture driver with pico stdio. Call once, right after
// board_init() so boot-time prints are included.
void weblog_init();

// Copy the most recent log contents (oldest first) into out, NUL-terminated.
// Returns bytes written (excluding the NUL).
int weblog_snapshot(char *out, int cap);

#endif // DS5_BRIDGE_WEBLOG_H
