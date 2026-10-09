#include "hdx_hal.h"
#include <string.h>
#include "hdx_hal_test.h"

static uint8_t wire_in[HDX_TEST_WIRE_BYTES];
static size_t  wire_in_length, wire_in_at;

uint8_t  hdx_test_wire_out[HDX_TEST_WIRE_BYTES];
size_t   hdx_test_wire_out_length;
uint32_t hdx_test_now_ms;
int      hdx_test_resets;
int      hdx_test_sends;
uint32_t hdx_test_baud;

void hdx_test_wire_load(const uint8_t *bytes, size_t length)
{
    if (length > sizeof wire_in) {
        length = sizeof wire_in;
    }
    memcpy(wire_in, bytes, length);
    wire_in_length = length;
    wire_in_at = 0u;
    hdx_test_wire_out_length = 0u;
}

void hdx_rs485_init(uint32_t baud)
{
    hdx_test_baud = baud;
    wire_in_length = wire_in_at = 0u;
    hdx_test_wire_out_length = 0u;
}

bool hdx_rs485_getc(uint8_t *byte)
{
    if (wire_in_at >= wire_in_length) {
        return false;
    }
    *byte = wire_in[wire_in_at++];
    return true;
}

void hdx_rs485_send(const uint8_t *data, size_t length)
{
    hdx_test_sends++;
    if (length > sizeof hdx_test_wire_out - hdx_test_wire_out_length) {
        length = sizeof hdx_test_wire_out - hdx_test_wire_out_length;
    }
    memcpy(&hdx_test_wire_out[hdx_test_wire_out_length], data, length);
    hdx_test_wire_out_length += length;
}

uint32_t hdx_rs485_errors(void)
{
    return 0u;
}

uint32_t hdx_millis(void)
{
    return hdx_test_now_ms;
}

void hdx_reset(void)
{
    hdx_test_resets++;
}
