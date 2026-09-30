/* SPDX-License-Identifier: LGPL-2.1-only */
/* Copyright (c) Kernel Labs Inc 2026. All Rights Reserved. */

#include "klsmpte2022_1/fec.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define TS_PACKET_SIZE 188u
#define TS_PER_RTP 7u
#define RTP_PAYLOAD_SIZE (TS_PACKET_SIZE * TS_PER_RTP)
#define RTP_PACKET_SIZE (S2022_RTP_HEADER_SIZE + RTP_PAYLOAD_SIZE)
#define PCR_HZ 27000000ull
#define RTP_CLOCK_HZ 90000ull

typedef struct destination {
    struct sockaddr_storage addr;
    socklen_t addr_len;
} destination;

typedef struct sender_context {
    int fd;
    destination first_fec;
    destination second_fec;
    uint64_t first_count;
    uint64_t second_count;
} sender_context;

typedef struct options {
    const char *url;
    uint64_t bitrate;
    unsigned int duration_seconds;
    unsigned int columns_l;
    unsigned int rows_d;
    unsigned int ttl;
    unsigned int source_port;
    uint32_t ssrc;
    s2022_fec_mode mode;
} options;

static void write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t mpeg_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    size_t i;

    for (i = 0; i < len; ++i) {
        int bit;

        crc ^= (uint32_t)data[i] << 24;
        for (bit = 0; bit < 8; ++bit) {
            if (crc & 0x80000000u) {
                crc = (crc << 1) ^ 0x04c11db7u;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t target_ns)
{
    for (;;) {
        uint64_t current = now_ns();
        uint64_t remaining;
        struct timespec req;

        if (current >= target_ns) {
            return;
        }

        remaining = target_ns - current;
        req.tv_sec = (time_t)(remaining / 1000000000ull);
        req.tv_nsec = (long)(remaining % 1000000000ull);
        if (nanosleep(&req, NULL) == 0 || errno != EINTR) {
            return;
        }
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s --url udp://HOST:PORT [options]\n"
            "\n"
            "Options:\n"
            "  --bitrate BPS       MPEG-TS bitrate, accepts k/m suffixes (default 20000000)\n"
            "  --fec off|a|b       Disable FEC, Level A, or Level B (default b)\n"
            "  --l COUNT           FEC columns L (default 5)\n"
            "  --d COUNT           FEC rows D (default 5)\n"
            "  --duration SEC      Run time in seconds, 0 means forever (default 0)\n"
            "  --source-port PORT  Local UDP source port, 0 means ephemeral (default 0)\n"
            "  --ttl COUNT         IPv4 multicast TTL / unicast hop limit (default 16)\n"
            "  --ssrc VALUE        RTP media SSRC in decimal or 0xhex (default 0x20220001)\n"
            "  --help              Show this help\n"
            "\n"
            "Media is sent to PORT, first-stream FEC to PORT+2, and second-stream FEC\n"
            "to PORT+4. All packets use one UDP socket and therefore one source port.\n",
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

static int parse_u32(const char *text, uint32_t *out)
{
    uint64_t value;

    if (parse_uint64(text, &value) != 0 || value > UINT32_MAX) {
        return -1;
    }

    *out = (uint32_t)value;
    return 0;
}

static int parse_options(int argc, char **argv, options *opts)
{
    int i;

    opts->url = NULL;
    opts->bitrate = 20000000ull;
    opts->duration_seconds = 0;
    opts->columns_l = 5;
    opts->rows_d = 5;
    opts->ttl = 16;
    opts->source_port = 0;
    opts->ssrc = 0x20220001u;
    opts->mode = S2022_FEC_MODE_LEVEL_B;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            return 1;
        } else if (strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            opts->url = argv[++i];
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            if (parse_uint64(argv[++i], &opts->bitrate) != 0 || opts->bitrate == 0) {
                return -1;
            }
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
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], 0, UINT_MAX, &opts->duration_seconds) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--source-port") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], 0, 65535, &opts->source_port) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--ttl") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], 0, 255, &opts->ttl) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--ssrc") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &opts->ssrc) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    if (!opts->url || opts->bitrate < 8ull * RTP_PAYLOAD_SIZE) {
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
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(service, sizeof(service), "%u", port);

    rc = getaddrinfo(host, service, &hints, &result);
    if (rc != 0 || !result) {
        fprintf(stderr, "resolve %s:%u failed: %s\n", host, port, gai_strerror(rc));
        return -1;
    }

    if (result->ai_addrlen > sizeof(dst->addr)) {
        freeaddrinfo(result);
        return -1;
    }

    memcpy(&dst->addr, result->ai_addr, result->ai_addrlen);
    dst->addr_len = (socklen_t)result->ai_addrlen;
    freeaddrinfo(result);
    return 0;
}

