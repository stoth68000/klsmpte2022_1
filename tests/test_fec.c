/* SPDX-License-Identifier: LGPL-2.1-only */
/* Copyright (c) Kernel Labs Inc 2026. All Rights Reserved. */

#include "klsmpte2022_1/fec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct captured {
    uint8_t packets[32][2048];
    size_t lens[32];
    s2022_fec_type types[32];
    size_t count;
} captured;

typedef struct recovered_capture {
    uint8_t packet[2048];
    size_t len;
    size_t count;
} recovered_capture;

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

static void on_fec(void *user, s2022_fec_type type, const uint8_t *packet, size_t packet_len)
{
    captured *cap = (captured *)user;

    if (cap->count >= 16 || packet_len > sizeof(cap->packets[0])) {
        abort();
    }

    cap->types[cap->count] = type;
    cap->lens[cap->count] = packet_len;
    memcpy(cap->packets[cap->count], packet, packet_len);
    cap->count++;
}

static void on_recovered(void *user, const uint8_t *packet, size_t packet_len)
{
    recovered_capture *cap = (recovered_capture *)user;

    if (packet_len > sizeof(cap->packet)) {
        abort();
    }

    memcpy(cap->packet, packet, packet_len);
    cap->len = packet_len;
    cap->count++;
}

static void make_rtp(uint8_t *packet, uint16_t sequence, uint32_t timestamp, uint8_t seed)
{
    size_t i;

    memset(packet, 0, 12 + 188);
    packet[0] = 0x80;
    packet[1] = S2022_RTP_PT_MP2T;
    write_u16(packet + 2, sequence);
    write_u32(packet + 4, timestamp);
    write_u32(packet + 8, 0x10203040u);

    packet[12] = 0x47;
    for (i = 13; i < 12 + 188; ++i) {
        packet[i] = (uint8_t)(seed + i);
    }
}

static size_t make_rtp_ext(uint8_t *packet, uint16_t sequence, uint32_t timestamp, uint8_t seed, uint16_t words)
{
    size_t i;
    size_t payload_offset = 12u + 4u + ((size_t)words * 4u);

    memset(packet, 0, payload_offset + 188u);
    packet[0] = 0x90;
    packet[1] = S2022_RTP_PT_MP2T;
    write_u16(packet + 2, sequence);
    write_u32(packet + 4, timestamp);
    write_u32(packet + 8, 0x10203040u);
    write_u16(packet + 12, 0x1000u);
    write_u16(packet + 14, words);

    for (i = 0; i < (size_t)words * 4u; ++i) {
        packet[16 + i] = (uint8_t)(0xa0u + i);
    }

    packet[payload_offset] = 0x47;
    for (i = payload_offset + 1u; i < payload_offset + 188u; ++i) {
        packet[i] = (uint8_t)(seed + i);
    }

    return payload_offset + 188u;
}

static int expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

