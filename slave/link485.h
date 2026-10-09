#ifndef LINK485_H
#define LINK485_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "net_mac.h"

/* The RS-485 side: a TMP-v2 slave that the master polls. Each POLL may carry
 * one Ethernet frame for us; each reply carries one of ours, or none. This is
 * the stack's MAC driver.
 *
 * A reply the master did not get intact, it asks for again: the same SEQ
 * with TMP_FLAG_RETRY. So our frame stays at the head of the queue until a
 * new poll shows that it arrived, and the poll's own frame is taken once,
 * however often it comes. A frame the queue has no room for is dropped, and
 * one the master gave up on is gone; TCP or the application retries. */

extern const net_mac_t link485_mac;

void link485_init(void);

/* Drain the receiver and answer at most one poll. Call continuously, with
 * net_poll() after it. */
void link485_service(void);

/* Called with a valid poll in hand, before the reply is sent: the place for
 * work that stalls the CPU (flash writes), while the master is waiting for us
 * rather than talking. */
void link485_set_before_reply(void (*hook)(void));

/* Every byte on the wire, as received and as sent (a bus monitor). */
void link485_set_tap(void (*tap)(bool transmitted, const uint8_t *data, size_t length));

/* Milliseconds since the last poll addressed to us; UINT32_MAX if never. */
uint32_t link485_poll_age_ms(void);

/* Ethernet frames carried either way since reset (a traffic LED). Empty
 * polls and replies do not count: the master polls without pause. */
uint32_t link485_frames(void);

/* Replies sent again because the master asked (TMP_FLAG_RETRY) since reset. */
uint32_t link485_retries(void);

#endif
