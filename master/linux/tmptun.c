/*
 * tmptun — the Linux end of a half-duplex TCP/IP link: the RS-485 master.
 *
 * Every cycle it sends a TMP_POLL carrying one Ethernet frame from the tap
 * interface (or none) and reads the slave's TMP_NET reply carrying one frame
 * for the tap (or none); the kernel's IP stack does everything else. Best
 * effort, like Ethernet: a frame lost on the wire is gone, and TCP or the
 * application retries. The transceiver must switch direction by itself
 * (automatic direction control), and the reply is read with poll(), never
 * tcdrain().
 *
 *   tmptun [-v] [-t seconds] [-g gap_us] <serial-device> [tap-interface]
 *
 *   -v  log every poll that went unanswered or came back corrupted, and
 *       print totals when stopped
 *   -t  stop after this many seconds and print totals (a measurement run)
 *   -g  hold a poll that follows a frame-carrying exchange this long, in
 *       microseconds (a diagnostic: without it the next poll goes out at
 *       once, while idle polls nap 0.5 ms; a timeout that a gap removes is a
 *       slave that cannot hear the bus right after its reply)
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

#define REPLY_TIMEOUT_MS    100  /* a full-MTU poll + reply is ~62 ms at 500 kbaud */
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

/* ── counting ───────────────────────────────────────────────────────── */

static struct {
    unsigned long polls, replies, timeouts, crcfails;
    unsigned long frames_down, frames_up, bytes_down, bytes_up;
    /* poll written -> reply complete, for exchanges with no frame either way */
    unsigned long empty_count;
    long          empty_min_us, empty_max_us, empty_sum_us;
} stats = { .empty_min_us = LONG_MAX };

struct outcome {
    bool   replied;
    long   reply_us;          /* poll written -> reply complete */
    long   first_byte_us;     /* poll written -> first byte back, -1 if none */
    size_t heard;             /* bytes received for this poll */
    uint8_t first[16];
};

static bool poll_for_reply(int serial_fd, uint8_t sequence,
                           const uint8_t *down_frame, uint16_t down_length,
                           struct tmp_scanner *scanner, struct tmp_frame *reply,
                           struct outcome *outcome)
{
    static uint8_t wire[TMP_FRAME_MAX];
    const size_t wire_length = tmp_frame_build(wire, TMP_POLL, TMP_ADDR_COLLECTOR, sequence,
                                               tmp_flags(false, 0u), down_frame, down_length);
    memset(outcome, 0, sizeof *outcome);
    outcome->first_byte_us = -1;
    if (wire_length == 0u) {
        return false;
    }

    tcflush(serial_fd, TCIFLUSH);
    tmp_scanner_reset(scanner);
    const long sent_us = monotonic_us();
    if (write(serial_fd, wire, wire_length) < 0) {
        return false;
    }
    stats.polls++;

    const long deadline_us = sent_us + REPLY_TIMEOUT_MS * 1000L;
    for (;;) {
        const long remaining_us = deadline_us - monotonic_us();
        if (remaining_us <= 0) {
            break;
        }
        struct pollfd watch = { .fd = serial_fd, .events = POLLIN, .revents = 0 };
        if (poll(&watch, 1, (int)((remaining_us + 999) / 1000)) <= 0) {
            break;
        }
        uint8_t chunk[256];
        const ssize_t got = read(serial_fd, chunk, sizeof chunk);
        if (got > 0 && outcome->first_byte_us < 0) {
            outcome->first_byte_us = monotonic_us() - sent_us;
        }
        for (ssize_t index = 0; index < got; index++) {
            if (outcome->heard < sizeof outcome->first) {
                outcome->first[outcome->heard] = chunk[index];
            }
            outcome->heard++;
            const enum tmp_scan_result result = tmp_scanner_push(scanner, chunk[index], reply);
            if (result == SCAN_CRC_FAILED) {
                stats.crcfails++;
            } else if (result == SCAN_FRAME_READY && reply->type == TMP_NET) {
                stats.replies++;
                outcome->replied = true;
                outcome->reply_us = monotonic_us() - sent_us;
                return true;
            }
        }
    }
    stats.timeouts++;
    return false;
}

