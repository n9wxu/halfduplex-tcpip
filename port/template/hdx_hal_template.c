/* A new MCU port starts here. Copy this file to port/<mcu>/hdx_hal_<mcu>.c
 * and replace every TODO; docs/PORTING.md walks through it. As it stands it
 * compiles and links (the CI builds it), but it neither receives nor sends,
 * so a slave built on it never answers a poll.
 *
 * What the slave needs, in short:
 *   - a receiver that never drops a byte while the application runs: an
 *     interrupt or DMA filling a ring that holds a whole poll frame;
 *   - a transmitter that blocks for the whole reply and controls the
 *     transceiver's driver enable (DE) around it, in the order below;
 *   - a millisecond clock and a reset.
 *
 * The SAMD21 port (interrupt into a ring) and the RP2040 port (DMA into a
 * ring) are two worked examples. */

#include "hdx_hal.h"
#include "link485_config.h"

/* TODO: the UART, its pins and the DE pin for your board. */

/* TODO: a power of two at least TMP_FRAME_MAX (1534) bytes. */
#define RX_RING_BYTES 2048u

static uint8_t           rx_ring[RX_RING_BYTES];
static volatile uint32_t rx_head;      /* written by the receive interrupt or DMA */
static volatile uint32_t rx_tail;      /* written by hdx_rs485_getc() */
static volatile uint32_t rx_errors;

/* TODO: busy-wait at least `microseconds`. Erring long is harmless. */
static void delay_us(uint32_t microseconds)
{
    (void)microseconds;
}

/* TODO: drive (true) or release (false) the transceiver's DE pin. */
static void driver_enable(bool driving)
{
    (void)driving;
}

void hdx_rs485_init(uint32_t baud)
{
    /* TODO:
     *  - DE pin: output, low (released) before anything else, so a reset
     *    never leaves the slave driving the bus;
     *  - the UART: 8 data bits, no parity, 1 stop bit at `baud`, FIFO on if
     *    it has one; check the error the baud divisor leaves (under 2 %);
     *  - its receive path into rx_ring: an interrupt that copies each byte
     *    in (and counts overruns, framing errors and a full ring in
     *    rx_errors), or a circular DMA;
     *  - a millisecond tick for hdx_millis(), unless the platform has one. */
    (void)baud;
    rx_head = rx_tail = 0u;
    rx_errors = 0u;
    driver_enable(false);
}

bool hdx_rs485_getc(uint8_t *byte)
{
    if (rx_tail == rx_head) {
        return false;
    }
    *byte = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1u) & (RX_RING_BYTES - 1u);
    return true;
}

void hdx_rs485_send(const uint8_t *data, size_t length)
{
    /* 1. The master's transceiver must have let go of the bus. */
    delay_us(RS485_TURNAROUND_US);
    /* 2. Take the bus, and let the driver settle to idle-mark (high), so the
     *    first start bit is a clean edge. */
    driver_enable(true);
    delay_us(60);
    /* 3. TODO: send every byte, then wait until the last stop bit has left
     *    the shift register: "transmit complete", not "buffer empty". */
    (void)data;
    (void)length;
    delay_us(20);
    /* 4. The bus is still ours: anything received is our own echo. Drop it. */
    rx_tail = rx_head;
    /* 5. Release. From here on every byte is the master's: do not discard
     *    anything after this point; the next poll can start at once. */
    driver_enable(false);
}

uint32_t hdx_rs485_errors(void)
{
    return rx_errors;
}

uint32_t hdx_millis(void)
{
    /* TODO: a free-running millisecond count. */
    return 0u;
}

void hdx_reset(void)
{
    /* TODO: reset the device (a watchdog or the core's system reset). */
}
