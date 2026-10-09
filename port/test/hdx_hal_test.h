#ifndef HDX_HAL_TEST_H
#define HDX_HAL_TEST_H

#include <stdint.h>
#include <stddef.h>

/* The test port: the HAL on a host, with the wire as two buffers. A test
 * loads what the master sends, runs the slave, and reads back what the slave
 * sent; the clock and the reset are plain variables. This is what the host
 * tests and the CI run on. */

#define HDX_TEST_WIRE_BYTES 4096u

/* Bytes for hdx_rs485_getc() to return, replacing any left unread. */
void hdx_test_wire_load(const uint8_t *bytes, size_t length);

/* Everything hdx_rs485_send() was given since the last load. */
extern uint8_t  hdx_test_wire_out[HDX_TEST_WIRE_BYTES];
extern size_t   hdx_test_wire_out_length;

extern uint32_t hdx_test_now_ms;     /* what hdx_millis() returns */
extern int      hdx_test_resets;     /* hdx_reset() calls */
extern int      hdx_test_sends;      /* hdx_rs485_send() calls */
extern uint32_t hdx_test_baud;       /* as given to hdx_rs485_init() */

#endif