static void set_port(destination *dst, unsigned int port)
{
    if (dst->addr.ss_family == AF_INET) {
        ((struct sockaddr_in *)&dst->addr)->sin_port = htons((uint16_t)port);
    } else if (dst->addr.ss_family == AF_INET6) {
        ((struct sockaddr_in6 *)&dst->addr)->sin6_port = htons((uint16_t)port);
    }
}

static int create_socket(const destination *dst, unsigned int source_port, unsigned int ttl)
{
    int fd;
    int hop_limit = (int)ttl;

    fd = socket(dst->addr.ss_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    if (dst->addr.ss_family == AF_INET) {
        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons((uint16_t)source_port);
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
            perror("bind");
            close(fd);
            return -1;
        }
        (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &hop_limit, sizeof(hop_limit));
        (void)setsockopt(fd, IPPROTO_IP, IP_TTL, &hop_limit, sizeof(hop_limit));
    } else if (dst->addr.ss_family == AF_INET6) {
        struct sockaddr_in6 local6;
        memset(&local6, 0, sizeof(local6));
        local6.sin6_family = AF_INET6;
        local6.sin6_port = htons((uint16_t)source_port);
        if (bind(fd, (struct sockaddr *)&local6, sizeof(local6)) != 0) {
            perror("bind");
            close(fd);
            return -1;
        }
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hop_limit, sizeof(hop_limit));
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hop_limit, sizeof(hop_limit));
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

static void put_ts_header(uint8_t *packet, uint16_t pid, unsigned int payload_unit_start, uint8_t *continuity)
{
    packet[0] = 0x47;
    packet[1] = (uint8_t)((payload_unit_start ? 0x40u : 0x00u) | ((pid >> 8) & 0x1fu));
    packet[2] = (uint8_t)pid;
    packet[3] = (uint8_t)(0x10u | (*continuity & 0x0fu));
    *continuity = (uint8_t)((*continuity + 1u) & 0x0fu);
}

