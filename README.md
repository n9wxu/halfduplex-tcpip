# halfduplex-tcpip

Real TCP/IP between a Linux host and a microcontroller over one two-wire
RS-485 pair. The Linux side gets an ordinary Ethernet interface; the
microcontroller gets an IP stack with ping, UDP and TCP. Between them,
a polled, collision-free link carries one Ethernet frame each way on every
exchange, and acknowledges each one, so a damaged frame is sent again on the
very next poll instead of waiting for TCP.

```
  Linux host (master)                                   microcontroller (slave)
 ┌──────────────────────────┐                        ┌──────────────────────────┐
 │ ping, nc, ssh, Python... │                        │ your UDP / TCP services  │
 │ kernel IP stack          │                        │ IP stack (smallest_tcp)  │
 │ tap0  ⇄  tmptun          │  RS-485, two wires     │ link485: TMP-v2 slave    │
 │          (TMP-v2 master) │◀══════════════════════▶│ HAL: UART + DE pin       │
 └──────────────────────────┘  500 kbaud, half duplex └──────────────────────────┘
          POLL [+ one Ethernet frame]  ──▶
                                       ◀──  NET [+ one Ethernet frame]
```

Measured on a SAMD21 (Cortex-M0+, 48 MHz) slave and a Raspberry Pi CM4
master: **ping in 8.9 ms**, **~16 kB/s each way at the same time**, and no
corrupted frame in 73 Mbit of traffic, and with one reply in ten deliberately
corrupted, nothing lost ([benchmarks](#benchmarks)). The slave side costs
2.2 KB of flash plus the IP stack.

## The problem

RS-485 is the wire of choice wherever two boxes are far apart in an
electrically hostile place: vehicles and trailers, farms, plant floors. One
twisted pair runs hundreds of metres and shrugs off noise. But it is **half
duplex**: only one end may drive the pair at a time, and both talking at once
corrupts both.

What you want across that pair is ordinary networking: a TCP stream from a
sensor, a UDP command to an actuator, `ping` to see that it is alive, a TFTP
firmware update. You want the Linux side's tools (`nc`, `ping`, `curl`,
sockets in any language) to work unchanged. The usual options fall short:

- **SLIP and PPP** carry IP over serial lines, but assume a full-duplex
  point-to-point stream where either end may send at any moment. On a
  half-duplex pair the two ends collide.
- **Modbus RTU and similar polled protocols** solve the turn-taking, but
  carry registers, not IP, and every service is hand-built on top.
- **Turn-taking is subtle.** A transceiver needs time to stop driving before
  the other end may start; a receiver that stays on hears its own
  transmission; a slave that misjudges the moment it may listen again loses
  the next message. Get any of it slightly wrong and the link works in a
  quick test and fails under load.
- **Noise** is real on a long cable, and IP's own checks are weak: TCP's
  16-bit checksum, and UDP's, which is optional.
- **Microcontrollers are small.** An IP stack, its buffers and the link
  layer have to fit in kilobytes.

## How this solves it

- **The master polls; the slave answers.** The Linux side always speaks
  first, and the slave answers each poll exactly once, after a turnaround
  delay. There is never a moment when both may talk, so there are no
  collisions and no arbitration.
- **Every exchange carries data both ways.** A poll can carry one Ethernet
  frame down; its answer can carry one up. Traffic in both directions moves
  on every exchange, so the link behaves almost like full duplex at the
  frame level.
- **The Linux side is a TAP interface.** `tmptun` bridges the serial port to
  `tap0`. The kernel sees an Ethernet interface with an address, routes over
  it, and runs TCP over it; nothing on the Linux side knows about RS-485.
- **The microcontroller side is a MAC driver.** `link485` hands the frame in
  each poll to the [smallest_tcp](https://github.com/n9wxu/smallest_tcp) IP
  stack, and queues the stack's frames for the next answers.
- **Every frame has sync bytes, a length and a CRC-32**, the same CRC as
  Ethernet. Receivers resynchronise on garbage and drop any frame that fails
  its CRC.
- **The master's polls are the clock, and every frame is acknowledged.**
  Each poll acknowledges the slave's last frame and carries the master's
  next; each reply does the same the other way. A frame the other end did
  not get intact is not acknowledged, so it simply goes again in the next
  message, a couple of milliseconds later. Nobody asks for a resend and no
  timer decides one, and a repeated frame is recognised and not taken
  twice. TCP's own retransmission, which waits a second or more and backs
  off from there, is left for a link that is actually down.
- **The timing rules live in one place.** The hardware half of the slave is
  a six-function HAL whose contract spells out the turnaround, the preamble,
  the driver-enable hold and the moment the echo may be discarded
  ([docs/PROTOCOL.md](docs/PROTOCOL.md) §6). Ports for the **SAMD21** and
  the **RP2040** follow it, a **test port** runs the whole slave on a host in
  CI, and a **template** starts a new one.
- **The slave watches its own reachability.** It ARPs for the master every
  10 s and resets if nothing has answered for 10 minutes, so a link broken
  in any direction recovers by itself.

The protocol, frame by frame: [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Benchmarks

**Setup.** Master: Raspberry Pi Compute Module 4, Linux 6.1, PL011 UART at
500 000 baud, RSM485PHT isolated transceiver with automatic direction
control, `tmptun` from this repo. Slave: SAMD21G18A at 48 MHz on the SAMD21
port, smallest_tcp v0.1.10, `LINK485_FRAME_MAX` 600, 1 ms turnaround, built
with arm-none-eabi-gcc 13.2 `-Os`. The slave is a working device (it also
streams four VE.Direct serial ports), on a trailer's installed RS-485 cable.
Measured 2026-10-08. The latency, throughput and integrity figures predate
acknowledged delivery, which adds no bytes to any frame; the empty exchange
measured the same with it (2.29 ms mean).

### Latency

| ICMP data | Frame each way | RTT min | mean | max |
|---:|---:|---:|---:|---:|
| 16 B | 58 B | 7.4 ms | 8.9 ms | 14.9 ms |
| 64 B | 106 B | 9.6 ms | 11.3 ms | 13.3 ms |
| 128 B | 170 B | 12.6 ms | 14.2 ms | 16.8 ms |
| 256 B | 298 B | 18.8 ms | 20.2 ms | 22.9 ms |
| 384 B | 426 B | 24.9 ms | 26.4 ms | 29.1 ms |
| 512 B | 554 B | 31.1 ms | 32.5 ms | 33.9 ms |
| 558 B | 600 B | 33.2 ms | 34.7 ms | 36.8 ms |

`ping -c 200 -i 0.1` from the master; none lost. Each byte of payload adds
about 47.5 µs: 40 µs of that is the frame crossing the wire twice (request
down, reply up, 20 µs a byte each way), the rest is work at the two ends.

An exchange with no frame either way takes **2.23 ms** from poll written to
reply complete (mean 2.30 ms over 13 154). Nominally 1.58 ms of that is
on the wire (poll 0.24, turnaround 1.00, driver settle 0.06, reply 0.26,
hold 0.02); the rest is the master's UART and Linux scheduling, and any
slack in the slave's busy-wait delays. Idle, the master polls 347 times a
second.

### Throughput

| Load | Exchanges | Rate |
|---|---|---|
| `ping -A -s 56` (each request waits for the last reply) | 2000 in 22.1 s | 90.7 round trips/s, 10.7 ms mean |
| `ping -A -s 558` | 1999 in 70.1 s | 28.5 round trips/s: **15.9 kB/s of data each way at once** |

At 558 bytes, 34 kB/s of Ethernet frames cross a line whose raw capacity is
50 kB/s shared by both directions: 68 %. The rest is framing, the
turnarounds, and the empty exchange in each ping's cycle. TCP bulk transfer
was not measured (the firmware has no bulk endpoint); with the slave's MSS
of 546 it is bounded by the same figures.

Offered more than that (558-byte pings every 20 ms, 28 kB/s each way), the
link stays up and the backlog queues: RTT climbed to 2.3 s, almost all of it
in the master kernel's tap queue (`txqueuelen`, 1000 frames by default); the
slave holds at most 6.

### Integrity

`tools/bustest.py` in the master's place, every poll classified and every
echo compared byte for byte:

| Traffic | Polls | Result |
|---|---|---|
| empty polls: 2.4 Mbit down, 2.6 Mbit up | 20 000 | 20 000 answered |
| full 1514-byte frames to the slave: 30.4 Mbit | 2 000 | 2 000 answered |
| 558-byte echoes: 18.7 Mbit each way | 6 005 | 3 000 echoes, 2 999 intact; 1 lost |

No CRC failure and no unanswered poll in 51.5 Mbit toward the slave and
21.6 Mbit back: a bit error rate below 6 × 10⁻⁸ and 1.4 × 10⁻⁷ respectively
(95 % confidence). The one loss coincided with the Pi's PL011 overrunning
its 32-byte receive FIFO (Linux interrupt latency, counted as `oe`); across
all runs that happened about once per few thousand full-size replies, and
never on the wire.

### The release-order bug, measured

An earlier slave released the bus and *then*, 60 µs later, discarded its
own echo. The master polls again immediately after any exchange that
carried a frame; when that poll started inside the 60 µs, its sync bytes
went with the echo, and the master waited out its 100 ms timeout.
`tmptun -v` found it:

| Slave | Polls | Timeouts | Pings over 50 ms | Worst ping |
|---|---:|---:|---:|---:|
| discard 60 µs after release | 12 728 | 19 | 38 of 400 | 117 ms |
| same, master holding such polls 50 µs (`-g 50`) | 7 261 | 0 | 0 of 300 | — |
| discard before release (all ports here) | 8 797 | 0 | 0 of 400 | 18 ms |

All 19 timeouts were the poll right after one that carried a ping, with no
byte heard; each stalled the ping in flight and the one behind it. Holding
the master 100, 150 or 300 µs cleared them as well. The host test `link`
now checks that a poll arriving right behind a reply is answered.

### Recovery

What a damaged frame costs, measured on the slave's live VE.Direct TCP
streams (one block every 0.5 s or 1 s per stream, each checksummed):

**Before: best effort, with TCP left to recover.** One data segment of the
inverter's stream dropped every 20 s. smallest_tcp retransmitted it after
1 s, then 2, 4, 8 and 16 s: it backs off and never comes back down
([smallest_tcp#12](https://github.com/n9wxu/smallest_tcp/issues/12)). With
one segment in flight, the 512-byte serial buffer behind it overflowed from
the second loss on: holes of 2.5 to 16.5 s, and corrupted blocks.

**Now: acknowledged by the next poll.** `tmptun -f 10` treats one in ten
replies of every exchange that carried a frame as corrupted:

| Run | Replies corrupted | VE.Direct blocks | Pings | Frames sent again (master / slave) |
|---|---:|---|---|---|
| 130 s | 319 | 477 of 477 intact, no gaps | 1000 of 1000, no duplicates | 168 / 160 |
| 75 s | 186 | 257 of 257 intact, no gaps | 600 of 600, no duplicates, worst 25 ms | 85 / 104 |

Every corrupted reply was repaired by the next poll. In the second run,
with the master as published, the slave counted as duplicates exactly the 85
frames the master sent again. (The first run's master did not yet start in
step with the slave, which cost one frame at its start.) Restarting the
master three times under a 10-per-second ping lost nothing on the link; the
restarts themselves cost two pings each while the service re-created the
interface's address, and the one duplicate was an answer the old master had
taken but not yet acknowledged.
A deterministic link should own its retries, not the TCP timer:
[smallest_tcp#13](https://github.com/n9wxu/smallest_tcp/issues/13).

### Footprint

SAMD21 port, arm-none-eabi-gcc 13.2 `-Os`, Cortex-M0+:

| | Flash | RAM |
|---|---:|---:|
| `link485.c`: TMP-v2 slave, acknowledged delivery, MAC driver | 1 418 B | 7 323 B |
| `linkcheck.c` | 152 B | 14 B |
| `hdx_hal_samd21.c` | 608 B | 2 060 B |
| **link layer** | **2 178 B** | **9 397 B** |
| smallest_tcp: IPv4, ARP, ICMP, UDP, TFTP, TCP | 11 747 B | — |

The RAM is buffers: six 600-byte outbound slots, room for one full-size
inbound frame (the master's MTU is 1500), and the 2 KB receive ring. Fewer
slots (`LINK485_OUTBOUND_SLOTS`) and a smaller `LINK485_FRAME_MAX` shrink
it. The stack's state (`net_t`) and its two frame buffers belong to the
application.

The RP2040 port comes from an earlier RP2040 slave that ran this protocol
against the same master. In the form published here (with the release
order fixed and the turnaround added) it builds in CI but has not been
measured; the figures above are the SAMD21's.

## What is here

```
include/tmp_v2.h          the wire format: frame build, scanner, CRC-32 (both ends)
slave/                    the microcontroller side, portable C
  link485.c, .h             TMP-v2 slave and MAC driver for smallest_tcp
  linkcheck.c, .h           reachability check and reset
  hdx_hal.h                 the HAL: what a port provides
  link485_config.h          build-time settings and their defaults
port/
  samd21/                   SERCOM UART, DE pin, interrupt into a ring
  rp2040/                   pico-sdk UART, DE pin, DMA into a ring
  test/                     the wire as two buffers, for host tests and CI
  template/                 a compiling skeleton for a new MCU
master/linux/             tmptun, its Makefile, systemd unit, interface setup
tools/bustest.py          poll-level link test, run in the master's place
examples/minimal_slave.c  the smallest useful slave
test/                     host tests: the slave and the IP stack, end to end
docs/                     PROTOCOL.md, PORTING.md
```

## Using it

### The master (Linux)

On the host with the RS-485 UART (a Raspberry Pi, for instance):

```sh
cd master/linux
make && sudo make install       # tmptun, tunnel-up.sh, tmptun.service
sudo systemctl edit --full tmptun.service    # serial device, interface address
sudo systemctl enable --now tmptun
ping 10.55.0.3
```

`tmptun <serial-device> [tap]` is the whole master. `-v` logs every
exchange that failed, `-t <s>` runs for a while and prints totals (polls,
answers and failures by kind, frames sent again, duplicates, the empty
exchange's latency). `-r <ms>` sets the slave's response window (100 ms),
`-n <polls>` the run of unanswered polls after which a pending frame is
dropped (50), `-f <n>` corrupts every nth frame-carrying reply on purpose to
exercise the recovery, and `-g <µs>` holds polls that follow a frame, to test
a slave's release timing.

### The slave (microcontroller)

Compile `slave/*.c`, your port and the IP stack into the firmware (or use the
CMake libraries: `cmake -DHDX_PORT=samd21` builds `hdx_samd21` and
`hdx_stack_samd21`), then run the link from the main loop:

```c
hdx_rs485_init(TMP_V2_BAUD);
link485_init();
net_init(&net, rx_frame, sizeof rx_frame, tx_frame, sizeof tx_frame, mac, &link485_mac, NULL);
net.ipv4_addr    = NET_IPV4(10, 55, 0, 3);
net.subnet_mask  = NET_IPV4(255, 255, 255, 0);
net.gateway_ipv4 = NET_IPV4(10, 55, 0, 1);      /* the master's tap0 */
net.mtu          = LINK485_FRAME_MAX - 14u;      /* so TCP's MSS fits */
linkcheck_init(&net);

for (;;) {
    link485_service();       /* answer at most one poll */
    net_poll(&net);          /* hand the stack what it carried */
    /* your work: keep each pass short, a poll waits for it */
    /* every millisecond: net_tick(&net, ms); linkcheck_tick(ms); */
}
```

The full version is [examples/minimal_slave.c](examples/minimal_slave.c).
Services are smallest_tcp's: `udp_set_ports()` for UDP, `tcp_listen()` and
`tcp_set_connections()` for TCP. Settings (frame size, queue depth, slave
address, turnaround, link-check timing) are in
[slave/link485_config.h](slave/link485_config.h), each overridable from the
compiler command line.

The SAMD21 port defaults to the Arduino Zero's D0/D1 on SERCOM0 with DE on
D3; the RP2040 port to uart0 on GPIO16/17 with DE on GPIO18. Both take their
pins and peripheral from `HDX_SAMD21_*` / `HDX_RP2040_*` macros.

## Porting to another MCU

Copy `port/template`, fill in six functions, and bring it up with the
checklist in [docs/PORTING.md](docs/PORTING.md); the last step is the flood
test that would have caught the release-order bug.

## Tests and CI

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

The host tests run the slave on the test port with the real IP stack above
it, driven by a TMP-v2 master written into the test:

- **link**: empty polls and the preamble; polls for another address and
  corrupted ones ignored, and recovery after noise; a poll right behind a
  reply; acknowledged delivery (a reply the master did not acknowledge comes
  again, a frame repeated by the master is taken once, best effort for a
  master without ARQ); ARP, ping, a UDP datagram and a TCP SYN/SYN+ACK with
  an MSS that fits; a full outbound queue dropping its surplus.
- **linkcheck**: probes while the master answers, a reset only after the
  limit when it stops, and the same with no master at all.
- **template_links**: a slave links against the template port alone.

GitHub Actions ([ci.yml](.github/workflows/ci.yml)) runs those, builds the
master with `-Werror`, checks the tools, and cross-builds the SAMD21 port
(arm-none-eabi) and the RP2040 port (pico-sdk 2.2.0), printing each object's
size.

## Origin

Built to reach a VE.Direct concentrator (four Victron VE.Direct serial ports
as TCP streams) in a travel trailer from a Raspberry Pi over the trailer's
RS-485 pair; that firmware is
[ve.direct-concentrator](https://github.com/n9wxu/ve.direct-concentrator).
The protocol and the RP2040 slave came first; the SAMD21 port, the timing
rules and the measurements came from bringing the concentrator up.

## License

MIT; see [LICENSE](LICENSE).
