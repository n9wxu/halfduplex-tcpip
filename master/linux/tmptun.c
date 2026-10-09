/*
 * tmptun — the Linux end of a half-duplex TCP/IP link: the RS-485 master.
 *
 * Every cycle it sends a TMP_POLL carrying one Ethernet frame from the tap
 * interface (or none) and reads the slave's TMP_NET reply carrying one frame
 * for the tap (or none); the kernel's IP stack does everything else. The
 * transceiver must switch direction by itself (automatic direction control),
 * and the reply is read with poll(), never tcdrain().
 *
 * Delivery is acknowledged, one frame in flight each way (TMP_FLAG_ARQ). Each
 * poll carries the master's pending frame with its SEQ bit, and ACK: the SEQ
 * it expects next from the slave. Each reply does the same the other way. A
 * frame stays pending until the other end's ACK moves past it; until then it
 * simply goes again in the next message. Nobody asks for a resend and no
 * timer decides one: the next poll is the clock, and a poll that does not
 * acknowledge a frame is what makes the slave send it again. A frame that
 * arrives with a SEQ already taken is a duplicate (the acknowledgement was
 * lost) and is not taken twice. A slave without ARQ gets best effort.
 *
 * The two ends start in step: until the master has heard one ARQ reply it
 * polls empty, and from that reply it takes the slave's SEQ and ACK as its
 * own. A reply without ARQ (a slave's bootloader, after a reset) or a run of
 * failed exchanges puts it out of step again, so it starts over from the
 * slave's state whenever either end restarts, instead of losing a frame.
 *
 * The one wait is the half-duplex turn: the master may not talk while the
 * slave might. A reply must start within the slave's response window (-r)
 * after the poll has left; once it starts, its length says when it ends, so a
 * damaged reply is known at that moment and the next poll goes out at once.
 *
 *   tmptun [-v] [-t s] [-n polls] [-r ms] [-f n] [-g us] <serial-device> [tap]
 *
 *   -v  log every exchange that failed, and print totals when stopped
 *   -t  stop after this many seconds and print totals (a measurement run)
 *   -n  failed exchanges in a row before the master drops the frame it has
 *       pending (default 50: a slave gone, not a damaged reply)
 *   -r  the slave's response window: how long after the poll has left its
 *       reply may start, in ms (default 100: a slave that writes flash before
 *       answering takes tens of ms). Never shorter than the slave's slowest
 *       answer: a poll sent while the slave is answering collides with it
 *   -f  fault injection: treat every nth intact reply of an exchange that
 *       carried a frame as corrupted, to exercise the acknowledgements
 *   -g  hold a poll that follows a frame-carrying exchange this long, in
 *       microseconds (a diagnostic for a slave that cannot hear the bus right
 *       after its reply; without it the next poll goes out at once)
 *
 * The tap interface (default tap0) must exist: master/linux/tunnel-up.sh.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "tmp_v2.h"

_Static_assert(TMP_V2_BAUD == 500000u, "open_serial() sets B500000");

#define BYTE_US             (10L * 1000000L / (long)TMP_V2_BAUD)   /* 8N1: 20 us */
#define REPLY_END_MARGIN_US 3000L    /* after a reply's last byte, for the UART and Linux */
#define IDLE_NAP_NS         (500L * 1000L)   /* 0.5 ms when both directions are quiet */

static long monotonic_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000L + now.tv_nsec / 1000L;
}

static int open_serial(const char *device)
{
    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror(device);
        return -1;
    }
    struct termios settings;
    tcgetattr(fd, &settings);
    cfmakeraw(&settings);
    cfsetispeed(&settings, B500000);
    cfsetospeed(&settings, B500000);
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &settings);
    return fd;
}

static int open_tap(const char *name)
{
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/net/tun");
        return -1;
    }
    struct ifreq request;
    memset(&request, 0, sizeof request);
    request.ifr_flags = IFF_TAP | IFF_NO_PI;
    snprintf(request.ifr_name, IFNAMSIZ, "%s", name);
    if (ioctl(fd, TUNSETIFF, &request) < 0) {
        perror("TUNSETIFF");
        close(fd);
        return -1;
    }
    return fd;
}

