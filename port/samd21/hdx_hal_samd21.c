/* The SAMD21 port: RS-485 on a SERCOM USART with a driver-enable (DE) pin,
 * received by interrupt into a ring; SysTick for milliseconds.
 *
 * In service on a SAMD21G18A at 48 MHz (the VE.Direct concentrator), against
 * a Raspberry Pi CM4 master with an RSM485PHT transceiver.
 *
 * Defaults are the Arduino Zero pin map: TX on D1 (PA10), RX on D0 (PA11),
 * SERCOM0 pads 2/3, DE on D3 (PA09). Override any HDX_SAMD21_* setting on the
 * compiler command line. The core must already run at HDX_SAMD21_CPU_HZ from
 * GCLK0 when hdx_rs485_init() is called; the port does not touch the clocks
 * beyond its SERCOM's. */

#include "hdx_hal.h"
#include "link485_config.h"
#include "sam.h"

#ifndef HDX_SAMD21_CPU_HZ
#define HDX_SAMD21_CPU_HZ       48000000u
#endif
#ifndef HDX_SAMD21_SERCOM
#define HDX_SAMD21_SERCOM       SERCOM0
#define HDX_SAMD21_SERCOM_INDEX 0u
#define HDX_SAMD21_IRQ_HANDLER  SERCOM0_Handler
#endif
/* Pins as (port << 5) | number: port A = 0, B = 1. */
#ifndef HDX_SAMD21_TX_PIN
#define HDX_SAMD21_TX_PIN       ((0u << 5) | 10u)   /* PA10, Zero D1 */
#endif
#ifndef HDX_SAMD21_RX_PIN
#define HDX_SAMD21_RX_PIN       ((0u << 5) | 11u)   /* PA11, Zero D0 */
#endif
#ifndef HDX_SAMD21_PIN_FUNCTION
#define HDX_SAMD21_PIN_FUNCTION 2u                  /* peripheral C: SERCOM */
#endif
#ifndef HDX_SAMD21_TX_PAD
#define HDX_SAMD21_TX_PAD       2u                  /* 0 or 2 */
#endif
#ifndef HDX_SAMD21_RX_PAD
#define HDX_SAMD21_RX_PAD       3u
#endif
#ifndef HDX_SAMD21_DE_PIN
#define HDX_SAMD21_DE_PIN       ((0u << 5) | 9u)    /* PA09, Zero D3; high drives */
#endif
/* A power of two, holding a whole poll between two drains. */
#ifndef HDX_SAMD21_RX_RING
#define HDX_SAMD21_RX_RING      2048u
#endif
/* 1: the port runs SysTick at 1 kHz for hdx_millis(). 0: the application
 * owns SysTick and supplies hdx_millis() itself. */
#ifndef HDX_SAMD21_SYSTICK
#define HDX_SAMD21_SYSTICK      1
#endif

#if (HDX_SAMD21_RX_RING & (HDX_SAMD21_RX_RING - 1u)) != 0u
#error "HDX_SAMD21_RX_RING must be a power of two"
#endif

static uint8_t           rx_ring[HDX_SAMD21_RX_RING];
static volatile uint16_t rx_head;      /* written by the interrupt */
static volatile uint16_t rx_tail;      /* written by hdx_rs485_getc() */
static volatile uint32_t rx_errors;

/* ── pins ───────────────────────────────────────────────────────────── */

static void pin_write(uint8_t pin, bool high)
{
    if (high) {
        PORT->Group[pin >> 5].OUTSET.reg = 1ul << (pin & 31u);
    } else {
        PORT->Group[pin >> 5].OUTCLR.reg = 1ul << (pin & 31u);
    }
}

static void pin_output(uint8_t pin)
{
    PORT->Group[pin >> 5].DIRSET.reg = 1ul << (pin & 31u);
}

static void pin_peripheral(uint8_t pin, uint8_t function)
{
    PortGroup *group = &PORT->Group[pin >> 5];
    const uint8_t number = pin & 31u;
    if (number & 1u) {
        group->PMUX[number >> 1].bit.PMUXO = function;
    } else {
        group->PMUX[number >> 1].bit.PMUXE = function;
    }
    group->PINCFG[number].reg = PORT_PINCFG_PMUXEN | PORT_PINCFG_INEN;
}

/* About 8 cycles a turn on the Cortex-M0+ (a volatile counter). The waits
 * here are minimums, so erring long is harmless. */
static void delay_us(uint32_t microseconds)
{
    for (volatile uint32_t turns = microseconds * (HDX_SAMD21_CPU_HZ / 8000000u); turns != 0u;
         turns--) {
    }
}

/* ── RS-485 ─────────────────────────────────────────────────────────── */

