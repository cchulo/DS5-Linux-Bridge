//
// hid_config.h -- USB HID config tunnel ("WebHID config page").
//
// The config UI talks to the dongle over the gamepad's OWN HID interface
// (slot 0, TinyUSB HID instance 0) using feature reports 0x80 (host -> dongle
// command) and 0x81 (dongle -> host reply). A real DualSense carries exactly
// this pair for its factory/test command channel, so the HID report
// descriptor stays byte-identical to real hardware: no vendor report IDs, no
// extra interface, no network. A command is only ours when its first four
// bytes are the magic "DS5B"; anything else on 0x80/0x81 falls through to the
// normal pass-through to the controller.
//
// Wire format (63 data bytes after the report ID, both directions):
//   [0..3]  'D','S','5','B'      magic
//   [4]     cmd                  (request) / cmd echoed (reply)
//   [5]     idx                  chunk index (request) / echoed (reply)
//   [6]     len                  payload bytes that follow (0..55)
//   [7]     reserved (request)   / status (reply; 0 = OK, see HID_CFG_ST_*)
//   [8..62] payload              (55 bytes)
//
// Commands:
//   PING        (1)  -> reply payload: [proto version][firmware version str]
//   GET_BEGIN   (2)  payload = route ("/api/config"). Renders the route via
//                    web_api_get(); reply payload: u16 body length, u16 HTTP
//                    status (little-endian).
//   GET_CHUNK   (3)  idx = chunk number -> reply payload: body[idx*55 ..].
//   POST_BEGIN  (4)  payload = route; opens a body buffer.
//   POST_DATA   (5)  idx = chunk number, payload appended to the body.
//   POST_END    (6)  dispatches via web_api_post(); the reply is exactly a
//                    GET_BEGIN of the result route (so GET_CHUNK then reads
//                    the outcome body: JSON on success, a message on error).
//
// So the page does HTTP-shaped GET/POST with no HTTP: every /api/* route and
// form body is identical to the WiFi version, and any host program that can
// send feature reports (WebHID, hidapi, an Electron app) can drive it.
//

#ifndef DS5_BRIDGE_HID_CONFIG_H
#define DS5_BRIDGE_HID_CONFIG_H

#include <stdint.h>

#define HID_CONFIG_REPORT_OUT 0x80 // feature report: host -> dongle command
#define HID_CONFIG_REPORT_IN  0x81 // feature report: dongle -> host reply
#define HID_CONFIG_REPORT_LEN 63   // data bytes after the report ID
#define HID_CONFIG_PROTO_VERSION 1

#define HID_CFG_ST_OK         0
#define HID_CFG_ST_BAD_CMD    1
#define HID_CFG_ST_BAD_ROUTE  2
#define HID_CFG_ST_NO_SESSION 3
#define HID_CFG_ST_TOO_BIG    4

// SET_REPORT 0x80 on slot 0's interface. Returns true if the report was a
// tunnel command (magic matched) and has been consumed; false = not ours,
// caller passes it through to the controller as before. `buf`/`len` exclude
// the report ID (TinyUSB strips it).
bool hid_config_set_report(const uint8_t *buf, uint16_t len);
// True while the last 0x80 SET was a tunnel command, i.e. the next GET 0x81
// belongs to us rather than to the controller's pass-through.
bool hid_config_armed(void);
// GET_REPORT 0x81: copy the prepared reply. Returns bytes written.
uint16_t hid_config_get_report(uint8_t *buf, uint16_t reqlen);

#endif // DS5_BRIDGE_HID_CONFIG_H
