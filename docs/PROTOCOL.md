# The link protocol

TMP-v2 over a two-wire RS-485 pair: one master that polls, one slave that
answers, and an Ethernet frame riding in each direction on every exchange.
The wire format is `include/tmp_v2.h`; both ends compile that one header.

## 1. Physical layer

- RS-485, two wires, half duplex: only one side drives the pair at a time.
- **500 000 baud, 8N1** (`TMP_V2_BAUD`). 500 k divides both a 48 MHz SAMD21
  SERCOM and the Raspberry Pi's 48 MHz PL011 clock exactly, and is the top
  rate of the RSM485PHT transceiver the reference master uses. One byte is
  20 µs on the wire.
- The **master's** transceiver switches direction by itself (automatic
  direction control, as on the RSM485PHT or an FT232R USB adapter).
- The **slave** drives its transceiver's driver enable (DE) from firmware. Its
  receiver may stay enabled while it transmits (/RE tied low); the slave then
  hears its own echo, and discards it (rule 5 below).
- Terminate both ends of the cable (120 Ω) and bias the idle line toward
  mark, as for any RS-485 bus.

Swapped A/B wires show as **total silence**: the slave decodes nothing, so it
never answers, and the master sees no bytes and no framing errors at all.
`tools/bustest.py --listen 1 --empty 100` tells that apart from a slave that
answers with errors (CRC failures, partial replies).

## 2. Frame

```
 offset  field          bytes
   0     SYNC0 = 0xAA     1
   1     SYNC1 = 0x55     1
   2     VER<<4 | TYPE    1     VER = 2
   3     ADDR             1     0x01 the slave; 0x00 broadcast
   4     SEQ              1     the master counts; the slave echoes
   5     FLAGS            1     0 here (bit 0 = compressed, bits 4-7 = dictionary: unused)
   6     LEN              2     payload length, big-endian, 0..1522
   8     payload          LEN
 8+LEN   CRC-32           4     big-endian, over VER..payload
```

- 12 bytes of overhead. The largest frame carries a whole Ethernet frame
  (1522 bytes with a VLAN tag, no FCS).
- CRC-32 is the Ethernet/zlib one (reflected 0xEDB88320, init and final XOR
  0xFFFFFFFF). A 16-bit TCP checksum is too weak to be the only check on a
  noisy pair; the CRC is what keeps corrupted frames away from the IP stacks.
- Receivers scan for `AA 55`, read the header, and resynchronise on anything
  that does not parse: a length over 1522, or a CRC that does not match. A bad
  frame is dropped, never delivered. (`tmp_scanner_push()`.)

## 3. Frame types

| Type | Value | Direction | Payload |
|---|---|---|---|
| `TMP_POLL` | 0x1 | master → slave | nothing, or one Ethernet frame for the slave |
| `TMP_NET` | 0x7 | slave → master | nothing, or one Ethernet frame for the master |

The header also defines `REPORT`, `CMD`, `ACK`, `NAK` and `LOG`, and a
compression flag, for the wider TMP-v2 family this link came from. This
project uses only `POLL` and `NET`; a slave ignores everything but a `POLL`
to its address or to broadcast. Each end also ignores its own type, which is
how the slave tells its echo from the master's poll.

## 4. The exchange

1. The master sends a `POLL`, carrying the next frame from its tap interface
   if one is queued.
2. The slave answers every good `POLL` addressed to it with exactly one
   `NET`: the same `SEQ`, carrying the oldest frame in its outbound queue if
   there is one. One `0xFF` byte goes before the frame (rule 2).
3. The master waits up to 100 ms for the `NET`, writes its frame (if any) to
   the tap interface, and polls again at once. When neither direction carried
   a frame it naps 0.5 ms first, so an idle link costs little CPU.
4. A poll with no good answer within 100 ms is simply over. Its frames are
   lost, as on Ethernet; TCP or the application retries.

The frame a poll carries is handed to the slave's IP stack after the reply
has gone, so the answer to it (an ARP reply, an echo reply, a TCP segment)
leaves with the *next* poll. A ping therefore takes two exchanges: request
down, reply up.

## 5. Timing rules

Each of these was found the hard way on real hardware.

1. **Turnaround.** The slave waits `RS485_TURNAROUND_US` after a poll before
   it drives the bus, so the master's transceiver has stopped driving. An
   RSM485PHT let go in about 80 µs; an FT232R adapter needed 150–300 µs. The
   default, 1 ms, covers both. Too short looks like a dead slave: it answers
   every poll, and the master hears none of it.
2. **Preamble.** The `NET` frame is preceded by one `0xFF` byte, which looks
   like idle line. It absorbs the master receiver's own turnaround, so the
   sync bytes arrive intact.
3. **Settle.** After raising DE, the slave waits 60 µs before the first
   start bit, so the line sits at mark and the start bit is a clean edge.
4. **Hold.** The slave keeps DE up until the last stop bit has left the shift
   register ("transmit complete", not "buffer empty"), plus 20 µs.
5. **Echo, then release, then nothing.** If the slave hears itself, it
   discards what it received *while it still holds the bus*, then releases
   DE, and discards nothing after that. The master may poll again as soon as
   the last stop bit ends, and it does after any exchange that carried a
   frame. A slave that released first and flushed 60 µs later lost the start
   of about 5 % of those polls; each loss stalled the link for the master's
   100 ms timeout. See the README's benchmarks.
6. **The master never calls `tcdrain()`.** On a Raspberry Pi it waits on
   kernel timers, far longer than the bytes take. The master writes the poll
   and reads the reply with `poll()`.
7. **The master's timeout** (100 ms) is longer than the longest exchange: a
   full 1534-byte poll and reply take about 62 ms on the wire.

## 6. Sizes and queues

- **Master side:** a tap interface with MTU 1500. Every frame the kernel
  sends fits one `POLL`.
- **Slave side:** frames up to `LINK485_FRAME_MAX` (600 by default). The
  slave's IP stack gets MTU `LINK485_FRAME_MAX - 14` (586), so TCP advertises
  an MSS of 546 and the master's kernel never sends it a larger segment. A
  longer frame arriving anyway (a big ping) is cut by the stack. With 600, an
  ICMP echo carries at most 558 bytes of data.
- **Queues are shallow on purpose.** The slave holds `LINK485_OUTBOUND_SLOTS`
  (6) frames and drops the next when full; the master takes one frame from
  the kernel per poll. Dropping is the backpressure: TCP sees the loss and
  slows down within a round trip, instead of piling up seconds of latency
  in deep buffers.

## 7. Liveness

Polls arriving prove the slave hears the master, not the reverse.
`slave/linkcheck.c` closes the loop: every 10 s the slave ARPs for the
gateway (the master's tap address), which the master's kernel answers
whatever else is running. With no answer for `LINKCHECK_LOSS_RESET_S` (10
minutes) the slave resets, so a slave whose link is broken in any way starts
over rather than staying unreachable.

## 8. Network setup

The tap interface is an ordinary Ethernet interface to the master's kernel:
give it an address (the examples use 10.55.0.1/24 for the master and
10.55.0.3 for the slave) and route as usual. `master/linux/tunnel-up.sh`
creates it with multicast and IPv6 off. A master that also has WiFi or
Ethernet and runs mDNS would otherwise reflect multicast between the
networks, and on a slow link that storm can take both down. ARP is broadcast,
not multicast, so ping and TCP are unaffected.
