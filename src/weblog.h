//
// RAM capture of stdio (all printf diagnostics) so the firmware log can be
// read from a browser at /api/log -- no UART adapter needed. Two sections:
// the first KB of output is frozen forever (boot diagnostics survive hours
// of HCI chatter), and a rolling ring keeps the most recent KB.
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