static uint16_t take_frame_from_tap(int tap_fd, uint8_t *out)
{
    ssize_t count = read(tap_fd, out, TMP_ETHERNET_FRAME_MAX);
    return (count > 0) ? (uint16_t)count : 0u;
}

/* ── one exchange ──────────────────────────────────────────────────── */

enum result { ANSWERED, SILENT, PARTIAL, CRC_FAILED, WRONG_SEQUENCE, INJECTED, RESULTS };
static const char *const result_names[RESULTS] = {
    "answered", "silent", "partial", "crc failed", "wrong seq", "injected"
};

static struct {
    unsigned long polls, results[RESULTS];
    unsigned long down_again, up_duplicates, down_dropped, syncs;
    unsigned long frames_down, frames_up, bytes_down, bytes_up;
    /* poll written -> reply complete, for first-try exchanges with no frame */
    unsigned long empty_count;
    long          empty_min_us, empty_max_us, empty_sum_us;
} stats = { .empty_min_us = LONG_MAX };

static long reply_window_us = 100000L;
static unsigned long inject_every;      /* -f */
static unsigned long carrying_replies;  /* intact replies of frame-carrying exchanges */

struct outcome {
    long    reply_us;          /* poll written -> reply complete */
    size_t  heard;             /* bytes received for this poll */
    uint8_t first[16];
};

/* One poll and its reply. The reply has to start within the response window
 * after the poll has left; once it has started, the frame's length (or, until
 * the length is known, the longest frame) sets when it must be complete. */
static enum result poll_for_reply(int serial_fd, uint8_t sequence, uint8_t flags,
                                  const uint8_t *down_frame, uint16_t down_length,
                                  struct tmp_scanner *scanner, struct tmp_frame *reply,
                                  struct outcome *outcome)
{
    static uint8_t wire[TMP_FRAME_MAX];
    const size_t wire_length = tmp_frame_build(wire, TMP_POLL, TMP_ADDR_COLLECTOR, sequence,
                                               flags, down_frame, down_length);
    memset(outcome, 0, sizeof *outcome);

    tcflush(serial_fd, TCIFLUSH);
    tmp_scanner_reset(scanner);
    const long sent_us = monotonic_us();
    if (write(serial_fd, wire, wire_length) < 0) {
        return SILENT;
    }
    stats.polls++;

    long deadline_us = sent_us + (long)wire_length * BYTE_US + reply_window_us;
    long first_us = -1;
    bool length_known = false;
    for (;;) {
        long remaining_us = deadline_us - monotonic_us();
        if (remaining_us > 0) {
            struct pollfd watch = { .fd = serial_fd, .events = POLLIN, .revents = 0 };
            (void)poll(&watch, 1, (int)((remaining_us + 999) / 1000));
        }
        uint8_t chunk[256];
        const ssize_t got = read(serial_fd, chunk, sizeof chunk);
        if (got <= 0) {
            if (deadline_us - monotonic_us() <= 0) {
                return outcome->heard == 0u ? SILENT : PARTIAL;
            }
            continue;
        }
        if (first_us < 0) {
            first_us = monotonic_us();
            deadline_us = first_us + (1L + (long)TMP_FRAME_MAX) * BYTE_US + REPLY_END_MARGIN_US;
        }
        for (ssize_t index = 0; index < got; index++) {
            if (outcome->heard < sizeof outcome->first) {
                outcome->first[outcome->heard] = chunk[index];
            }
            outcome->heard++;
            const enum tmp_scan_result scanned = tmp_scanner_push(scanner, chunk[index], reply);
            if (scanned == SCAN_CRC_FAILED) {
                return CRC_FAILED;
            }
            if (scanned == SCAN_FRAME_READY && reply->type == TMP_NET) {
                if (reply->sequence != sequence) {
                    return WRONG_SEQUENCE;
                }
                outcome->reply_us = monotonic_us() - sent_us;
                if (inject_every != 0u && (down_length != 0u || reply->length != 0u)
                    && ++carrying_replies % inject_every == 0u) {
                    return INJECTED;
                }
                return ANSWERED;
            }
            if (!length_known
                && (scanner->state == SCAN_PAYLOAD || scanner->state == SCAN_CRC)) {
                /* The header is in: the frame ends this many bytes after its first. */
                length_known = true;
                deadline_us = first_us
                            + (1L + (long)TMP_FRAME_OVERHEAD + scanner->length) * BYTE_US
                            + REPLY_END_MARGIN_US;
            }
        }
    }
}

