#ifndef HDX_HAL_H
#define HDX_HAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* The platform half of the slave: one RS-485 port and a clock. Each MCU port
 * implements all of it (port/samd21, port/rp2040); port/test is the fake the
 * host tests run on, and port/template the starting point for a new MCU.
 *
 * The timing rules are what make a half-duplex bus work; docs/PROTOCOL.md
 * explains each one. */

/* Set up the RS-485 port, 8N1 at `baud` (TMP_V2_BAUD on a real link), with
 * the receiver running into a ring and the driver released. */
void hdx_rs485_init(uint32_t baud);

/* One received byte, if any. Bytes arrive by interrupt or DMA, never by
 * polling in here: the ring has to hold a whole poll (TMP_FRAME_MAX bytes)
 * between two calls of link485_service(). */
bool hdx_rs485_getc(uint8_t *byte);

/* Send one reply. In order:
 *   1. wait RS485_TURNAROUND_US (link485_config.h), so the master's
 *      transceiver has let go of the bus;
 *   2. drive the bus (DE) and let the driver settle to idle-mark;
 *   3. send every byte and wait until the last stop bit is on the wire;
 *   4. discard anything received meanwhile: the bus was ours, so it is our
 *      own echo (a transceiver whose receiver stays enabled hears itself);
 *   5. release the bus, and discard nothing after that: the master may poll
 *      again as soon as our last stop bit ends.
 * Blocks throughout; the master is waiting for exactly this. */
void hdx_rs485_send(const uint8_t *data, size_t length);

/* Received bytes lost since init: overruns, framing errors, a full ring. */
uint32_t hdx_rs485_errors(void);

/* A free-running millisecond count. */
uint32_t hdx_millis(void);

/* Reset the device (linkcheck, when the link has been lost for long). */
void hdx_reset(void);

#endif
