//
// hid_config.cpp -- USB HID config tunnel. Protocol in hid_config.h.
//
// Commands execute inside TinyUSB's SET_REPORT callback (tud_task, main
// loop, core0), so the web_api handlers need no locking. The reply for the next GET 0x81 is
// prepared here and only copied out in the GET callback.
//

#include "hid_config.h"

#include <cstdio>
#include <cstring>

#include "web_api.h"

namespace {
constexpr uint8_t MAGIC[4] = {'D', 'S', '5', 'B'};
constexpr size_t HDR = 8;
constexpr size_t PAYLOAD = HID_CONFIG_REPORT_LEN - HDR; // 55
constexpr size_t ROUTE_CAP = 48;
// Must hold the whole /api/config form body (~550 B worst case).
constexpr size_t POST_CAP = 768;

enum : uint8_t {
    CMD_PING = 1,
    CMD_GET_BEGIN = 2,
    CMD_GET_CHUNK = 3,
    CMD_POST_BEGIN = 4,
    CMD_POST_DATA = 5,
    CMD_POST_END = 6,
};

bool     armed = false;
uint8_t  reply[HID_CONFIG_REPORT_LEN];

// GET response body (rendered by web_api_get) served out in chunks.
char     body[WEB_API_RESP_CAP];
int      body_len = 0;

// In-flight POST.
char     post_route[ROUTE_CAP];
char     post_buf[POST_CAP];
size_t   post_len = 0;
bool     post_open = false;

void copy_route(char *dst, const uint8_t *src, size_t n) {
    if (n >= ROUTE_CAP) n = ROUTE_CAP - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Render `route` into body[] and put (len, status) in the reply payload.
void begin_body(const char *route) {
    int status = 0;
    body_len = web_api_get(route, body, sizeof(body), &status);
    if (body_len < 0) {
        body_len = 0;
        reply[7] = HID_CFG_ST_BAD_ROUTE;
        return;
    }
    reply[8]  = (uint8_t) (body_len & 0xFF);
    reply[9]  = (uint8_t) (body_len >> 8);
    reply[10] = (uint8_t) (status & 0xFF);
    reply[11] = (uint8_t) (status >> 8);
    reply[6]  = 4;
}
} // namespace

bool hid_config_set_report(const uint8_t *buf, uint16_t len) {
    if (len < HDR || memcmp(buf, MAGIC, 4) != 0) {
        armed = false; // a real 0x80 command: the next 0x81 is the pad's
        return false;
    }
    armed = true;
    const uint8_t cmd = buf[4];
    const uint8_t idx = buf[5];
    size_t plen = buf[6];
    if (plen > PAYLOAD) plen = PAYLOAD;
    if (plen > (size_t) (len - HDR)) plen = len - HDR;
    const uint8_t *pl = buf + HDR;

    memset(reply, 0, sizeof(reply));
    memcpy(reply, MAGIC, 4);
    reply[4] = cmd;
    reply[5] = idx;
    reply[6] = 0;
    reply[7] = HID_CFG_ST_OK;

    switch (cmd) {
        case CMD_PING: {
            reply[8] = HID_CONFIG_PROTO_VERSION;
            const int n = snprintf((char *) reply + 9, PAYLOAD - 1, "%s",
                                   PICO_PROGRAM_VERSION_STRING);
            reply[6] = (uint8_t) (1 + (n < (int) PAYLOAD - 1 ? n : (int) PAYLOAD - 1));
            break;
        }
        case CMD_GET_BEGIN: {
            char route[ROUTE_CAP];
            copy_route(route, pl, plen);
            begin_body(route);
            break;
        }
        case CMD_GET_CHUNK: {
            const size_t off = (size_t) idx * PAYLOAD;
            if (off >= (size_t) body_len) break; // len 0 = past the end
            size_t n = (size_t) body_len - off;
            if (n > PAYLOAD) n = PAYLOAD;
            memcpy(reply + HDR, body + off, n);
            reply[6] = (uint8_t) n;
            break;
        }
        case CMD_POST_BEGIN:
            copy_route(post_route, pl, plen);
            post_len = 0;
            post_open = true;
            break;
        case CMD_POST_DATA:
            if (!post_open) {
                reply[7] = HID_CFG_ST_NO_SESSION;
            } else if (post_len + plen >= POST_CAP) {
                post_open = false;
                reply[7] = HID_CFG_ST_TOO_BIG;
            } else {
                memcpy(post_buf + post_len, pl, plen);
                post_len += plen;
            }
            break;
        case CMD_POST_END: {
            if (!post_open) {
                reply[7] = HID_CFG_ST_NO_SESSION;
                break;
            }
            post_open = false;
            post_buf[post_len] = '\0';
            const char *result = web_api_post(post_route, post_buf);
            if (!result) {
                reply[7] = HID_CFG_ST_BAD_ROUTE;
                break;
            }
            begin_body(result);
            break;
        }
        default:
            reply[7] = HID_CFG_ST_BAD_CMD;
            break;
    }
    return true;
}

bool hid_config_armed(void) { return armed; }

uint16_t hid_config_get_report(uint8_t *buf, uint16_t reqlen) {
    uint16_t n = reqlen < HID_CONFIG_REPORT_LEN ? reqlen : HID_CONFIG_REPORT_LEN;
    memcpy(buf, reply, n);
    return n;
}
