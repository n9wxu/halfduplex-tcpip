/*
 * tmptun — the Linux end of a half-duplex TCP/IP link: the RS-485 master.
 *
 * Every cycle it sends a TMP_POLL carrying one Ethernet frame from the tap
 * interface (or none) and reads the slave's TMP_NET reply carrying one frame
 * for the tap (or none); the kernel's IP stack does everything else. The
 * transceiver must switch direction by itself (automatic direction control),
 * and the reply is read with poll(), never tcdrain().
 *
 * The link repairs what it damages. A reply that is damaged or missing (a
 * CRC failure, a reply cut short or with the wrong SEQ, or no reply at all) is
 * never handed over, and the same poll goes again at once: the same SEQ, the
 * same frame, and TMP_FLAG_RETRY. The slave answers with the same reply, and
 * takes the poll's frame only once, so either direction's frame gets through
 * on a later try. After -n tries the master gives up on the exchange and polls
 * anew; TCP's own timer recovers what it carried. The limit keeps the time
 * the link holds a frame well under TCP's retransmission timeout (RFC 3366):
 * a try costs at most the poll's time on the wire plus the response window.
 *
 * A TCP segment the slave sends to this host is handed to the kernel, and the
 * master waits (-w) for the kernel's answer, its ACK, before it polls again,
 * so the next poll carries it; tunnel-up.sh gives the link's route quickack.
 * Both save time, and nothing depends on them.
 *
 * Timing is the half-duplex turn: a reply must start within the slave's
 * response window (-r) after the poll has left; once it starts, its length
 * says when it ends, and it is judged then.
 *
 *   tmptun [-v] [-t s] [-w us] [-n tries] [-r ms] [-f n] [-s n] [-g us]
 *          <serial-device> [tap]
 *
 *   -v  log every exchange that failed, and print totals when stopped
 *   -t  stop after this many seconds and print totals (a measurement run)
 *   -w  after handing the host a TCP segment it must acknowledge (data, SYN
 *       or FIN, for the tap's own address), wait this long at most for its
 *       answer before polling again, in microseconds (default 5000)
 *   -n  times an exchange whose reply was damaged or missing is tried again
 *       (TMP_FLAG_RETRY) before the master gives up on it (default 4: at most
 *       5 x (31 ms for a full poll + the 100 ms window) = 0.65 s, well under
 *       TCP's 1 s retransmission timeout)
 *   -r  the slave's response window: how long after the poll has left its
 *       reply may start, in ms (default 100: a slave that writes flash before
 *       answering takes tens of ms). Never shorter than the slave's slowest
 *       answer: a poll sent while the slave is answering collides with it
 *   -f  fault injection: treat every nth intact reply of an exchange that
 *       carried a frame as damaged, so the exchange is tried again
 *   -s  fault injection: damage every nth poll that carries a frame, so the
 *       slave does not hear it and the exchange is tried again
 *   -g  hold a poll that follows a frame-carrying exchange this long, in
 *       microseconds (a diagnostic for a slave that cannot hear the bus right
 *       after its reply)
 *
 * The tap interface (default tap0) must exist: master/linux/tunnel-up.sh.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/if_tun.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
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
    unsigned long frames_down, frames_up, bytes_down, bytes_up;
    unsigned long retries, repaired, given_up, polls_damaged;
    /* the host's answers to segments handed to it */
    unsigned long host_answers, host_silent;
    long          host_min_us, host_max_us, host_sum_us;
    /* poll written -> reply complete, for exchanges with no frame */
    unsigned long empty_count;
    long          empty_min_us, empty_max_us, empty_sum_us;
} stats = { .host_min_us = LONG_MAX, .empty_min_us = LONG_MAX };

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
static enum result poll_for_reply(int serial_fd, uint8_t sequence, bool retry, bool damage,
                                  const uint8_t *down_frame, uint16_t down_length,
                                  struct tmp_scanner *scanner, struct tmp_frame *reply,
                                  struct outcome *outcome)
{
    static uint8_t wire[TMP_FRAME_MAX];
    const uint8_t flags = (uint8_t)(tmp_flags(false, 0u) | (retry ? TMP_FLAG_RETRY : 0u));
    const size_t wire_length = tmp_frame_build(wire, TMP_POLL, TMP_ADDR_COLLECTOR, sequence,
                                               flags, down_frame, down_length);
    memset(outcome, 0, sizeof *outcome);
    if (damage) {
        wire[wire_length - 1u] ^= 0xFFu;    /* a CRC the slave will not accept */
    }

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

/* ── the host's answer ──────────────────────────────────────────────── */

/* The tap's IPv4 address, host order; 0 if it has none. */
static uint32_t tap_address(const char *name)
{
    struct ifreq request;
    memset(&request, 0, sizeof request);
    snprintf(request.ifr_name, IFNAMSIZ, "%s", name);
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return 0u;
    }
    const int failed = ioctl(fd, SIOCGIFADDR, &request);
    close(fd);
    if (failed) {
        return 0u;
    }
    return ntohl(((const struct sockaddr_in *)(const void *)&request.ifr_addr)->sin_addr.s_addr);
}

