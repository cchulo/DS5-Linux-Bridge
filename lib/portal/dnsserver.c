/*
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Vendored from raspberrypi/pico-examples (pico_w/wifi/access_point). See
// dnsserver.h. Unmodified captive-portal DNS server: every A-query is answered
// with the AP's own IP.

#include <stdio.h>
#include <string.h>

#include "dnsserver.h"
#include "lwip/udp.h"

#define PORT_DNS_SERVER 53
#define DUMP_DATA 0

#define DEBUG_printf(...)
//#define DEBUG_printf printf

typedef struct dns_header_t_ {
    uint16_t id;
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_record_count;
    uint16_t authority_record_count;
    uint16_t additional_record_count;
} dns_header_t;

#define MAX_DNS_MSG_SIZE 300

static int dns_socket_new_dgram(struct udp_pcb **udp, void *cb_data, udp_recv_fn cb_udp_recv) {
    *udp = udp_new();
    if (*udp == NULL) {
        return -1;
    }
    udp_recv(*udp, cb_udp_recv, (void *)cb_data);
    return ERR_OK;
}

static void dns_socket_free(struct udp_pcb **udp) {
    if (*udp != NULL) {
        udp_remove(*udp);
        *udp = NULL;
    }
}

static int dns_socket_bind(struct udp_pcb **udp, uint32_t ip, uint16_t port) {
    ip_addr_t addr;
    IP4_ADDR(&addr, ip >> 24 & 0xff, ip >> 16 & 0xff, ip >> 8 & 0xff, ip & 0xff);
    err_t err = udp_bind(*udp, &addr, port);
    if (err != ERR_OK) {
        DEBUG_printf("dns failed to bind to port %u: %d", port, err);
        assert(false);
    }
    return err;
}

static int dns_socket_sendto(struct udp_pcb **udp, const void *buf, size_t len, const ip_addr_t *dest, uint16_t port) {
    if (len > 0xffff) {
        len = 0xffff;
    }

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (p == NULL) {
        DEBUG_printf("DNS: Failed to send message out of memory\n");
        return -1;
    }

    memcpy(p->payload, buf, len);
    err_t err = udp_sendto(*udp, p, dest, port);

    pbuf_free(p);

    if (err != ERR_OK) {
        DEBUG_printf("DNS: Failed to send message %d\n", err);
        return err;
    }

    return len;
}

static void dns_server_process(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *src_addr, u16_t src_port) {
    dns_server_t *d = arg;
    DEBUG_printf("dns_server_process %u\n", p->tot_len);
    (void)upcb;

    uint8_t dns_msg[MAX_DNS_MSG_SIZE];
    dns_header_t *dns_hdr = (dns_header_t*)dns_msg;

    size_t msg_len = pbuf_copy_partial(p, dns_msg, sizeof(dns_msg), 0);
    if (msg_len < sizeof(dns_header_t)) {
        goto ignore_request;
    }

#if DUMP_DATA
    for (size_t i = 0; i < msg_len; i++) {
        printf("%02x ", dns_msg[i]);
    }
    printf("\n");
#endif

    uint16_t flags = lwip_ntohs(dns_hdr->flags);
    uint16_t question_count = lwip_ntohs(dns_hdr->question_count);

    DEBUG_printf("len %d\n", msg_len);
    DEBUG_printf("dns flags 0x%x\n", flags);
    DEBUG_printf("dns question count 0x%x\n", question_count);

    // flags from rfc1035
    // QR | Opcode  | AA | TC | RD | RA |   Z    | RCODE
    //  0 | 0 0 0 0 |  0 |  0 |  0 |  0 | 0  0  0 | 0 0 0 0

    // Check QR indicates a query
    if (((flags >> 15) & 0x1) != 0) {
        DEBUG_printf("Ignoring non-query\n");
        goto ignore_request;
    }

    // Check opcode
    if (((flags >> 11) & 0xf) != 0) {
        DEBUG_printf("Ignoring non-standard query\n");
        goto ignore_request;
    }

    // Check question count
    if (question_count < 1) {
        DEBUG_printf("Invalid question count\n");
        goto ignore_request;
    }

    // Print the question
    DEBUG_printf("question: ");
    const uint8_t *question_ptr_start = dns_msg + sizeof(dns_header_t);
    const uint8_t *question_ptr_end = dns_msg + msg_len;
    const uint8_t *question_ptr = question_ptr_start;
    while (question_ptr < question_ptr_end) {
        if (*question_ptr == 0) {
            question_ptr++;
            break;
        } else {
            if (question_ptr > question_ptr_start) {
                DEBUG_printf(".");
            }
            int label_len = *question_ptr++;
            if (label_len > 63) {
                DEBUG_printf("Invalid label\n");
                goto ignore_request;
            }
            DEBUG_printf("%.*s", label_len, question_ptr);
            question_ptr += label_len;
        }
    }
    DEBUG_printf("\n");

    // Check question length
    if (question_ptr - question_ptr_start > 255) {
        DEBUG_printf("Invalid question length\n");
        goto ignore_request;
    }

    // Skip QNAME and QTYPE and QCLASS offsets
    if (question_ptr + 4 > question_ptr_end) {
        DEBUG_printf("Invalid question length\n");
        goto ignore_request;
    }
    question_ptr += 4;

    // Generate answer
    uint8_t *answer_ptr = dns_msg + (question_ptr - dns_msg);
    *answer_ptr++ = 0xc0; // pointer
    *answer_ptr++ = question_ptr_start - dns_msg; // pointer to question

    *answer_ptr++ = 0;
    *answer_ptr++ = 1; // host address

    *answer_ptr++ = 0;
    *answer_ptr++ = 1; // Internet class

    *answer_ptr++ = 0;
    *answer_ptr++ = 0;
    *answer_ptr++ = 0;
    *answer_ptr++ = 60; // ttl 60s

    *answer_ptr++ = 0;
    *answer_ptr++ = 4; // length
    memcpy(answer_ptr, &ip4_addr_get_u32(ip_2_ip4(&d->ip)), 4); // use our address
    answer_ptr += 4;

    dns_hdr->flags = lwip_htons(
                0x1 << 15 | // QR = response
                0x1 << 10 | // AA = authoritative
                0x1 << 7);   // RA = authenticated
    dns_hdr->question_count = lwip_htons(1);
    dns_hdr->answer_record_count = lwip_htons(1);
    dns_hdr->authority_record_count = 0;
    dns_hdr->additional_record_count = 0;

    // Send the reply
    DEBUG_printf("Sending %d byte reply to %s:%d\n", answer_ptr - dns_msg, ipaddr_ntoa(src_addr), src_port);
    dns_socket_sendto(&d->udp, &dns_msg, answer_ptr - dns_msg, src_addr, src_port);

ignore_request:
    pbuf_free(p);
}

void dns_server_init(dns_server_t *d, ip_addr_t *ip) {
    if (dns_socket_new_dgram(&d->udp, d, dns_server_process) != 0) {
        DEBUG_printf("dns server failed to start\n");
        return;
    }
    dns_socket_bind(&d->udp, 0, PORT_DNS_SERVER);
    ip_addr_copy(d->ip, *ip);
}

void dns_server_deinit(dns_server_t *d) {
    dns_socket_free(&d->udp);
}
