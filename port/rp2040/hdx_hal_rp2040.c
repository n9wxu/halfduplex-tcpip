/* The RP2040 port (pico-sdk): RS-485 on a UART with a driver-enable (DE) pin,
 * received by a self-reloading DMA ring (dmaring.c), so no byte is lost while
 * the application is busy elsewhere and no interrupt is spent on receiving.
 *
 * From the trailer-monitor RP2040 tunnel slave, which ran against a Raspberry
 * Pi CM4 master with an RSM485PHT transceiver. On that board the
 * transceiver's /RE is grounded, so the receiver hears every byte we send.
 *
 * Defaults: uart0, TX on GPIO16, RX on GPIO17, DE on GPIO18. Override any
 * HDX_RP2040_* setting on the compiler command line. */

#include "hdx_hal.h"
#include "link485_config.h"
#include "dmaring.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "pico/time.h"

#ifndef HDX_RP2040_UART
#define HDX_RP2040_UART     uart0
#endif
#ifndef HDX_RP2040_TX_PIN
#define HDX_RP2040_TX_PIN   16u
#endif
#ifndef HDX_RP2040_RX_PIN
#define HDX_RP2040_RX_PIN   17u
#endif
#ifndef HDX_RP2040_DE_PIN
#define HDX_RP2040_DE_PIN   18u       /* high drives the bus */
#endif
/* A power of two, holding a whole poll between two drains. */
#ifndef HDX_RP2040_RX_RING
#define HDX_RP2040_RX_RING  2048u
#endif

static uint8_t   rx_ring[HDX_RP2040_RX_RING] __attribute__((aligned(HDX_RP2040_RX_RING)));
static dmaring_t rx_dma;
static uint32_t  rx_errors;

void hdx_rs485_init(uint32_t baud)
{
    gpio_init(HDX_RP2040_DE_PIN);
    gpio_put(HDX_RP2040_DE_PIN, 0u);
    gpio_set_dir(HDX_RP2040_DE_PIN, GPIO_OUT);

    uart_init(HDX_RP2040_UART, baud);
    gpio_set_function(HDX_RP2040_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(HDX_RP2040_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(HDX_RP2040_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(HDX_RP2040_UART, true);

    rx_errors = 0u;
    dmaring_init(&rx_dma, rx_ring, HDX_RP2040_RX_RING, &uart_get_hw(HDX_RP2040_UART)->dr,
                 uart_get_dreq(HDX_RP2040_UART, false));
}

bool hdx_rs485_getc(uint8_t *byte)
{
    /* The DMA reads the data register 8 bits wide, which drops the per-byte
     * error flags; the sticky ones in RSR still show that something was lost. */
    uart_hw_t *hw = uart_get_hw(HDX_RP2040_UART);
    if (hw->rsr != 0u) {
        hw->rsr = 0u;
        rx_errors++;
    }
    return dmaring_getc(&rx_dma, byte) != 0;
}

void hdx_rs485_send(const uint8_t *data, size_t length)
{
    sleep_us(RS485_TURNAROUND_US);              /* the master lets go of the bus */
    gpio_put(HDX_RP2040_DE_PIN, 1u);
    sleep_us(60);                               /* let the driver settle to idle-mark */
    uart_write_blocking(HDX_RP2040_UART, data, length);
    uart_tx_wait_blocking(HDX_RP2040_UART);
    while ((uart_get_hw(HDX_RP2040_UART)->fr & UART_UARTFR_BUSY_BITS) != 0u) {
        tight_loop_contents();
    }
    sleep_us(40);                               /* hold the final stop bit */
    /* The bus is still ours, so everything received is our own echo. Drop it
     * now: after the release below the master may already be polling. */
    dmaring_flush(&rx_dma);
    gpio_put(HDX_RP2040_DE_PIN, 0u);
}

uint32_t hdx_rs485_errors(void)
{
    return rx_errors;
}

uint32_t hdx_millis(void)
{
    return to_ms_since_boot(get_absolute_time());
}

void hdx_reset(void)
{
    watchdog_reboot(0u, 0u, 0u);
    for (;;) {
        tight_loop_contents();
    }
}