void hdx_rs485_init(uint32_t baud)
{
    SercomUsart *usart = &HDX_SAMD21_SERCOM->USART;

    pin_write(HDX_SAMD21_DE_PIN, false);
    pin_output(HDX_SAMD21_DE_PIN);

    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM0 << HDX_SAMD21_SERCOM_INDEX;
    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(GCLK_CLKCTRL_ID_SERCOM0_CORE_Val + HDX_SAMD21_SERCOM_INDEX)
                      | GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_CLKEN;
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {
    }

    usart->CTRLA.reg = SERCOM_USART_CTRLA_SWRST;
    while (usart->SYNCBUSY.reg & SERCOM_USART_SYNCBUSY_SWRST) {
    }
    /* Internal clock, 16x oversampling, LSB first. TXPO 0 = TX on pad 0,
     * TXPO 1 = TX on pad 2. */
    usart->CTRLA.reg = SERCOM_USART_CTRLA_MODE_USART_INT_CLK | SERCOM_USART_CTRLA_DORD
                     | SERCOM_USART_CTRLA_RXPO(HDX_SAMD21_RX_PAD)
                     | SERCOM_USART_CTRLA_TXPO(HDX_SAMD21_TX_PAD == 2u ? 1u : 0u);
    usart->BAUD.reg = (uint16_t)(65536ull - (65536ull * 16ull * baud) / HDX_SAMD21_CPU_HZ);
    usart->CTRLB.reg = SERCOM_USART_CTRLB_RXEN | SERCOM_USART_CTRLB_TXEN;
    while (usart->SYNCBUSY.reg & SERCOM_USART_SYNCBUSY_CTRLB) {
    }

    pin_peripheral(HDX_SAMD21_TX_PIN, HDX_SAMD21_PIN_FUNCTION);
    pin_peripheral(HDX_SAMD21_RX_PIN, HDX_SAMD21_PIN_FUNCTION);

    rx_head = rx_tail = 0u;
    rx_errors = 0u;
    usart->INTENSET.reg = SERCOM_USART_INTENSET_RXC | SERCOM_USART_INTENSET_ERROR;
    NVIC_EnableIRQ((IRQn_Type)(SERCOM0_IRQn + HDX_SAMD21_SERCOM_INDEX));

    usart->CTRLA.reg |= SERCOM_USART_CTRLA_ENABLE;
    while (usart->SYNCBUSY.reg & SERCOM_USART_SYNCBUSY_ENABLE) {
    }

#if HDX_SAMD21_SYSTICK
    SysTick_Config(HDX_SAMD21_CPU_HZ / 1000u);
#endif
}

void HDX_SAMD21_IRQ_HANDLER(void)
{
    SercomUsart *usart = &HDX_SAMD21_SERCOM->USART;
    const uint8_t flags = usart->INTFLAG.reg;

    if (flags & SERCOM_USART_INTFLAG_ERROR) {
        usart->STATUS.reg = SERCOM_USART_STATUS_BUFOVF | SERCOM_USART_STATUS_FERR
                          | SERCOM_USART_STATUS_PERR;
        usart->INTFLAG.reg = SERCOM_USART_INTFLAG_ERROR;
        rx_errors++;
    }
    if (flags & SERCOM_USART_INTFLAG_RXC) {
        const uint8_t byte = (uint8_t)usart->DATA.reg;
        const uint16_t next = (uint16_t)((rx_head + 1u) & (HDX_SAMD21_RX_RING - 1u));
        if (next != rx_tail) {
            rx_ring[rx_head] = byte;
            rx_head = next;
        } else {
            rx_errors++;
        }
    }
}

bool hdx_rs485_getc(uint8_t *byte)
{
    if (rx_tail == rx_head) {
        return false;
    }
    *byte = rx_ring[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1u) & (HDX_SAMD21_RX_RING - 1u));
    return true;
}

void hdx_rs485_send(const uint8_t *data, size_t length)
{
    SercomUsart *usart = &HDX_SAMD21_SERCOM->USART;

    delay_us(RS485_TURNAROUND_US);      /* the master lets go of the bus */
    pin_write(HDX_SAMD21_DE_PIN, true);
    delay_us(60);                       /* let the driver settle to idle-mark */
    for (size_t index = 0u; index < length; index++) {
        while (!(usart->INTFLAG.reg & SERCOM_USART_INTFLAG_DRE)) {
        }
        usart->INTFLAG.reg = SERCOM_USART_INTFLAG_TXC;   /* so it can only mean the last byte */
        usart->DATA.reg = data[index];
    }
    while (!(usart->INTFLAG.reg & SERCOM_USART_INTFLAG_TXC)) {
    }
    delay_us(20);                       /* hold the final stop bit */
    /* The bus is still ours, so everything received is our own echo. Drop it
     * now: after the release below the master may already be polling. */
    rx_tail = rx_head;
    pin_write(HDX_SAMD21_DE_PIN, false);
}

uint32_t hdx_rs485_errors(void)
{
    return rx_errors;
}

/* ── clock and reset ────────────────────────────────────────────────── */

#if HDX_SAMD21_SYSTICK
static volatile uint32_t milliseconds;

void SysTick_Handler(void)
{
    milliseconds++;
}

uint32_t hdx_millis(void)
{
    return milliseconds;
}
#endif

void hdx_reset(void)
{
    NVIC_SystemReset();
}
