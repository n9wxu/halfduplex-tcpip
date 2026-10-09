/* A slave linked against the template port alone: every HAL function the
 * slave calls has to be there. One pass of the main loop, which on the
 * template hears nothing. */

#include "hdx_hal.h"
#include "link485.h"
#include "link485_config.h"
#include "linkcheck.h"
#include "net.h"
#include "tmp_v2.h"

int main(void)
{
    static net_t   net;
    static uint8_t rx_frame[LINK485_FRAME_MAX], tx_frame[LINK485_FRAME_MAX];
    static const uint8_t mac[6] = { 0x02, 0, 0, 0, 0, 0x03 };

    hdx_rs485_init(TMP_V2_BAUD);
    link485_init();
    if (net_init(&net, rx_frame, sizeof rx_frame, tx_frame, sizeof tx_frame, mac, &link485_mac,
                 NULL) != NET_OK) {
        return 1;
    }
    linkcheck_init(&net);
    link485_service();
    (void)net_poll(&net);
    linkcheck_tick(1u);
    return link485_poll_age_ms() == UINT32_MAX ? 0 : 1;
}
