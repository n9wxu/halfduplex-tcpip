#ifndef LINKCHECK_H
#define LINKCHECK_H

#include <stdint.h>
#include "net.h"

/* Proof that the master hears us, and a way back if it stops.
 *
 * Polls arriving only show that we hear the master. The round trip shows the
 * reverse: every LINKCHECK_PROBE_MS we ARP for the gateway (the master host's
 * address on the link), whose kernel answers whatever else is or is not
 * running. With no answer for LINKCHECK_LOSS_RESET_S the device resets, so a
 * slave whose link is broken in any way (receive, transmit, turnaround, IP)
 * starts again from scratch instead of staying unreachable. With a bootloader
 * that listens on the link at start, that reset is also the way in for a
 * recovery. */

void linkcheck_init(net_t *net);
void linkcheck_tick(uint32_t elapsed_ms);

/* Milliseconds since the gateway last answered (since start, if it never has). */
uint32_t linkcheck_silent_ms(void);

#endif
