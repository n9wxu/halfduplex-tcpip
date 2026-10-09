/* The slave link and MAC driver, driven as the master would drive them, with
 * the real IP stack on top: ARP, ping, a UDP datagram and a TCP open all have
 * to make the round trip through TMP-v2 frames. */

#include <string.h>
#include "link485_config.h"
#include "link485.h"
#include "master.h"
#include "tcp.h"
#include "udp.h"

static void echo(net_t *n, uint32_t source_ip, uint16_t source_port, const uint8_t *source_mac,
                 const uint8_t *payload, uint16_t length)
{
    (void)udp_send(n, source_ip, source_mac, TEST_UDP_PORT, source_port, payload, length);
}
static const udp_port_entry_t udp_ports[] = { { TEST_UDP_PORT, echo } };

static tcp_conn_t       connection;
static tcp_conn_t      *connections[1] = { &connection };
static tcp_saw_tx_ctx_t to_client;
static tcp_saw_rx_ctx_t from_client;
static uint8_t          to_client_bytes[512], from_client_bytes[128];

static int tap_received, tap_transmitted;

static void tap(bool transmitted, const uint8_t *data, size_t length)
{
    (void)data;
    *(transmitted ? &tap_transmitted : &tap_received) += (int)length;
}

int main(void)
{
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t frame[128];
    size_t  length;

    CHECK(master_unit_init() == NET_OK);
    link485_set_tap(tap);
    udp_set_ports(&net, udp_ports, 1);
    tcp_saw_tx_init(&to_client, to_client_bytes, sizeof to_client_bytes);
    tcp_saw_rx_init(&from_client, from_client_bytes, sizeof from_client_bytes);
    CHECK(tcp_conn_init(&connection, &tcp_saw_tx_ops, &to_client, &tcp_saw_rx_ops, &from_client,
                        NULL) == NET_OK);
    CHECK(tcp_listen(&connection, TEST_TCP_PORT) == NET_OK);
    tcp_set_connections(&net, connections, 1);
    CHECK(link485_poll_age_ms() == UINT32_MAX);

    /* An empty poll gets an empty NET reply with the sequence echoed, after a
     * 0xFF preamble. */
    poll(LINK485_ADDRESS, NULL, 0u);
    CHECK(answered && answer.type == TMP_NET && answer.length == 0u);
    CHECK(answer.sequence == sequence && answer.address == LINK485_ADDRESS);
    CHECK(hdx_test_wire_out[0] == 0xFFu && hdx_test_wire_out[1] == TMP_SYNC_0);
    CHECK(tap_received == (int)TMP_FRAME_OVERHEAD && tap_transmitted == (int)TMP_FRAME_OVERHEAD + 1);
    hdx_test_now_ms = 25u;
    CHECK(link485_poll_age_ms() == 25u);

    /* Polls for another address, and corrupted ones, get silence. */
    poll(0x02u, NULL, 0u);
    CHECK(!answered && hdx_test_wire_out_length == 0u);
    {
        uint8_t wire[64];
        const size_t wire_length = tmp_frame_build(wire, TMP_POLL, LINK485_ADDRESS, 9u, 0u,
                                                   (const uint8_t *)"abcdef", 6u);
        wire[10] ^= 0x40u;
        exchange_raw(wire, wire_length);
        CHECK(!answered && hdx_test_wire_out_length == 0u);
        /* ... and the link recovers on the next good frame, even after noise. */
        wire[10] ^= 0x40u;
        memmove(wire + 3, wire, wire_length);
        wire[0] = 0x00u; wire[1] = 0xAAu; wire[2] = 0x13u;
        exchange_raw(wire, wire_length + 3u);
        CHECK(answered && answer.sequence == 9u);
    }

    /* A poll right behind our reply is not lost: the master may poll again as
     * soon as our last stop bit ends, so whatever is in the ring after a
     * reply belongs to the next poll. Two polls arrive back to back; each
     * service call answers one. */
    {
        uint8_t wire[64];
        size_t  wire_length = tmp_frame_build(wire, TMP_POLL, LINK485_ADDRESS, 20u, 0u, NULL, 0u);
        wire_length += tmp_frame_build(wire + wire_length, TMP_POLL, LINK485_ADDRESS, 21u, 0u,
                                       NULL, 0u);
        hdx_test_wire_load(wire, wire_length);
        const int sends = hdx_test_sends;
        link485_service();
        link485_service();
        CHECK(hdx_test_sends == sends + 2);
        static struct tmp_scanner scanner;
        static struct tmp_frame   reply;
        uint8_t                   sequences[2] = { 0u, 0u };
        int                replies = 0;
        tmp_scanner_reset(&scanner);
        for (size_t index = 0u; index < hdx_test_wire_out_length; index++) {
            if (tmp_scanner_push(&scanner, hdx_test_wire_out[index], &reply) == SCAN_FRAME_READY
                && replies < 2) {
                sequences[replies++] = reply.sequence;
            }
        }
        CHECK(replies == 2 && sequences[0] == 20u && sequences[1] == 21u);
    }

    /* ARP: who has the unit's address? The reply to the poll that carries the
     * request is empty (the stack has not seen it yet); the next poll collects
     * the ARP reply. */
    length = ethernet(frame, broadcast, 0x0806u);
    put16(frame + length, 1u); put16(frame + length + 2, 0x0800u);
    frame[length + 4] = 6u; frame[length + 5] = 4u; put16(frame + length + 6, 1u);
    memcpy(frame + length + 8, master_mac, 6); put32(frame + length + 14, MASTER_IP);
    memset(frame + length + 18, 0, 6);          put32(frame + length + 24, UNIT_IP);
    poll(LINK485_ADDRESS, frame, length + 28u);
    CHECK(answered && answer.length == 0u);
    poll(LINK485_ADDRESS, NULL, 0u);
    CHECK(answered && answer.length >= 42u);
    CHECK(memcmp(answer.payload, master_mac, 6) == 0 && memcmp(answer.payload + 6, unit_mac, 6) == 0);
    CHECK(answer.payload[12] == 0x08u && answer.payload[13] == 0x06u && answer.payload[21] == 2u);
    CHECK(memcmp(answer.payload + 22, unit_mac, 6) == 0);

    /* Ping. */
    length = ipv4(frame, 1u, 8u + 4u);
    memset(frame + length, 0, 8); frame[length] = 8u;
    memcpy(frame + length + 8, "ping", 4);
    put16(frame + length + 2, checksum(frame + length, 12u, 0u));
    poll(LINK485_ADDRESS, frame, length + 12u);
    poll(LINK485_ADDRESS, NULL, 0u);
    CHECK(answered && answer.length >= 46u);
    CHECK(answer.payload[23] == 1u && answer.payload[34] == 0u);        /* ICMP echo reply */
    CHECK(memcmp(answer.payload + 42, "ping", 4) == 0);

    /* UDP to an echo port. */
    length = ipv4(frame, 17u, 8u + 6u);
    put16(frame + length, 40000u); put16(frame + length + 2, TEST_UDP_PORT);
    put16(frame + length + 4, 14u); put16(frame + length + 6, 0u);
    memcpy(frame + length + 8, "status", 6);
    put16(frame + length + 6, checksum(frame + length, 14u, pseudo_header(17u, 14u)));
    poll(LINK485_ADDRESS, frame, length + 14u);
    poll(LINK485_ADDRESS, NULL, 0u);
    CHECK(answered && answer.length == 34u + 8u + 6u);
    CHECK(answer.payload[36] == (40000u >> 8) && answer.payload[37] == (40000u & 0xFFu));
    CHECK(memcmp(answer.payload + 42, "status", 6) == 0);

    /* TCP: a SYN to a listening port is answered SYN+ACK, with an MSS that
     * fits our frame buffer. */
    length = ipv4(frame, 6u, 20u);
    memset(frame + length, 0, 20);
    put16(frame + length, 40001u); put16(frame + length + 2, TEST_TCP_PORT);
    put32(frame + length + 4, 1000u);
    frame[length + 12] = 0x50u; frame[length + 13] = TCP_FLAG_SYN;
    put16(frame + length + 14, 4096u);
    put16(frame + length + 16, checksum(frame + length, 20u, pseudo_header(6u, 20u)));
    poll(LINK485_ADDRESS, frame, length + 20u);
    poll(LINK485_ADDRESS, NULL, 0u);
    CHECK(answered && answer.length >= 54u);
    CHECK(answer.payload[23] == 6u);
    CHECK(answer.payload[34 + 13] == (TCP_FLAG_SYN | TCP_FLAG_ACK));
    CHECK(get_be32(&answer.payload[34 + 8]) == 1001u);
    CHECK(answer.payload[54] == TCP_OPT_MSS);
    CHECK(get_be16(&answer.payload[56]) <= LINK485_FRAME_MAX - 54u);
    CHECK(connection.state == TCP_SYN_RECEIVED);

    /* More frames than the queue holds: the surplus is dropped, the link
     * carries the rest one per poll and then goes back to empty replies. */
    for (int count = 0; count < 10; count++) {
        (void)udp_send(&net, MASTER_IP, master_mac, TEST_UDP_PORT, 40000u,
                       (const uint8_t *)"x", 1u);
    }
    int carried = 0;
    for (int count = 0; count < 10; count++) {
        poll(LINK485_ADDRESS, NULL, 0u);
        CHECK(answered);
        carried += (answer.length != 0u);
    }
    CHECK(carried == 6);

    printf("link: ok\n");
    return 0;
}