/* A TCP segment for the host itself that it must acknowledge: data, a SYN or
 * a FIN. */
static bool host_must_answer(const uint8_t *frame, uint16_t length, uint32_t host)
{
    if (host == 0u || length < 54u || frame[12] != 0x08u || frame[13] != 0x00u
        || (frame[14] >> 4) != 4u || frame[23] != 6u || get_be32(&frame[30]) != host) {
        return false;
    }
    const unsigned ip_header = (frame[14] & 0x0Fu) * 4u;
    if (length < 14u + ip_header + 20u) {
        return false;
    }
    const uint8_t *tcp = &frame[14u + ip_header];
    const unsigned tcp_header = (tcp[12] >> 4) * 4u;
    const bool data = get_be16(&frame[16]) > ip_header + tcp_header;
    return data || (tcp[13] & 0x03u) != 0u;    /* FIN 0x01, SYN 0x02 */
}

/* Wait up to `limit_us` for the tap to have something for the slave; the
 * microseconds waited, or -1 if nothing came. */
static long wait_for_host(int tap_fd, long limit_us)
{
    const long started_us = monotonic_us();
    struct pollfd watch = { .fd = tap_fd, .events = POLLIN, .revents = 0 };
    const struct timespec limit = { .tv_sec = limit_us / 1000000L,
                                    .tv_nsec = (limit_us % 1000000L) * 1000L };
    if (ppoll(&watch, 1, &limit, NULL) <= 0) {
        return -1;
    }
    return monotonic_us() - started_us;
}

static void print_totals(double seconds, long gap_us)
{
    printf("%.1f s, gap %ld us: polls %lu (%.0f/s), answered %lu", seconds, gap_us, stats.polls,
           stats.polls / seconds, stats.results[ANSWERED]);
    for (int kind = SILENT; kind < RESULTS; kind++) {
        printf(", %s %lu", result_names[kind], stats.results[kind]);
    }
    printf("\n  frames down %lu (%lu B), up %lu (%lu B)\n", stats.frames_down, stats.bytes_down,
           stats.frames_up, stats.bytes_up);
    printf("  polls tried again %lu: exchanges repaired %lu, given up %lu; polls damaged on "
           "purpose %lu\n", stats.retries, stats.repaired, stats.given_up, stats.polls_damaged);
    if (stats.host_answers != 0u) {
        printf("  host answered %lu segments: min %ld us, mean %ld us, max %ld us; "
               "not within the wait %lu\n", stats.host_answers, stats.host_min_us,
               stats.host_sum_us / (long)stats.host_answers, stats.host_max_us,
               stats.host_silent);
    } else if (stats.host_silent != 0u) {
        printf("  host answered no segment within the wait (%lu)\n", stats.host_silent);
    }
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
    fprintf(stderr, "usage: tmptun [-v] [-t s] [-w us] [-n tries] [-r ms] [-f n] [-s n] [-g us] "
                    "<serial-device> [tap-interface]\n");
}

