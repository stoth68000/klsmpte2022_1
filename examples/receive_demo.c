/* SPDX-License-Identifier: LGPL-2.1-only */
/* Copyright (c) Kernel Labs Inc 2026. All Rights Reserved. */

#include "klsmpte2022_1/fec.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_PACKET_SIZE 2048u
#define DEFAULT_LATENCY_PACKETS 256u

typedef struct destination {
    struct sockaddr_storage addr;
    socklen_t addr_len;
} destination;

typedef struct packet_slot {
    uint8_t valid;
    uint16_t sequence;
    size_t len;
    uint8_t packet[MAX_PACKET_SIZE];
} packet_slot;

typedef struct options {
    const char *url;
    const char *output_url;
    const char *interface_addr;
    unsigned int duration_seconds;
    unsigned int columns_l;
    unsigned int rows_d;
    unsigned int latency_packets;
    unsigned int socket_buffer;
    unsigned int drop_every;
    s2022_fec_mode mode;
} options;

typedef struct app_context {
    packet_slot *slots;
    size_t slot_count;
    int output_fd;
    destination output;
    uint8_t have_output;
    uint8_t have_next;
    uint8_t have_latest;
    uint16_t next_sequence;
    uint16_t latest_sequence;
    unsigned int latency_packets;
    uint64_t forwarded;
    uint64_t recovered;
    uint64_t unrecovered;
    uint64_t overwritten;
} app_context;

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static uint16_t seq_distance(uint16_t newer, uint16_t older)
{
    return (uint16_t)(newer - older);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s --url udp://GROUP_OR_IP:PORT [options]\n"
            "\n"
            "Options:\n"
            "  --fec off|a|b          Disabled, Level A, or Level B receiver (default b)\n"
            "  --l COUNT              FEC columns L (default 5)\n"
            "  --d COUNT              FEC rows D (default 5)\n"
            "  --latency PACKETS      Output repair buffer depth in RTP packets (default 256)\n"
            "  --interface ADDRESS    Local interface address for IPv4 multicast joins\n"
            "  --output udp://H:P     Forward repaired media RTP to this UDP destination\n"
            "  --duration SEC         Run time in seconds, 0 means forever (default 0)\n"
            "  --socket-buffer BYTES  Requested receive socket buffer size (default OS value)\n"
            "  --drop-every COUNT     Test aid: discard every Nth media packet before FEC\n"
            "  --help                 Show this help\n"
            "\n"
            "The receiver listens for media on PORT, first-stream FEC on PORT+2,\n"
            "and second-stream FEC on PORT+4.\n",
            argv0);
}

static int parse_uint64(const char *text, uint64_t *out)
{
    char *end = NULL;
    uint64_t multiplier = 1;
    unsigned long long value;

    if (!text || !*text) {
        return -1;
    }

    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text) {
        return -1;
    }

    if (*end == 'k' || *end == 'K') {
        multiplier = 1000ull;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        multiplier = 1000000ull;
        end++;
    }

    if (*end != '\0' || value > ULLONG_MAX / multiplier) {
        return -1;
    }

    *out = (uint64_t)value * multiplier;
    return 0;
}

static int parse_uint(const char *text, unsigned int min, unsigned int max, unsigned int *out)
{
    uint64_t value;

    if (parse_uint64(text, &value) != 0 || value < min || value > max) {
        return -1;
    }

    *out = (unsigned int)value;
    return 0;
}

