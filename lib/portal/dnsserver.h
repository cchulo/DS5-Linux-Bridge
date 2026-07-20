/*
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
//
// Vendored from raspberrypi/pico-examples (pico_w/wifi/access_point). A tiny DNS
// *server* for the captive portal: answers EVERY A-query with the dongle's own
// AP IP so any hostname the phone tries (connectivitycheck, captive.apple.com,
// ...) resolves to us and the OS pops the "Sign in to network" sheet. Used ONLY
// by the WiFi-WOL onboarding AP path (src/wifi_net.cpp). Unmodified except for
// this note.
//
#ifndef _DNSSERVER_H_
#define _DNSSERVER_H_

#include "lwip/ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _dns_server_t {
    ip_addr_t ip;
    struct udp_pcb *udp;
} dns_server_t;

void dns_server_init(dns_server_t *d, ip_addr_t *ip);
void dns_server_deinit(dns_server_t *d);

#ifdef __cplusplus
}
#endif

#endif // _DNSSERVER_H_