int main(void)
{
    s2022_config config;
    s2022_encoder *encoder = NULL;
    s2022_receiver *receiver = NULL;
    captured cap;
    recovered_capture rcap;
    uint8_t media[32][12 + 188];
    uint8_t media_ext[8][12 + 4 + 4 + 188];
    uint8_t expected[12 + 188];
    s2022_media_packet row_media[4];
    uint8_t recovered[12 + 188];
    size_t recovered_len = 0;
    s2022_status status;
    s2022_receiver_stats stats;
    size_t i;

    memset(&cap, 0, sizeof(cap));
    s2022_config_init(&config);
    config.columns_l = 4;
    config.rows_d = 4;

    status = s2022_encoder_create(&config, &encoder);
    if (expect(status == S2022_OK, "encoder create")) {
        return 1;
    }

    for (i = 0; i < 16; ++i) {
        make_rtp(media[i], (uint16_t)(1000 + i), 90000u + (uint32_t)i, (uint8_t)i);
        status = s2022_encoder_push_rtp(encoder, media[i], sizeof(media[i]), on_fec, &cap);
        if (expect(status == S2022_OK, "push rtp")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }

    if (expect(cap.count == 4, "four second-stream FEC packets emitted immediately")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(cap.types[0] == S2022_FEC_SECOND, "first packet is second-stream FEC")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }

    for (i = 0; i < 4; ++i) {
        row_media[i].data = media[i];
        row_media[i].len = sizeof(media[i]);
    }
    row_media[2].data = NULL;
    row_media[2].len = 0;

    if (expect(cap.lens[0] == S2022_FEC_PACKET_OVERHEAD + 188, "even-NA FEC carries payload recovery bytes")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }

    status = s2022_recover_media_packet(cap.packets[0], cap.lens[0],
                                        row_media, 4, 2,
                                        recovered, sizeof(recovered), &recovered_len);
    if (expect(status == S2022_OK, "recover missing row packet")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(recovered_len == sizeof(media[2]), "recovered packet length")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(memcmp(recovered, media[2], sizeof(media[2])) == 0, "recovered packet bytes")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }

    memset(&rcap, 0, sizeof(rcap));
    status = s2022_receiver_create(&config, &receiver);
    if (expect(status == S2022_OK, "receiver create")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    (void)s2022_receiver_push_media(receiver, media[0], sizeof(media[0]));
    (void)s2022_receiver_push_media(receiver, media[1], sizeof(media[1]));
    (void)s2022_receiver_push_media(receiver, media[3], sizeof(media[3]));
    status = s2022_receiver_push_fec(receiver, cap.packets[0], cap.lens[0],
                                     on_recovered, &rcap);
    if (expect(status == S2022_OK, "receiver recover from row FEC")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(rcap.count == 1, "receiver callback count")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(rcap.len == sizeof(media[2]) &&
               memcmp(rcap.packet, media[2], sizeof(media[2])) == 0,
               "receiver recovered packet bytes")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    status = s2022_receiver_get_stats(receiver, &stats);
    if (expect(status == S2022_OK, "receiver stats query")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(stats.reset_time != (time_t)0 && stats.sampled_time >= stats.reset_time,
               "receiver stats timestamps")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(stats.media_packets_processed == 3u &&
               stats.media_bytes_processed == 3u * (uint64_t)sizeof(media[0]),
               "receiver media byte stats")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(stats.fec_packets_processed == 1u &&
               stats.fec_bytes_processed == (uint64_t)cap.lens[0],
               "receiver FEC byte stats")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(stats.recovery_attempts == 1u &&
               stats.recovered_packets == 1u &&
               stats.recovered_bytes == (uint64_t)sizeof(media[2]) &&
               stats.recovery_failed_packets == 0u &&
               stats.recovery_error_rate == 0.0,
               "receiver recovery stats")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    s2022_receiver_reset_stats(receiver);
    status = s2022_receiver_get_stats(receiver, &stats);
    if (expect(status == S2022_OK &&
               stats.media_packets_processed == 0u &&
               stats.media_bytes_processed == 0u &&
               stats.fec_packets_processed == 0u &&
               stats.fec_bytes_processed == 0u &&
               stats.recovery_attempts == 0u &&
               stats.recovered_packets == 0u &&
               stats.recovered_bytes == 0u &&
               stats.recovery_failed_packets == 0u &&
               stats.recovery_error_rate == 0.0,
               "receiver stats reset")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    s2022_receiver_destroy(receiver);
    receiver = NULL;

    for (i = 16; i < 20; ++i) {
        make_rtp(media[i], (uint16_t)(1000 + i), 90000u + (uint32_t)i, (uint8_t)i);
        status = s2022_encoder_push_rtp(encoder, media[i], sizeof(media[i]), on_fec, &cap);
        if (expect(status == S2022_OK, "push next-block rtp")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }

    if (expect(cap.count == 6, "first-stream FEC waits L media packets")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(cap.types[4] == S2022_FEC_FIRST, "first delayed packet is first-stream FEC")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(cap.types[5] == S2022_FEC_SECOND, "next block second stream follows delayed first stream")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    for (i = 20; i < 23; ++i) {
        make_rtp(media[i], (uint16_t)(1000 + i), 90000u + (uint32_t)i, (uint8_t)i);
        status = s2022_encoder_push_rtp(encoder, media[i], sizeof(media[i]), on_fec, &cap);
        if (expect(status == S2022_OK, "push more next-block rtp")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }
    if (expect(cap.count == 9 && cap.types[8] == S2022_FEC_FIRST,
               "remaining delayed first-stream FEC emitted after delay")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    s2022_encoder_destroy(encoder);

    memset(&cap, 0, sizeof(cap));
    s2022_config_init(&config);
    config.columns_l = 4;
    config.rows_d = 4;
    status = s2022_encoder_create(&config, &encoder);
    if (expect(status == S2022_OK, "flush encoder create")) {
        return 1;
    }
    for (i = 0; i < 16; ++i) {
        make_rtp(media[i], (uint16_t)(1500 + i), 90500u + (uint32_t)i, (uint8_t)i);
        status = s2022_encoder_push_rtp(encoder, media[i], sizeof(media[i]), on_fec, &cap);
        if (expect(status == S2022_OK, "flush test push")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }
    if (expect(cap.count == 4, "flush test has queued first-stream FEC")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    status = s2022_encoder_flush(encoder, on_fec, &cap);
    if (expect(status == S2022_ERROR_NOT_READY, "flush rejects early first-stream FEC")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(cap.count == 4, "early flush emits no first-stream FEC")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    for (i = 16; i < 20; ++i) {
        make_rtp(media[i], (uint16_t)(1500 + i), 90500u + (uint32_t)i, (uint8_t)i);
        status = s2022_encoder_push_rtp(encoder, media[i], sizeof(media[i]), on_fec, &cap);
        if (expect(status == S2022_OK, "flush delay push")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }
    if (expect(cap.count == 6, "delay elapsed emits first queued column and row FEC")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    status = s2022_encoder_flush(encoder, on_fec, &cap);
    if (expect(status == S2022_OK, "flush queued first-stream FEC after delay")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(cap.count == 9 && cap.types[6] == S2022_FEC_FIRST && cap.types[8] == S2022_FEC_FIRST,
               "flush packets after delay are first-stream FEC")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }

    s2022_encoder_destroy(encoder);

    memset(&cap, 0, sizeof(cap));
    s2022_config_init(&config);
    config.mode = S2022_FEC_MODE_LEVEL_A;
    config.columns_l = 1;
    config.rows_d = 4;
    status = s2022_encoder_create(&config, &encoder);
    if (expect(status == S2022_OK, "level A encoder create with L=1")) {
        return 1;
    }
    for (i = 0; i < 4; ++i) {
        make_rtp(media[i], (uint16_t)(2000 + i), 91000u + (uint32_t)i, (uint8_t)i);
        status = s2022_encoder_push_rtp(encoder, media[i], sizeof(media[i]), on_fec, &cap);
        if (expect(status == S2022_OK, "level A push")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }
    if (expect(cap.count == 0, "level A queues first FEC stream at block boundary")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    make_rtp(media[4], 2004, 91004u, 4);
    status = s2022_encoder_push_rtp(encoder, media[4], sizeof(media[4]), on_fec, &cap);
    if (expect(status == S2022_OK, "level A delayed first-stream push")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    if (expect(cap.count == 1 && cap.types[0] == S2022_FEC_FIRST,
               "level A flush emits first FEC stream")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    s2022_encoder_destroy(encoder);

    memset(&cap, 0, sizeof(cap));
    s2022_config_init(&config);
    config.mode = S2022_FEC_MODE_DISABLED;
    config.columns_l = 4;
    config.rows_d = 4;
    status = s2022_encoder_create(&config, &encoder);
    if (expect(status == S2022_OK, "disabled encoder create")) {
        return 1;
    }
    for (i = 0; i < 8; ++i) {
        make_rtp(media[i], (uint16_t)(3000 + i), 92000u + (uint32_t)i, (uint8_t)i);
        status = s2022_encoder_push_rtp(encoder, media[i], sizeof(media[i]), on_fec, &cap);
        if (expect(status == S2022_OK, "disabled push")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }
    if (expect(cap.count == 0, "disabled mode emits no FEC")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    s2022_encoder_destroy(encoder);

    s2022_config_init(&config);
    config.columns_l = 4;
    config.rows_d = 4;
    status = s2022_encoder_create(&config, &encoder);
    if (expect(status == S2022_OK, "sequence test encoder create")) {
        return 1;
    }
    make_rtp(media[0], 4000, 93000u, 0);
    make_rtp(media[1], 4002, 93001u, 1);
    status = s2022_encoder_push_rtp(encoder, media[0], sizeof(media[0]), NULL, NULL);
    if (expect(status == S2022_OK, "sequence first push")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    status = s2022_encoder_push_rtp(encoder, media[1], sizeof(media[1]), NULL, NULL);
    if (expect(status == S2022_ERROR_INVALID_PACKET, "sequence gap rejected")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    s2022_encoder_destroy(encoder);

    memset(&cap, 0, sizeof(cap));
    s2022_config_init(&config);
    config.columns_l = 4;
    config.rows_d = 4;
    status = s2022_encoder_create(&config, &encoder);
    if (expect(status == S2022_OK, "extension encoder create")) {
        return 1;
    }
    for (i = 0; i < 4; ++i) {
        size_t len = make_rtp_ext(media_ext[i], (uint16_t)(5000 + i),
                                  94000u + (uint32_t)i, (uint8_t)i, 1);
        status = s2022_encoder_push_rtp(encoder, media_ext[i], len, on_fec, &cap);
        if (expect(status == S2022_OK, "extension push")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }
    if (expect(cap.count == 1 && cap.lens[0] == S2022_FEC_PACKET_OVERHEAD + 188,
               "extension payload recovery excludes RTP extension")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    memset(&rcap, 0, sizeof(rcap));
    status = s2022_receiver_create(&config, &receiver);
    if (expect(status == S2022_OK, "extension receiver create")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    (void)s2022_receiver_push_media(receiver, media_ext[0], 12u + 4u + 4u + 188u);
    (void)s2022_receiver_push_media(receiver, media_ext[1], 12u + 4u + 4u + 188u);
    (void)s2022_receiver_push_media(receiver, media_ext[3], 12u + 4u + 4u + 188u);
    status = s2022_receiver_push_fec(receiver, cap.packets[0], cap.lens[0],
                                     on_recovered, &rcap);
    if (expect(status == S2022_OK, "extension receiver recovery")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    memcpy(expected, media_ext[2], S2022_RTP_HEADER_SIZE);
    expected[0] = (uint8_t)(expected[0] & ~0x10u);
    memcpy(expected + S2022_RTP_HEADER_SIZE, media_ext[2] + 20, 188u);
    if (expect(rcap.count == 1 && rcap.len == sizeof(expected) &&
               memcmp(rcap.packet, expected, sizeof(expected)) == 0,
               "extension receiver recovered normalized packet")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    cap.packets[0][24] = (uint8_t)(cap.packets[0][24] | 0x10u);
    rcap.count = 0;
    status = s2022_receiver_push_fec(receiver, cap.packets[0], cap.lens[0],
                                     on_recovered, &rcap);
    if (expect(status == S2022_OK && rcap.count == 0,
               "receiver ignores unsupported FEC type")) {
        s2022_receiver_destroy(receiver);
        s2022_encoder_destroy(encoder);
        return 1;
    }
    s2022_receiver_destroy(receiver);
    receiver = NULL;
    {
        size_t len = make_rtp_ext(media_ext[4], 5004, 94004u, 4, 2);
        status = s2022_encoder_push_rtp(encoder, media_ext[4], len, on_fec, &cap);
        if (expect(status == S2022_ERROR_UNSUPPORTED, "extension length change rejected")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }
    s2022_encoder_reset(encoder);
    {
        size_t len = make_rtp_ext(media_ext[4], 7000, 96000u, 4, 2);
        status = s2022_encoder_push_rtp(encoder, media_ext[4], len, on_fec, &cap);
        if (expect(status == S2022_OK, "encoder reset starts new extension session")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }
    s2022_encoder_destroy(encoder);

    s2022_config_init(&config);
    config.columns_l = 4;
    config.rows_d = 4;
    status = s2022_receiver_create(&config, &receiver);
    if (expect(status == S2022_OK, "receiver reset create")) {
        return 1;
    }
    {
        size_t len = make_rtp_ext(media_ext[0], 8000, 97000u, 0, 1);
        status = s2022_receiver_push_media(receiver, media_ext[0], len);
        if (expect(status == S2022_OK, "receiver reset first session media")) {
            s2022_receiver_destroy(receiver);
            return 1;
        }
    }
    {
        size_t len = make_rtp_ext(media_ext[1], 8001, 97001u, 1, 2);
        status = s2022_receiver_push_media(receiver, media_ext[1], len);
        if (expect(status == S2022_ERROR_UNSUPPORTED, "receiver rejects extension change before reset")) {
            s2022_receiver_destroy(receiver);
            return 1;
        }
    }
    s2022_receiver_reset(receiver);
    {
        size_t len = make_rtp_ext(media_ext[1], 9000, 98000u, 1, 2);
        status = s2022_receiver_push_media(receiver, media_ext[1], len);
        if (expect(status == S2022_OK, "receiver reset starts new extension session")) {
            s2022_receiver_destroy(receiver);
            return 1;
        }
    }
    s2022_receiver_destroy(receiver);
    receiver = NULL;

    memset(&cap, 0, sizeof(cap));
    memset(&rcap, 0, sizeof(rcap));
    s2022_config_init(&config);
    config.columns_l = 4;
    config.rows_d = 4;
    status = s2022_encoder_create(&config, &encoder);
    if (expect(status == S2022_OK, "level A second-stream source encoder create")) {
        return 1;
    }
    for (i = 0; i < 4; ++i) {
        make_rtp(media[i], (uint16_t)(6000 + i), 95000u + (uint32_t)i, (uint8_t)i);
        status = s2022_encoder_push_rtp(encoder, media[i], sizeof(media[i]), on_fec, &cap);
        if (expect(status == S2022_OK, "level A second-stream source push")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }
    s2022_encoder_destroy(encoder);

    s2022_config_init(&config);
    config.mode = S2022_FEC_MODE_LEVEL_A;
    config.columns_l = 4;
    config.rows_d = 4;
    status = s2022_receiver_create(&config, &receiver);
    if (expect(status == S2022_OK, "level A receiver create")) {
        return 1;
    }
    (void)s2022_receiver_push_media(receiver, media[0], sizeof(media[0]));
    (void)s2022_receiver_push_media(receiver, media[1], sizeof(media[1]));
    (void)s2022_receiver_push_media(receiver, media[3], sizeof(media[3]));
    status = s2022_receiver_push_fec(receiver, cap.packets[0], cap.lens[0],
                                     on_recovered, &rcap);
    if (expect(status == S2022_OK, "level A receiver auto processes second stream")) {
        s2022_receiver_destroy(receiver);
        return 1;
    }
    if (expect(rcap.count == 1 &&
               memcmp(rcap.packet, media[2], sizeof(media[2])) == 0,
               "level A receiver recovered from second stream")) {
        s2022_receiver_destroy(receiver);
        return 1;
    }
    s2022_receiver_destroy(receiver);

    memset(&cap, 0, sizeof(cap));
    s2022_config_init(&config);
    config.columns_l = 4;
    config.rows_d = 4;
    status = s2022_encoder_create(&config, &encoder);
    if (expect(status == S2022_OK, "geometry source encoder create")) {
        return 1;
    }
    for (i = 0; i < 20; ++i) {
        make_rtp(media[i], (uint16_t)(10000 + i), 99000u + (uint32_t)i, (uint8_t)i);
        status = s2022_encoder_push_rtp(encoder, media[i], sizeof(media[i]), on_fec, &cap);
        if (expect(status == S2022_OK, "geometry source push")) {
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }
    status = s2022_encoder_flush(encoder, on_fec, &cap);
    if (expect(status == S2022_OK, "geometry source flush")) {
        s2022_encoder_destroy(encoder);
        return 1;
    }
    s2022_encoder_destroy(encoder);

    status = s2022_receiver_create(&config, &receiver);
    if (expect(status == S2022_OK, "geometry receiver create")) {
        return 1;
    }
    {
        uint8_t bad_fec[2048];

        memcpy(bad_fec, cap.packets[0], cap.lens[0]);
        bad_fec[25] = 2u;
        status = s2022_receiver_push_fec(receiver, bad_fec, cap.lens[0],
                                         on_recovered, &rcap);
        if (expect(status == S2022_ERROR_INVALID_PACKET,
                   "receiver rejects bad second-stream offset")) {
            s2022_receiver_destroy(receiver);
            return 1;
        }

        memcpy(bad_fec, cap.packets[4], cap.lens[4]);
        bad_fec[26] = 3u;
        status = s2022_receiver_push_fec(receiver, bad_fec, cap.lens[4],
                                         on_recovered, &rcap);
        if (expect(status == S2022_ERROR_INVALID_PACKET,
                   "receiver rejects bad first-stream NA")) {
            s2022_receiver_destroy(receiver);
            return 1;
        }
    }
    s2022_receiver_destroy(receiver);

    return 0;
}