static void print_totals(double seconds, long gap_us)
{
    printf("%.1f s, gap %ld us: polls %lu (%.0f/s), answered %lu", seconds, gap_us, stats.polls,
           stats.polls / seconds, stats.results[ANSWERED]);
    for (int kind = SILENT; kind < RESULTS; kind++) {
        printf(", %s %lu", result_names[kind], stats.results[kind]);
    }
    printf("\n  frames sent again %lu, duplicates from the slave %lu, dropped %lu, "
           "in step %lu times\n", stats.down_again, stats.up_duplicates, stats.down_dropped,
           stats.syncs);
    printf("  frames down %lu (%lu B), up %lu (%lu B)\n", stats.frames_down, stats.bytes_down,
           stats.frames_up, stats.bytes_up);
    if (stats.empty_count != 0u) {
        printf("  empty exchange: min %ld us, mean %ld us, max %ld us (%lu)\n",
               stats.empty_min_us, stats.empty_sum_us / (long)stats.empty_count,
               stats.empty_max_us, stats.empty_count);
    }
    fflush(stdout);
}

static volatile sig_atomic_t stopping;

static void stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void usage(void)
{
    fprintf(stderr, "usage: tmptun [-v] [-t s] [-n polls] [-r ms] [-f n] [-g us] "
                    "<serial-device> [tap-interface]\n");
}