static void make_pat(uint8_t *packet, uint8_t *continuity)
{
    uint8_t section[16] = {
        0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00,
        0x00, 0x01, 0xe1, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint32_t crc = mpeg_crc32(section, 12);

    write_u32(section + 12, crc);
    memset(packet, 0xff, TS_PACKET_SIZE);
    put_ts_header(packet, 0x0000u, 1, continuity);
    packet[4] = 0x00;
    memcpy(packet + 5, section, sizeof(section));
}

static void make_pmt(uint8_t *packet, uint8_t *continuity)
{
    uint8_t section[21] = {
        0x02, 0xb0, 0x12, 0x00, 0x01, 0xc1, 0x00, 0x00,
        0xe1, 0xff, 0xf0, 0x00, 0x1b, 0xe1, 0xff, 0xf0,
        0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint32_t crc = mpeg_crc32(section, 17);

    write_u32(section + 17, crc);
    memset(packet, 0xff, TS_PACKET_SIZE);
    put_ts_header(packet, 0x0100u, 1, continuity);
    packet[4] = 0x00;
    memcpy(packet + 5, section, sizeof(section));
}

static void make_null_ts(uint8_t *packet, uint8_t *continuity)
{
    memset(packet, 0xff, TS_PACKET_SIZE);
    put_ts_header(packet, 0x1fffu, 0, continuity);
}

static void make_rtp_packet(uint8_t *packet,
                            uint16_t sequence,
                            uint32_t timestamp,
                            uint32_t ssrc,
                            uint64_t ts_index,
                            uint8_t *pat_cc,
                            uint8_t *pmt_cc,
                            uint8_t *null_cc)
{
    unsigned int i;

    memset(packet, 0, RTP_PACKET_SIZE);
    packet[0] = 0x80;
    packet[1] = S2022_RTP_PT_MP2T;
    write_u16(packet + 2, sequence);
    write_u32(packet + 4, timestamp);
    write_u32(packet + 8, ssrc);

    for (i = 0; i < TS_PER_RTP; ++i) {
        uint8_t *ts = packet + S2022_RTP_HEADER_SIZE + (i * TS_PACKET_SIZE);
        uint64_t packet_index = ts_index + i;

        if ((packet_index % 700ull) == 0ull) {
            make_pat(ts, pat_cc);
        } else if ((packet_index % 700ull) == 1ull) {
            make_pmt(ts, pmt_cc);
        } else {
            make_null_ts(ts, null_cc);
        }
    }
}

static void on_fec(void *user, s2022_fec_type type, const uint8_t *packet, size_t packet_len)
{
    sender_context *ctx = (sender_context *)user;
    const destination *dst = type == S2022_FEC_FIRST ? &ctx->first_fec : &ctx->second_fec;

    if (send_packet(ctx->fd, dst, packet, packet_len) != 0) {
        return;
    }

    if (type == S2022_FEC_FIRST) {
        ctx->first_count++;
    } else {
        ctx->second_count++;
    }
}

int main(int argc, char **argv)
{
    options opts;
    s2022_config config;
    s2022_encoder *encoder = NULL;
    sender_context ctx;
    destination media;
    char host[256];
    unsigned int base_port;
    uint8_t packet[RTP_PACKET_SIZE];
    uint8_t pat_cc = 0;
    uint8_t pmt_cc = 0;
    uint8_t null_cc = 0;
    uint64_t packet_interval_ns;
    uint64_t start_ns;
    uint64_t next_ns;
    uint64_t stop_ns;
    uint64_t media_count = 0;
    uint16_t sequence = 0;
    s2022_status status;
    int parsed;

    parsed = parse_options(argc, argv, &opts);
    if (parsed != 0) {
        usage(argv[0]);
        return parsed > 0 ? 0 : 1;
    }

    if (split_url(opts.url, host, sizeof(host), &base_port) != 0 ||
        resolve_destination(host, base_port, &media) != 0) {
        usage(argv[0]);
        return 1;
    }

    ctx.first_fec = media;
    ctx.second_fec = media;
    set_port(&ctx.first_fec, base_port + 2u);
    set_port(&ctx.second_fec, base_port + 4u);
    ctx.first_count = 0;
    ctx.second_count = 0;
    ctx.fd = create_socket(&media, opts.source_port, opts.ttl);
    if (ctx.fd < 0) {
        return 1;
    }

    s2022_config_init(&config);
    config.mode = opts.mode;
    config.columns_l = (uint8_t)opts.columns_l;
    config.rows_d = (uint8_t)opts.rows_d;
    status = s2022_encoder_create(&config, &encoder);
    if (status != S2022_OK) {
        fprintf(stderr, "encoder create failed: %s\n", s2022_status_string(status));
        close(ctx.fd);
        return 1;
    }

    packet_interval_ns = (uint64_t)((1000000000.0 * (double)(RTP_PAYLOAD_SIZE * 8u)) /
                                    (double)opts.bitrate);
    if (packet_interval_ns == 0) {
        packet_interval_ns = 1;
    }

    start_ns = now_ns();
    next_ns = start_ns;
    stop_ns = opts.duration_seconds == 0 ? UINT64_MAX :
        start_ns + ((uint64_t)opts.duration_seconds * 1000000000ull);

    fprintf(stderr,
            "Sending RTP MPEG-TS to udp://%s:%u at %" PRIu64 " b/s, FEC %s L=%u D=%u\n",
            host, base_port, opts.bitrate,
            opts.mode == S2022_FEC_MODE_DISABLED ? "off" :
            (opts.mode == S2022_FEC_MODE_LEVEL_A ? "level-a" : "level-b"),
            opts.columns_l, opts.rows_d);

    while (now_ns() < stop_ns) {
        uint32_t timestamp = (uint32_t)((media_count * TS_PER_RTP * TS_PACKET_SIZE * 8ull *
                                         RTP_CLOCK_HZ) / opts.bitrate);

        make_rtp_packet(packet, sequence, timestamp, opts.ssrc,
                        media_count * TS_PER_RTP, &pat_cc, &pmt_cc, &null_cc);

        if (send_packet(ctx.fd, &media, packet, sizeof(packet)) != 0) {
            s2022_encoder_destroy(encoder);
            close(ctx.fd);
            return 1;
        }

        status = s2022_encoder_push_rtp(encoder, packet, sizeof(packet), on_fec, &ctx);
        if (status != S2022_OK) {
            fprintf(stderr, "encoder push failed: %s\n", s2022_status_string(status));
            s2022_encoder_destroy(encoder);
            close(ctx.fd);
            return 1;
        }

        media_count++;
        sequence++;
        next_ns += packet_interval_ns;
        sleep_until(next_ns);
    }

    status = s2022_encoder_flush(encoder, on_fec, &ctx);
    if (status != S2022_OK && status != S2022_ERROR_NOT_READY) {
        fprintf(stderr, "encoder flush failed: %s\n", s2022_status_string(status));
        s2022_encoder_destroy(encoder);
        close(ctx.fd);
        return 1;
    }

    fprintf(stderr,
            "Sent %" PRIu64 " media packets, %" PRIu64 " first FEC packets, %" PRIu64 " second FEC packets\n",
            media_count, ctx.first_count, ctx.second_count);

    s2022_encoder_destroy(encoder);
    close(ctx.fd);
    return 0;
}