static int parse_options(int argc, char **argv, options *opts)
{
    int i;

    opts->url = NULL;
    opts->output_url = NULL;
    opts->interface_addr = NULL;
    opts->duration_seconds = 0;
    opts->columns_l = 5;
    opts->rows_d = 5;
    opts->latency_packets = DEFAULT_LATENCY_PACKETS;
    opts->socket_buffer = 0;
    opts->drop_every = 0;
    opts->mode = S2022_FEC_MODE_LEVEL_B;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            return 1;
        } else if (strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            opts->url = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            opts->output_url = argv[++i];
        } else if (strcmp(argv[i], "--interface") == 0 && i + 1 < argc) {
            opts->interface_addr = argv[++i];
        } else if (strcmp(argv[i], "--fec") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "off") == 0) {
                opts->mode = S2022_FEC_MODE_DISABLED;
            } else if (strcmp(mode, "a") == 0 || strcmp(mode, "A") == 0) {
                opts->mode = S2022_FEC_MODE_LEVEL_A;
            } else if (strcmp(mode, "b") == 0 || strcmp(mode, "B") == 0) {
                opts->mode = S2022_FEC_MODE_LEVEL_B;
            } else {
                return -1;
            }
        } else if (strcmp(argv[i], "--l") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], 1, 20, &opts->columns_l) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--d") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], 4, 20, &opts->rows_d) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--latency") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], 1, 60000, &opts->latency_packets) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], 0, UINT_MAX, &opts->duration_seconds) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--socket-buffer") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], 0, INT_MAX, &opts->socket_buffer) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--drop-every") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], 0, UINT_MAX, &opts->drop_every) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    if (!opts->url) {
        return -1;
    }

    return 0;
}

static int split_url(const char *url, char *host, size_t host_size, unsigned int *port)
{
    const char *prefix = "udp://";
    const char *p = url;
    const char *last_colon;
    size_t host_len;

    if (strncmp(p, prefix, strlen(prefix)) == 0) {
        p += strlen(prefix);
    }

    last_colon = strrchr(p, ':');
    if (!last_colon || last_colon == p) {
        return -1;
    }

    host_len = (size_t)(last_colon - p);
    if (host_len == 0 || host_len >= host_size) {
        return -1;
    }

    memcpy(host, p, host_len);
    host[host_len] = '\0';
    if (parse_uint(last_colon + 1, 1, 65531, port) != 0) {
        return -1;
    }

    return 0;
}

static int resolve_destination(const char *host, unsigned int port, destination *dst)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char service[16];
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(service, sizeof(service), "%u", port);

    rc = getaddrinfo(host, service, &hints, &result);
    if (rc != 0 || !result) {
        fprintf(stderr, "resolve %s:%u failed: %s\n", host, port, gai_strerror(rc));
        return -1;
    }

    memcpy(&dst->addr, result->ai_addr, result->ai_addrlen);
    dst->addr_len = (socklen_t)result->ai_addrlen;
    freeaddrinfo(result);
    return 0;
}

static int is_multicast_ipv4(const char *host, struct in_addr *addr)
{
    uint32_t n;

    if (inet_pton(AF_INET, host, addr) != 1) {
        return 0;
    }

    n = ntohl(addr->s_addr);
    return n >= 0xe0000000u && n <= 0xefffffffu;
}

static int create_receive_socket(const char *host,
                                 unsigned int port,
                                 const char *interface_addr,
                                 unsigned int socket_buffer)
{
    int fd;
    int reuse = 1;
    struct sockaddr_in local;
    struct in_addr group_addr;
    int multicast = is_multicast_ipv4(host, &group_addr);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
    if (socket_buffer > 0) {
        int size = (int)socket_buffer;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    }

    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons((uint16_t)port);
    (void)host;
    local.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (multicast) {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = group_addr;
        if (interface_addr && inet_pton(AF_INET, interface_addr, &mreq.imr_interface) != 1) {
            fprintf(stderr, "invalid multicast interface address: %s\n", interface_addr);
            close(fd);
            return -1;
        }
        if (!interface_addr) {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        }
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            perror("IP_ADD_MEMBERSHIP");
            close(fd);
            return -1;
        }
    }

    return fd;
}

static int send_packet(int fd, const destination *dst, const uint8_t *packet, size_t packet_len)
{
    ssize_t sent = sendto(fd, packet, packet_len, 0,
                          (const struct sockaddr *)&dst->addr, dst->addr_len);
    if (sent < 0 || (size_t)sent != packet_len) {
        perror("sendto");
        return -1;
    }

    return 0;
}

