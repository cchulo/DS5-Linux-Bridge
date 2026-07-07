//
// See weblog.h. A pico stdio driver whose out_chars appends into a ring
// buffer; /api/log (usb_net.cpp) serves a linearized snapshot. Registered
// alongside the UART driver, so GP0 output is unaffected.
//

#include "weblog.h"

#include <cstdint>

#include "pico/stdio.h"
#include "pico/stdio/driver.h"

namespace {
constexpr uint32_t RING_SIZE = 1024; // power of two; heap is tight (see audio)
char ring[RING_SIZE];
// Total chars ever written; ring index is wpos & (RING_SIZE-1). Races with
// core1 printfs are tolerable for diagnostics (worst case: garbled chars).
volatile uint32_t wpos = 0;

void weblog_out_chars(const char *buf, int len) {
    const uint32_t p = wpos;
    for (int i = 0; i < len; i++) {
        ring[(p + (uint32_t) i) & (RING_SIZE - 1)] = buf[i];
    }
    wpos = p + (uint32_t) len;
}
} // namespace

static stdio_driver_t weblog_driver = {};

void weblog_init() {
    weblog_driver.out_chars = weblog_out_chars;
    stdio_set_driver_enabled(&weblog_driver, true);
}

int weblog_snapshot(char *out, int cap) {
    if (cap <= 0) return 0;
    const uint32_t end = wpos;
    const uint32_t avail = end < RING_SIZE ? end : RING_SIZE;
    int n = (int) avail;
    if (n > cap - 1) n = cap - 1;
    const uint32_t start = end - (uint32_t) n;
    for (int i = 0; i < n; i++) {
        out[i] = ring[(start + (uint32_t) i) & (RING_SIZE - 1)];
    }
    out[n] = '\0';
    return n;
}
