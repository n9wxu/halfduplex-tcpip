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
void poll(uint8_t address, const uint8_t *frame, size_t length);
/* A poll with a given SEQ and FLAGS (poll() counts SEQ up and sends none). */
void poll_as(uint8_t address, uint8_t poll_sequence, uint8_t flags, const uint8_t *frame,
             size_t length);

/* A master with acknowledged delivery (TMP_FLAG_ARQ), as tmptun runs it: the
 * SEQ of its frame and the SEQ it expects next from the unit. `lost` pretends
 * the reply arrived corrupted: the master takes nothing from it, so it
 * acknowledges nothing. arq_new: the reply carried a frame the master took. */
extern bool arq_down_bit, arq_up_expected, arq_new;
void arq_poll(const uint8_t *frame, size_t length, bool lost);

/* Frame builders; each returns the header length. */
uint16_t checksum(const uint8_t *data, size_t length, uint32_t sum);
void     put16(uint8_t *at, uint16_t value);
void     put32(uint8_t *at, uint32_t value);
size_t   ethernet(uint8_t *frame, const uint8_t *destination, uint16_t type);
size_t   ipv4(uint8_t *frame, uint8_t protocol, size_t payload_length);
uint32_t pseudo_header(uint8_t protocol, size_t length);

#endif
