#include "dmaring.h"
#include "hardware/dma.h"

void dmaring_init(dmaring_t *r, void *buf, uint32_t size,
                  const volatile void *src, uint32_t dreq)
{
    r->buf = (uint8_t *)buf;
    r->size = size;
    r->mask = size - 1u;
    r->reload = size;
    r->rd = 0;

    uint bits = 0; while ((1u << bits) < size) bits++;

    r->dc = dma_claim_unused_channel(true);
    r->cc = dma_claim_unused_channel(true);

    /* control channel: one 32-bit write of `size` into the data channel's
     * transfer_count trigger alias -> reloads count and retriggers it. */
    dma_channel_config cc = dma_channel_get_default_config(r->cc);
    channel_config_set_read_increment(&cc, false);
    channel_config_set_write_increment(&cc, false);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_chain_to(&cc, r->cc);           /* self-chain = disabled */
    dma_channel_configure(r->cc, &cc,
        &dma_hw->ch[r->dc].al1_transfer_count_trig,
        (const void *)&r->reload, 1, false);

    /* data channel: src -> ring, write-address ring-wrap, chain to control. */
    dma_channel_config dc = dma_channel_get_default_config(r->dc);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_ring(&dc, true, bits);          /* wrap WRITE addr */
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
    channel_config_set_dreq(&dc, dreq);
    channel_config_set_chain_to(&dc, r->cc);
    dma_channel_configure(r->dc, &dc, r->buf, src, size, true);
}

uint32_t dmaring_widx(dmaring_t *r)
{
    uint32_t rem = dma_hw->ch[r->dc].transfer_count;
    if (rem > r->size) rem = r->size;                  /* mid-reload guard */
    return (r->size - rem) & r->mask;
}

int dmaring_getc(dmaring_t *r, uint8_t *c)
{
    if (r->rd == dmaring_widx(r)) return 0;
    *c = r->buf[r->rd & r->mask];
    r->rd = (r->rd + 1) & r->mask;
    return 1;
}

void dmaring_flush(dmaring_t *r)
{
    r->rd = dmaring_widx(r);
}