int main(int argc, char **argv)
{
    bool verbose = false;
    long run_s = 0, gap_us = 0, max_failures = 50;
    int option;
    while ((option = getopt(argc, argv, "vt:n:r:f:g:")) != -1) {
        switch (option) {
        case 'v': verbose = true; break;
        case 't': run_s = atol(optarg); break;
        case 'n': max_failures = atol(optarg); break;
        case 'r': reply_window_us = atol(optarg) * 1000L; break;
        case 'f': inject_every = strtoul(optarg, NULL, 10); break;
        case 'g': gap_us = atol(optarg); break;
        default: usage(); return 2;
        }
    }
    if (optind >= argc || max_failures < 1 || reply_window_us <= 0 || inject_every == 1u) {
        usage();
        return 2;
    }
    const char *serial_device = argv[optind];
    const char *tap_name = (optind + 1 < argc) ? argv[optind + 1] : "tap0";

    const int serial_fd = open_serial(serial_device);
    const int tap_fd = open_tap(tap_name);
    if (serial_fd < 0 || tap_fd < 0) {
        return 1;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    fprintf(stderr, "tmptun master on %s <-> %s @ %u, ARQ, %ld ms response window\n",
            serial_device, tap_name, TMP_V2_BAUD, reply_window_us / 1000L);

    struct tmp_scanner scanner = { 0 };
    tmp_scanner_reset(&scanner);
    static uint8_t down_frame[TMP_ETHERNET_FRAME_MAX];
    struct tmp_frame reply = { 0 };
    struct outcome outcome;
    uint8_t sequence = 0u;
    const long started_us = monotonic_us();
    long last_end_us = 0;
    bool last_carried = false;

    /* Acknowledged delivery: our pending frame and its SEQ, the SEQ we expect
     * next from the slave, and whether the slave does ARQ at all. */
    uint16_t down_length = 0u;
    bool     down_bit = false, up_expected = false;
    long     failures = 0;
    enum { OUT_OF_STEP, IN_STEP, BEST_EFFORT } mode = OUT_OF_STEP;

    while (!stopping && (run_s == 0 || monotonic_us() - started_us < run_s * 1000000L)) {
        if (down_length == 0u) {
            if (mode != OUT_OF_STEP) {     /* out of step: poll empty until in step */
                down_length = take_frame_from_tap(tap_fd, down_frame);
            }
        } else {
            stats.down_again++;
        }
        if (gap_us > 0 && last_carried) {
            while (monotonic_us() - last_end_us < gap_us) {
            }
        }
        const uint8_t flags = TMP_FLAG_ARQ | (down_bit ? TMP_FLAG_SEQ : 0u)
                            | (up_expected ? TMP_FLAG_ACK : 0u);
        const enum result result = poll_for_reply(serial_fd, sequence, flags,
                                                  down_length ? down_frame : NULL, down_length,
                                                  &scanner, &reply, &outcome);
        stats.results[result]++;
        uint16_t up_length = 0u;
        bool carried = down_length != 0u;
        if (result == ANSWERED) {
            failures = 0;
            const bool arq = (reply.flags & TMP_FLAG_ARQ) != 0u;
            if (!arq) {
                mode = BEST_EFFORT;
            } else if (mode != IN_STEP) {
                /* Start in step with the slave: its ACK is the SEQ it expects
                 * from us, its SEQ the one it is sending. */
                down_bit = (reply.flags & TMP_FLAG_ACK) != 0u;
                up_expected = (reply.flags & TMP_FLAG_SEQ) != 0u;
                mode = IN_STEP;
                stats.syncs++;
            }
            /* Our frame: taken once the slave's ACK moves past its SEQ (or at
             * once, by a slave without ARQ). */
            if (down_length != 0u
                && (!arq || ((reply.flags & TMP_FLAG_ACK) != 0u) != down_bit)) {
                stats.frames_down++;
                stats.bytes_down += down_length;
                down_length = 0u;
                if (arq) {
                    down_bit = !down_bit;
                }
            }
            /* The slave's frame: new if its SEQ is the one we expect. */
            if (reply.length != 0u) {
                carried = true;
                if (!arq || ((reply.flags & TMP_FLAG_SEQ) != 0u) == up_expected) {
                    (void)!write(tap_fd, reply.payload, reply.length);
                    up_length = reply.length;
                    stats.frames_up++;
                    stats.bytes_up += up_length;
                    if (arq) {
                        up_expected = !up_expected;
                    }
                } else {
                    stats.up_duplicates++;
                }
            }
            if (!carried) {
                stats.empty_count++;
                stats.empty_sum_us += outcome.reply_us;
                if (outcome.reply_us < stats.empty_min_us) stats.empty_min_us = outcome.reply_us;
                if (outcome.reply_us > stats.empty_max_us) stats.empty_max_us = outcome.reply_us;
            }
        } else {
            /* Nothing acknowledged either way: the next poll carries the same
             * frame and the same ACK, and goes out at once. */
            carried = true;
            failures++;
            if (verbose) {
                printf("seq %u: %s (pending %u B, heard %zu B", sequence, result_names[result],
                       down_length, outcome.heard);
                for (size_t index = 0u; index < outcome.heard && index < sizeof outcome.first;
                     index++) {
                    printf("%s %02x", index == 0u ? ":" : "", outcome.first[index]);
                }
                printf(")\n");
            }
            if (failures >= max_failures && mode != OUT_OF_STEP) {
                /* The slave is gone, or restarting: drop what we hold, and
                 * start in step with it again when it answers. */
                mode = OUT_OF_STEP;
                if (down_length != 0u) {
                    stats.down_dropped++;
                    down_length = 0u;
                }
                if (verbose) {
                    printf("seq %u: no answer %ld times running, out of step\n", sequence,
                           failures);
                }
            }
            if (verbose) {
                fflush(stdout);
            }
        }
        last_end_us = monotonic_us();
        last_carried = carried;
        sequence++;
        if (!carried && down_length == 0u) {
            const struct timespec nap = { .tv_sec = 0, .tv_nsec = IDLE_NAP_NS };
            nanosleep(&nap, NULL);
        }
    }

    if (verbose || run_s != 0) {
        print_totals((monotonic_us() - started_us) / 1e6, gap_us);
    }
    return 0;
}
