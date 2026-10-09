/*
 * Generic self-reloading DMA byte-ring for the RP2040.
 *
 * A data DMA channel streams bytes from a fixed source register (UART DR or a
 * PIO RX FIFO) into a power-of-two ring using write-address ring-wrap, so the
 * write pointer auto-returns to the base every lap. On completion it chains to
 * a control channel that rewrites the data channel's transfer_count trigger,
 * restarting it forever => zero-CPU continuous capture, no gaps.
 *
 * The ring buffer MUST be aligned to its size (for the write ring-wrap):
 *   static uint8_t buf[256] __attribute__((aligned(256)));
 */
#ifndef DMARING_H
#define DMARING_H

#include <stdint.h>

typedef struct {
    uint8_t  *buf;
    uint32_t  size, mask;
    volatile uint32_t reload;   /* = size; the control channel reads this      */
    int       dc, cc;           /* data / control DMA channel numbers          */
    uint32_t  rd;               /* software read index                         */
} dmaring_t;

/* src = fixed source byte address (e.g. &uart_hw->dr, or PIO rxf MSB byte).
 * dreq = the pacing DREQ for that source. size must be a power of two and buf
 * aligned to size. Claims two DMA channels. */
void     dmaring_init(dmaring_t *r, void *buf, uint32_t size,
                      const volatile void *src, uint32_t dreq);

/* current DMA write index within the ring */
uint32_t dmaring_widx(dmaring_t *r);

/* pop one byte; returns 1 and fills *c if available, else 0 */
int      dmaring_getc(dmaring_t *r, uint8_t *c);

/* drop everything currently buffered (read index := write index) */
void     dmaring_flush(dmaring_t *r);

#endif