int main(int argc, char **argv)
{
    bool verbose = false;
    long run_s = 0, gap_us = 0, host_wait_us = 5000, max_tries = TMP_RETRY_LIMIT;
    unsigned long damage_every = 0u, frame_polls = 0u;
    int option;
    while ((option = getopt(argc, argv, "vt:w:n:r:f:s:g:")) != -1) {
        switch (option) {
        case 'v': verbose = true; break;
        case 't': run_s = atol(optarg); break;
        case 'w': host_wait_us = atol(optarg); break;
        case 'n': max_tries = atol(optarg); break;
        case 'r': reply_window_us = atol(optarg) * 1000L; break;
        case 'f': inject_every = strtoul(optarg, NULL, 10); break;
        case 's': damage_every = strtoul(optarg, NULL, 10); break;
        case 'g': gap_us = atol(optarg); break;
        default: usage(); return 2;
        }
    }
    if (optind >= argc || max_tries < 0 || max_tries > 255 || host_wait_us < 0
        || reply_window_us <= 0 || inject_every == 1u || damage_every == 1u) {
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
    const uint32_t host = tap_address(tap_name);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    fprintf(stderr, "tmptun master on %s <-> %s @ %u, %ld ms response window, host %u.%u.%u.%u%s\n",
            serial_device, tap_name, TMP_V2_BAUD, reply_window_us / 1000L, host >> 24,
            (host >> 16) & 0xFFu, (host >> 8) & 0xFFu, host & 0xFFu,
            host == 0u ? " (none: its answers are not waited for)" : "");
    const long longest_hold_us = (max_tries + 1L) * ((long)TMP_FRAME_MAX * BYTE_US + reply_window_us);
    if (longest_hold_us >= 1000000L) {
        fprintf(stderr, "tmptun: -n %ld with a %ld ms window can hold a frame %ld ms, past TCP's "
                        "1 s retransmission timeout\n", max_tries, reply_window_us / 1000L,
                longest_hold_us / 1000L);
    }

    struct tmp_scanner scanner = { 0 };
    tmp_scanner_reset(&scanner);
    static uint8_t down_frame[TMP_ETHERNET_FRAME_MAX];
    struct tmp_frame reply = { 0 };
    struct outcome outcome;
    uint8_t sequence = 0u;
    const long started_us = monotonic_us();
    long last_end_us = 0;
    bool last_carried = false;
    uint16_t down_length = 0u;      /* a frame for the slave, kept while the exchange is retried */
    uint8_t tries = 0u;             /* retries of the exchange in hand (tmp_retry()) */

    while (!stopping && (run_s == 0 || monotonic_us() - started_us < run_s * 1000000L)) {
        if (down_length == 0u) {
            down_length = take_frame_from_tap(tap_fd, down_frame);
        }
        if (gap_us > 0 && last_carried) {
            while (monotonic_us() - last_end_us < gap_us) {
            }
        }
        const bool damage = damage_every != 0u && down_length != 0u
                         && ++frame_polls % damage_every == 0u;
        stats.polls_damaged += damage;
        const bool retry = tries != 0u;
        const uint8_t polled_sequence = sequence;
        const enum result result = poll_for_reply(serial_fd, sequence, retry, damage,
                                                  down_length ? down_frame : NULL, down_length,
                                                  &scanner, &reply, &outcome);
        stats.results[result]++;
        bool carried = down_length != 0u;

        /* Damaged or missing: the same poll again, SEQ, frame and all, with
         * TMP_FLAG_RETRY; past the limit, the exchange is given up. */
        if (tmp_retry(&tries, result == ANSWERED, (uint8_t)max_tries)) {
            stats.retries++;
        } else {
            if (result == ANSWERED) {
                stats.repaired += retry;
                if (down_length != 0u) {
                    stats.frames_down++;
                    stats.bytes_down += down_length;
                }
            } else {
                stats.given_up++;
            }
            down_length = 0u;
            sequence++;
        }

        if (result == ANSWERED && reply.length != 0u) {
            carried = true;
            (void)!write(tap_fd, reply.payload, reply.length);
            stats.frames_up++;
            stats.bytes_up += reply.length;
            if (host_wait_us > 0 && host_must_answer(reply.payload, reply.length, host)) {
                /* Its ACK rides the next poll. */
                const long waited = wait_for_host(tap_fd, host_wait_us);
                if (waited < 0) {
                    stats.host_silent++;
                } else {
                    stats.host_answers++;
                    stats.host_sum_us += waited;
                    if (waited < stats.host_min_us) stats.host_min_us = waited;
                    if (waited > stats.host_max_us) stats.host_max_us = waited;
                }
            }
        } else if (result == ANSWERED && !carried) {
            stats.empty_count++;
            stats.empty_sum_us += outcome.reply_us;
            if (outcome.reply_us < stats.empty_min_us) stats.empty_min_us = outcome.reply_us;
            if (outcome.reply_us > stats.empty_max_us) stats.empty_max_us = outcome.reply_us;
        }

        if (result != ANSWERED && verbose) {
            printf("seq %u: %s (%s, heard %zu B", polled_sequence, result_names[result],
                   tries != 0u ? "trying again" : "given up", outcome.heard);
            for (size_t index = 0u; index < outcome.heard && index < sizeof outcome.first;
                 index++) {
                printf("%s %02x", index == 0u ? ":" : "", outcome.first[index]);
            }
            printf(")\n");
            fflush(stdout);
        }
        last_end_us = monotonic_us();
        last_carried = carried;
        /* Idle, nap; after a failure or a frame, poll again at once. */
        if (result == ANSWERED && !carried && down_length == 0u) {
            const struct timespec nap = { .tv_sec = 0, .tv_nsec = IDLE_NAP_NS };
            nanosleep(&nap, NULL);
        }
    }

    if (verbose || run_s != 0) {
        print_totals((monotonic_us() - started_us) / 1e6, gap_us);
    }
    return 0;
}
