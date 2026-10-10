#ifndef MASTER_H
#define MASTER_H

/* The test harness: the slave on the test HAL (port/test), and the TMP-v2
 * master on the other end of the wire, building the Ethernet frames it
 * carries. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "hdx_hal.h"
#include "hdx_hal_test.h"
#include "link485_config.h"
#include "net.h"
#include "tmp_v2.h"

#define CHECK(condition) \
    do { if (!(condition)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; } } while (0)

#define MASTER_IP     NET_IPV4(10, 55, 0, 1)
#define UNIT_IP       NET_IPV4(10, 55, 0, 3)
#define NETMASK       NET_IPV4(255, 255, 255, 0)
#define UNIT_MAC_INIT { 0x02, 0x56, 0x45, 0x44, 0x43, 0x01 }
#define TEST_UDP_PORT 5000u     /* a UDP echo in test_link */
#define TEST_TCP_PORT 5001u     /* a TCP listener in test_link */

extern const uint8_t unit_mac[6];
extern const uint8_t master_mac[6];

/* The unit's stack, on the link485 MAC driver; master_unit_init() gives it
 * the configured addresses. */
extern net_t net;
net_err_t master_unit_init(void);


/* Run after net_poll() on every exchange: the unit's main-loop work. */
extern void (*unit_service)(void);

/* The master: send raw bytes or a poll, and keep the unit's reply. */
extern uint8_t          sequence;
extern struct tmp_frame answer;
extern bool             answered;
void exchange_raw(const uint8_t *bytes, size_t length);
/* Nothing on the wire for `ms`: the unit runs, and `answer` is what it sent
 * meanwhile (a NAK for a poll that went bad). */
void quiet(uint32_t ms);
void poll(uint8_t address, const uint8_t *frame, size_t length);
/* A poll with a given SEQ and FLAGS (poll() counts SEQ up and sends none). */
void poll_as(uint8_t address, uint8_t poll_sequence, uint8_t flags, const uint8_t *frame,
             size_t length);
/* The last poll again, as a master whose reply was damaged or missing sends
 * it: the same SEQ, with TMP_FLAG_RETRY, carrying `frame` (the same one). */
void poll_again(uint8_t address, const uint8_t *frame, size_t length);
/* One exchange by tmptun's rule (tmp_retry(), `limit` retries): a poll with
 * the next SEQ, and while its reply counts as damaged, the same poll again.
 * The first `damaged` replies count as damaged, as if the wire had broken
 * them. Returns the polls sent; `answer` is the last reply. */
int  exchange(uint8_t address, const uint8_t *frame, size_t length, int damaged, uint8_t limit);

/* Frame builders; each returns the header length. */
uint16_t checksum(const uint8_t *data, size_t length, uint32_t sum);
void     put16(uint8_t *at, uint16_t value);
void     put32(uint8_t *at, uint32_t value);
size_t   ethernet(uint8_t *frame, const uint8_t *destination, uint16_t type);
size_t   ipv4(uint8_t *frame, uint8_t protocol, size_t payload_length);
uint32_t pseudo_header(uint8_t protocol, size_t length);
/* The same from another source address: a host beyond the master. */
size_t   ipv4_from(uint8_t *frame, uint32_t source, uint8_t protocol, size_t payload_length);
uint32_t pseudo_header_from(uint32_t source, uint8_t protocol, size_t length);

#endif
