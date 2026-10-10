#include "master.h"
#include <string.h>
#include "link485_config.h"
#include "hdx_hal_test.h"
#include "link485.h"

/* ── The unit ───────────────────────────────────────────────────────── */

const uint8_t unit_mac[6] = UNIT_MAC_INIT;
const uint8_t master_mac[6] = { 0x02, 0, 0, 0, 0, 0x01 };

net_t net;
static uint8_t rx_frame[LINK485_FRAME_MAX], tx_frame[LINK485_FRAME_MAX];

void (*unit_service)(void);

net_err_t master_unit_init(void)
{
    hdx_rs485_init(TMP_V2_BAUD);
    link485_init();
    const net_err_t result = net_init(&net, rx_frame, sizeof rx_frame, tx_frame, sizeof tx_frame,
                                      unit_mac, &link485_mac, NULL);
    net.ipv4_addr = UNIT_IP;
    net.subnet_mask = NETMASK;
    net.gateway_ipv4 = MASTER_IP;
    net.mtu = LINK485_FRAME_MAX - 14u;
    return result;
}

/* ── The master ─────────────────────────────────────────────────────── */

uint8_t          sequence;
struct tmp_frame answer;     /* the unit's reply to the last poll */
bool             answered;

void exchange_raw(const uint8_t *bytes, size_t length)
{
    static struct tmp_scanner scanner;
    hdx_test_wire_load(bytes, length);
    link485_service();
    (void)net_poll(&net);
    if (unit_service != NULL) {
        unit_service();
    }

    answered = false;
    tmp_scanner_reset(&scanner);
    for (size_t index = 0u; index < hdx_test_wire_out_length; index++) {
        if (tmp_scanner_push(&scanner, hdx_test_wire_out[index], &answer) == SCAN_FRAME_READY) {
            answered = true;
        }
    }
}

void quiet(uint32_t ms)
{
    static const uint8_t nothing[1];
    hdx_test_now_ms += ms;
    exchange_raw(nothing, 0u);
}

void poll_as(uint8_t address, uint8_t poll_sequence, uint8_t flags, const uint8_t *frame,
             size_t length)
{
    static uint8_t wire[TMP_FRAME_MAX];
    sequence = poll_sequence;
    const size_t wire_length = tmp_frame_build(wire, TMP_POLL, address, poll_sequence, flags,
                                               frame, length);
    exchange_raw(wire, wire_length);
}

void poll(uint8_t address, const uint8_t *frame, size_t length)
{
    poll_as(address, (uint8_t)(sequence + 1u), tmp_flags(false, 0u), frame, length);
}

void poll_again(uint8_t address, const uint8_t *frame, size_t length)
{
    poll_as(address, sequence, (uint8_t)(tmp_flags(false, 0u) | TMP_FLAG_RETRY), frame, length);
}

int exchange(uint8_t address, const uint8_t *frame, size_t length, int damaged, uint8_t limit)
{
    uint8_t tries = 0u;
    int     polls = 1;
    poll(address, frame, length);
    while (tmp_retry(&tries, answered && polls > damaged, limit)) {
        poll_again(address, frame, length);
        polls++;
    }
    return polls;
}

/* ── Frame builders ─────────────────────────────────────────────────── */

uint16_t checksum(const uint8_t *data, size_t length, uint32_t sum)
{
    for (size_t index = 0u; index + 1u < length; index += 2u) {
        sum += (uint32_t)(data[index] << 8) | data[index + 1u];
    }
    if (length & 1u) {
        sum += (uint32_t)(data[length - 1u] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

void put16(uint8_t *at, uint16_t value) { at[0] = (uint8_t)(value >> 8); at[1] = (uint8_t)value; }
void put32(uint8_t *at, uint32_t value) { put16(at, (uint16_t)(value >> 16)); put16(at + 2, (uint16_t)value); }

size_t ethernet(uint8_t *frame, const uint8_t *destination, uint16_t type)
{
    memcpy(frame, destination, 6);
    memcpy(frame + 6, master_mac, 6);
    put16(frame + 12, type);
    return 14u;
}

size_t ipv4(uint8_t *frame, uint8_t protocol, size_t payload_length)
{
    return ipv4_from(frame, MASTER_IP, protocol, payload_length);
}

size_t ipv4_from(uint8_t *frame, uint32_t source, uint8_t protocol, size_t payload_length)
{
    uint8_t *ip = frame + ethernet(frame, unit_mac, 0x0800u);
    memset(ip, 0, 20);
    ip[0] = 0x45u;
    put16(ip + 2, (uint16_t)(20u + payload_length));
    ip[8] = 64u;
    ip[9] = protocol;
    put32(ip + 12, source);
    put32(ip + 16, UNIT_IP);
    put16(ip + 10, checksum(ip, 20u, 0u));
    return 34u;
}

uint32_t pseudo_header(uint8_t protocol, size_t length)
{
    return pseudo_header_from(MASTER_IP, protocol, length);
}

uint32_t pseudo_header_from(uint32_t source, uint8_t protocol, size_t length)
{
    return (source >> 16) + (source & 0xFFFFu) + (UNIT_IP >> 16) + (UNIT_IP & 0xFFFFu)
         + protocol + (uint32_t)length;
}
