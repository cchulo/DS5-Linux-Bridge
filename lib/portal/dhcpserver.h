/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (C) 2018-2019 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
//
// Vendored from raspberrypi/pico-examples (pico_w/wifi/access_point), itself
// from MicroPython. A tiny DHCP *server* for AP mode: hands a single private
// lease to a connecting client so a phone/laptop can reach the captive portal.
// Used ONLY by the WiFi-WOL onboarding AP path (src/wifi_net.cpp).
//
// MODIFIED from upstream: DHCPS_BASE_IP/DHCPS_MAX_IP are re-homed to the DS5 AP's
// 10.55.55.104/29 subnet (gateway .105, pool .106-.110) instead of the upstream
// example's /24 (.16+, 8 leases). Leases build off the server IP's first three
// octets + (DHCPS_BASE_IP + slot) as the last octet (see dhcpserver.c), so the
// last octet must fall inside the /29 host range. The .c logic is unchanged.
//
#ifndef _DHCPSERVER_H_
#define _DHCPSERVER_H_

#include "lwip/ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tuned to the DS5 AP /29 (10.55.55.104/29, gw .105): leases .106-.110.
#define DHCPS_BASE_IP (106)
#define DHCPS_MAX_IP (5)

typedef struct _dhcp_server_lease_t {
    uint8_t mac[6];
    uint16_t expiry;
} dhcp_server_lease_t;

typedef struct _dhcp_server_t {
    ip_addr_t ip;
    ip_addr_t nm;
    dhcp_server_lease_t lease[DHCPS_MAX_IP];
    struct udp_pcb *udp;
} dhcp_server_t;

void dhcp_server_init(dhcp_server_t *d, ip_addr_t *ip, ip_addr_t *nm);
void dhcp_server_deinit(dhcp_server_t *d);

#ifdef __cplusplus
}
#endif

#endif // _DHCPSERVER_H_