static int store_output_packet(app_context *ctx, const uint8_t *packet, size_t packet_len)
{
    uint16_t sequence;
    packet_slot *slot;

    if (packet_len < S2022_RTP_HEADER_SIZE || packet_len > MAX_PACKET_SIZE) {
        return -1;
    }

    sequence = read_u16(packet + 2);
    slot = &ctx->slots[sequence % ctx->slot_count];
    if (slot->valid && slot->sequence != sequence) {
        ctx->overwritten++;
    }

    slot->valid = 1;
    slot->sequence = sequence;
    slot->len = packet_len;
    memcpy(slot->packet, packet, packet_len);

    if (!ctx->have_next) {
        ctx->next_sequence = sequence;
        ctx->have_next = 1;
    }
    if (!ctx->have_latest || seq_distance(sequence, ctx->latest_sequence) < 32768u) {
        ctx->latest_sequence = sequence;
        ctx->have_latest = 1;
    }

    return 0;
}

static void drain_output(app_context *ctx)
{
    while (ctx->have_next && ctx->have_latest) {
        packet_slot *slot = &ctx->slots[ctx->next_sequence % ctx->slot_count];

        if (slot->valid && slot->sequence == ctx->next_sequence) {
            if (ctx->have_output) {
                (void)send_packet(ctx->output_fd, &ctx->output, slot->packet, slot->len);
            }
            slot->valid = 0;
            ctx->forwarded++;
            ctx->next_sequence++;
            continue;
        }

        if (seq_distance(ctx->latest_sequence, ctx->next_sequence) < 32768u &&
            seq_distance(ctx->latest_sequence, ctx->next_sequence) >= ctx->latency_packets) {
            ctx->unrecovered++;
            ctx->next_sequence++;
            continue;
        }

        break;
    }
}

static void on_recovered(void *user, const uint8_t *packet, size_t packet_len)
{
    app_context *ctx = (app_context *)user;

    if (store_output_packet(ctx, packet, packet_len) == 0) {
        ctx->recovered++;
        drain_output(ctx);
    }
}

