#ifndef LINK485_CONFIG_H
#define LINK485_CONFIG_H

/* Build-time settings of the slave side. Each has a default; define it on the
 * compiler command line to change it. */

/* The largest Ethernet frame (no FCS) the slave sends or keeps. Longer frames
 * from the master are cut by the IP stack, so give the stack this MTU
 * (net.mtu = LINK485_FRAME_MAX - 14) and TCP advertises an MSS that fits.
 * 600 holds a TFTP block (512 B) with its headers. */
#ifndef LINK485_FRAME_MAX
#define LINK485_FRAME_MAX       600u
#endif

/* Frames of ours waiting for a poll to carry them. Shallow on purpose: a full
 * queue drops, and TCP sees the loss and backs off, instead of piling up
 * seconds of latency. */
#ifndef LINK485_OUTBOUND_SLOTS
#define LINK485_OUTBOUND_SLOTS  6u
#endif

/* The TMP-v2 address we answer (and broadcast, 0x00). */
#ifndef LINK485_ADDRESS
#define LINK485_ADDRESS         0x01u
#endif

/* Quiet time after a poll before the slave drives the bus, so the master's
 * transceiver has let go of it. An RSM485PHT needed about 80 us, an FT232R
 * USB adapter with automatic direction 150-300 us; 1 ms covers both and costs
 * about 1 ms per poll. Used by the rs485 port. */
#ifndef RS485_TURNAROUND_US
#define RS485_TURNAROUND_US     1000u
#endif

/* Quiet on the bus after a poll for us that went bad (its CRC failed, or it
 * stopped short) before the slave answers it with a NAK; a frame cut short is
 * dropped then too. Counted in hdx_millis() ticks, so the quiet is at least
 * one tick less than this: 1-2 ms by default, 50 bytes' time and more. The
 * master must not pause that long inside a frame (docs/PROTOCOL.md §6). */
#ifndef LINK485_QUIET_MS
#define LINK485_QUIET_MS        2u
#endif

/* linkcheck: with no answer from the gateway for this long, reset. */
#ifndef LINKCHECK_LOSS_RESET_S
#define LINKCHECK_LOSS_RESET_S  600u
#endif

/* linkcheck: how often to ARP for the gateway. */
#ifndef LINKCHECK_PROBE_MS
#define LINKCHECK_PROBE_MS      10000u
#endif

#endif