static void print_totals(double seconds, long gap_us)
{
    printf("%.1f s, gap %ld us: polls %lu (%.0f/s) replies %lu timeouts %lu crcfails %lu\n",
           seconds, gap_us, stats.polls, stats.polls / seconds, stats.replies, stats.timeouts,
           stats.crcfails);
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
    fprintf(stderr, "usage: tmptun [-v] [-t seconds] [-g gap_us] <serial-device> [tap-interface]\n");
}

int main(int argc, char **argv)
{
    bool verbose = false;
    long run_s = 0, gap_us = 0;
    int option;
    while ((option = getopt(argc, argv, "vt:g:")) != -1) {
        switch (option) {
        case 'v': verbose = true; break;
        case 't': run_s = atol(optarg); break;
        case 'g': gap_us = atol(optarg); break;
        default: usage(); return 2;
        }
    }
    if (optind >= argc) {
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
    fprintf(stderr, "tmptun master on %s <-> %s @ %u\n", serial_device, tap_name, TMP_V2_BAUD);

    struct tmp_scanner scanner = { 0 };
    tmp_scanner_reset(&scanner);
    static uint8_t down_frame[TMP_ETHERNET_FRAME_MAX];
    struct tmp_frame reply = { 0 };
    struct outcome outcome;
    uint8_t sequence = 0u;
    const long started_us = monotonic_us();
    long last_end_us = 0;
    bool last_carried = false;

    while (!stopping && (run_s == 0 || monotonic_us() - started_us < run_s * 1000000L)) {
        const uint16_t down_length = take_frame_from_tap(tap_fd, down_frame);
        if (gap_us > 0 && last_carried) {
            while (monotonic_us() - last_end_us < gap_us) {
            }
        }
        const long gap_seen_us = last_end_us ? monotonic_us() - last_end_us : -1;
        uint16_t up_length = 0u;
        const bool replied = poll_for_reply(serial_fd, sequence, down_length ? down_frame : NULL,
                                            down_length, &scanner, &reply, &outcome);
        if (down_length != 0u) {
            stats.frames_down++;
            stats.bytes_down += down_length;
        }
        if (replied && reply.length > 0u) {
            (void)!write(tap_fd, reply.payload, reply.length);
            up_length = reply.length;
            stats.frames_up++;
            stats.bytes_up += up_length;
        }
        if (replied && down_length == 0u && up_length == 0u) {
            stats.empty_count++;
            stats.empty_sum_us += outcome.reply_us;
            if (outcome.reply_us < stats.empty_min_us) stats.empty_min_us = outcome.reply_us;
            if (outcome.reply_us > stats.empty_max_us) stats.empty_max_us = outcome.reply_us;
        }
        if (verbose && !replied) {
            printf("timeout seq %u: down %u B, %ld us after the last exchange (which %s), "
                   "heard %zu B", sequence, down_length, gap_seen_us,
                   last_carried ? "carried a frame" : "was empty", outcome.heard);
            if (outcome.heard != 0u) {
                printf(" from %ld us:", outcome.first_byte_us);
                for (size_t index = 0u; index < outcome.heard && index < sizeof outcome.first;
                     index++) {
                    printf(" %02x", outcome.first[index]);
                }
            }
            printf("\n");
            fflush(stdout);
        }
        last_end_us = monotonic_us();
        last_carried = (down_length != 0u || up_length != 0u);
        sequence++;
        if (!last_carried) {
            const struct timespec nap = { .tv_sec = 0, .tv_nsec = IDLE_NAP_NS };
            nanosleep(&nap, NULL);
        }
    }

    if (verbose || run_s != 0) {
        print_totals((monotonic_us() - started_us) / 1e6, gap_us);
    }
    return 0;
}