int main(int argc, char **argv)
{
    options opts;
    s2022_config config;
    s2022_receiver *receiver = NULL;
    app_context ctx;
    char host[256];
    unsigned int base_port;
    int media_fd;
    int first_fd;
    int second_fd;
    int max_fd;
    uint8_t packet[MAX_PACKET_SIZE];
    uint64_t media_packets = 0;
    uint64_t first_fec = 0;
    uint64_t second_fec = 0;
    uint64_t simulated_drops = 0;
    uint64_t start_ns;
    uint64_t stop_ns;
    int parsed;
    s2022_status status;

    parsed = parse_options(argc, argv, &opts);
    if (parsed != 0) {
        usage(argv[0]);
        return parsed > 0 ? 0 : 1;
    }

    if (split_url(opts.url, host, sizeof(host), &base_port) != 0) {
        usage(argv[0]);
        return 1;
    }

    media_fd = create_receive_socket(host, base_port, opts.interface_addr, opts.socket_buffer);
    first_fd = create_receive_socket(host, base_port + 2u, opts.interface_addr, opts.socket_buffer);
    second_fd = create_receive_socket(host, base_port + 4u, opts.interface_addr, opts.socket_buffer);
    if (media_fd < 0 || first_fd < 0 || second_fd < 0) {
        if (media_fd >= 0) {
            close(media_fd);
        }
        if (first_fd >= 0) {
            close(first_fd);
        }
        if (second_fd >= 0) {
            close(second_fd);
        }
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.slot_count = (size_t)opts.latency_packets + 1024u;
    if (ctx.slot_count < 4096u) {
        ctx.slot_count = 4096u;
    }
    ctx.slots = (packet_slot *)calloc(ctx.slot_count, sizeof(*ctx.slots));
    ctx.output_fd = -1;
    ctx.latency_packets = opts.latency_packets;
    if (!ctx.slots) {
        fprintf(stderr, "out of memory\n");
        close(media_fd);
        close(first_fd);
        close(second_fd);
        return 1;
    }

    if (opts.output_url) {
        char output_host[256];
        unsigned int output_port;

        if (split_url(opts.output_url, output_host, sizeof(output_host), &output_port) != 0 ||
            resolve_destination(output_host, output_port, &ctx.output) != 0) {
            free(ctx.slots);
            close(media_fd);
            close(first_fd);
            close(second_fd);
            return 1;
        }
        ctx.output_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (ctx.output_fd < 0) {
            perror("socket output");
            free(ctx.slots);
            close(media_fd);
            close(first_fd);
            close(second_fd);
            return 1;
        }
        ctx.have_output = 1;
    }

    s2022_config_init(&config);
    config.mode = opts.mode;
    config.columns_l = (uint8_t)opts.columns_l;
    config.rows_d = (uint8_t)opts.rows_d;
    status = s2022_receiver_create(&config, &receiver);
    if (status != S2022_OK) {
        fprintf(stderr, "receiver create failed: %s\n", s2022_status_string(status));
        free(ctx.slots);
        close(media_fd);
        close(first_fd);
        close(second_fd);
        if (ctx.output_fd >= 0) {
            close(ctx.output_fd);
        }
        return 1;
    }

    max_fd = media_fd;
    if (first_fd > max_fd) {
        max_fd = first_fd;
    }
    if (second_fd > max_fd) {
        max_fd = second_fd;
    }

    start_ns = now_ns();
    stop_ns = opts.duration_seconds == 0 ? UINT64_MAX :
        start_ns + ((uint64_t)opts.duration_seconds * 1000000000ull);

    fprintf(stderr,
            "Receiving RTP MPEG-TS from udp://%s:%u, FEC %s L=%u D=%u, latency=%u packets\n",
            host, base_port,
            opts.mode == S2022_FEC_MODE_DISABLED ? "off" :
            (opts.mode == S2022_FEC_MODE_LEVEL_A ? "level-a" : "level-b"),
            opts.columns_l, opts.rows_d, opts.latency_packets);

    while (now_ns() < stop_ns) {
        fd_set read_fds;
        struct timeval timeout;
        int ready;

        FD_ZERO(&read_fds);
        FD_SET(media_fd, &read_fds);
        FD_SET(first_fd, &read_fds);
        FD_SET(second_fd, &read_fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;

        ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        if (ready == 0) {
            drain_output(&ctx);
            continue;
        }

        if (FD_ISSET(media_fd, &read_fds)) {
            ssize_t n = recv(media_fd, packet, sizeof(packet), 0);
            if (n > 0) {
                media_packets++;
                if (opts.drop_every > 0 && (media_packets % opts.drop_every) == 0u) {
                    simulated_drops++;
                } else {
                    status = s2022_receiver_push_media(receiver, packet, (size_t)n);
                    if (status == S2022_OK) {
                        (void)store_output_packet(&ctx, packet, (size_t)n);
                    }
                }
            }
        }

        if (FD_ISSET(first_fd, &read_fds)) {
            ssize_t n = recv(first_fd, packet, sizeof(packet), 0);
            if (n > 0) {
                first_fec++;
                (void)s2022_receiver_push_fec(receiver, packet, (size_t)n, on_recovered, &ctx);
            }
        }

        if (FD_ISSET(second_fd, &read_fds)) {
            ssize_t n = recv(second_fd, packet, sizeof(packet), 0);
            if (n > 0) {
                second_fec++;
                (void)s2022_receiver_push_fec(receiver, packet, (size_t)n, on_recovered, &ctx);
            }
        }

        drain_output(&ctx);
    }

    fprintf(stderr,
            "Received %" PRIu64 " media packets, %" PRIu64 " first FEC, %" PRIu64 " second FEC\n"
            "Recovered %" PRIu64 ", forwarded %" PRIu64 ", unrecovered gaps %" PRIu64,
            media_packets, first_fec, second_fec,
            ctx.recovered, ctx.forwarded, ctx.unrecovered);
    if (opts.drop_every > 0) {
        fprintf(stderr, ", simulated drops %" PRIu64, simulated_drops);
    }
    fprintf(stderr, "\n");

    s2022_receiver_destroy(receiver);
    free(ctx.slots);
    close(media_fd);
    close(first_fd);
    close(second_fd);
    if (ctx.output_fd >= 0) {
        close(ctx.output_fd);
    }
    return 0;
}
