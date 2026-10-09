# Porting the slave to a new MCU

The slave's platform half is six functions (`slave/hdx_hal.h`). Everything
above them (the TMP-v2 slave, the MAC driver, the link check, the IP stack)
is portable C and already tested on the host.

## 1. Start from the template

```sh
mkdir port/mymcu
cp port/template/hdx_hal_template.c port/mymcu/hdx_hal_mymcu.c
```

The template compiles and links as it is (the CI checks that it stays
complete), with every hardware step marked `TODO` and the required order of
the transmit steps already written out. `port/samd21` (interrupt into a ring)
and `port/rp2040` (DMA into a ring) are worked examples of the two usual ways
to receive.

## 2. What each function must do

| Function | Must |
|---|---|
| `hdx_rs485_init(baud)` | Make the DE pin an output and drive it low (released) *first*. Set the UART to 8N1 at `baud`, within 2 %. Start receiving into a ring of at least `TMP_FRAME_MAX` (1534) bytes, by interrupt or circular DMA, counting overruns, framing errors and a full ring. Start a millisecond tick if the platform has none. |
| `hdx_rs485_getc(&byte)` | Pop one byte from the ring, or return false. Never block, never read the UART directly. |
| `hdx_rs485_send(data, length)` | Block for the whole reply: wait `RS485_TURNAROUND_US`; raise DE; wait 60 µs; send every byte; wait for *transmit complete* (the last stop bit out, not just the buffer empty); wait 20 µs; discard the ring's contents (our echo); lower DE. Discard nothing after lowering DE. |
| `hdx_rs485_errors()` | Received bytes lost since init. |
| `hdx_millis()` | A free-running millisecond count; wrapping is fine. |
| `hdx_reset()` | Reset the device. |

Why each rule exists is in `docs/PROTOCOL.md` §5. Two mistakes look like a
working port but are not:

- **Waiting for "buffer empty" instead of "transmit complete".** The last
  byte is still being shifted out when DE drops, so the master gets a frame
  whose CRC fails, every time, on the last byte.
- **Discarding after releasing DE.** Everything works under light traffic,
  and a fraction of polls that follow a frame vanish under load, each costing
  the master a 100 ms timeout. Only a test that polls immediately after a
  reply finds it (step 5 below).

## 3. Build it

Add the port to `CMakeLists.txt` like the others: an `HDX_PORT` value, a
`hdx_port_library(hdx_mymcu hdx_stack_mymcu port/mymcu/hdx_hal_mymcu.c)`
call with the target's compile options, and a CI job in
`.github/workflows/ci.yml` that builds it with `-DHDX_WERROR=ON`. A firmware
that builds with its own system can instead compile `slave/*.c`, the port
file and the IP stack's sources directly.

## 4. Bring it up on a bench

A USB-RS-485 adapter with automatic direction control (an FT232R, for
instance) on a Linux machine stands in for the master. Wire A to A and B to
B; build the master (`make -C master/linux`) and set up the interface
(`sudo master/linux/tunnel-up.sh tap0 10.55.0.1/24`). Then, in order:

1. **Silence.** `sudo tools/bustest.py --port /dev/ttyUSB0 --listen 2`: the
   idle line should carry no bytes. Bytes or framing errors on an idle line
   mean noise, a missing bias, or a slave that never releases DE.
2. **Answers.** `--empty 1000`: every poll `ok`. All `silent` means the slave
   hears nothing (power, A/B polarity, baud rate, the slave's receive path).
   All `crcfail` or `partial` means the reply is damaged (transmit complete,
   the hold, the turnaround).
3. **Both directions, loaded.** `--junk 500 --echo 500`: every poll `ok`,
   every echo `ok`.
4. **The real master.** `sudo master/linux/tmptun -v /dev/ttyUSB0 tap0`, and
   from another shell `ping 10.55.0.3`.
5. **Polls right behind a reply.** While `tmptun -v -t 60` runs, flood it:
   `sudo ping -A -c 2000 -s 558 10.55.0.3`. `tmptun` must report no
   timeouts. Any timeout after an exchange that "carried a frame", with
   0 bytes heard, is rule 5: the port discards after releasing DE. Running
   again with `-g 100` (hold such polls 100 µs) makes those timeouts
   disappear if that is the cause.

A USB adapter adds latency of its own (an FTDI chip holds received bytes for
up to its latency timer, 16 ms by default), so take timing numbers with the
master the link will run with. The UART error counters `bustest.py` reports
come from a Raspberry Pi's PL011 and are skipped on other ports.
