/* The smallest useful slave: answers ARP and ping at 10.55.0.3 through the
 * link, resends lost TCP segments at the next idle poll, and resets if the
 * master stops answering. A firmware adds its own
 * UDP and TCP services to the same stack (smallest_tcp's udp_set_ports(),
 * tcp_set_connections()).
 *
 * Board bring-up (clocks, pins other than the port's) is the firmware's own
 * and comes before hdx_rs485_init(). The CI builds this against the test port,
 * so it stays in step with the API. */

#include "hdx_hal.h"
#include "link485.h"
#include "link485_config.h"
#include "linkcheck.h"
#include "net.h"
#include "tcplink.h"
#include "tmp_v2.h"

static net_t   net;
static uint8_t rx_frame[LINK485_FRAME_MAX], tx_frame[LINK485_FRAME_MAX];
static const uint8_t mac[6] = { 0x02, 0x56, 0x45, 0x44, 0x43, 0x01 };   /* locally administered */

int main(void)
{
    hdx_rs485_init(TMP_V2_BAUD);
    link485_init();
    if (net_init(&net, rx_frame, sizeof rx_frame, tx_frame, sizeof tx_frame, mac, &link485_mac,
                 NULL) != NET_OK) {
        return 1;
    }
    net.ipv4_addr = NET_IPV4(10, 55, 0, 3);
    net.subnet_mask = NET_IPV4(255, 255, 255, 0);
    net.gateway_ipv4 = NET_IPV4(10, 55, 0, 1);      /* the master's tap interface */
    net.mtu = LINK485_FRAME_MAX - 14u;               /* so TCP's MSS fits our frames */
    linkcheck_init(&net);
    tcplink_init(&net);                              /* the poll is TCP's clock on the link */

    uint32_t last_ms = hdx_millis();
    for (;;) {
        link485_service();           /* answer at most one poll */
        (void)net_poll(&net);        /* hand the stack what that poll carried */
        /* ... the application's own work, kept short: a poll waits for it ... */

        const uint32_t now_ms = hdx_millis();
        if (now_ms != last_ms) {
            net_tick(&net, now_ms - last_ms);
            linkcheck_tick(now_ms - last_ms);
            last_ms = now_ms;
        }
    }
}
