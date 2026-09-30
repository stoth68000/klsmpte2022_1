/* SPDX-License-Identifier: LGPL-2.1-only */
/* Copyright (c) Kernel Labs Inc 2026. All Rights Reserved. */

#include "klsmpte2022_1/fec.h"

#include <stdio.h>
#include <string.h>

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
    (void)user;
    (void)packet;
    printf("%s FEC packet: %zu bytes\n", type == S2022_FEC_FIRST ? "first-stream" : "second-stream", packet_len);
}

static void make_demo_rtp(uint8_t *packet, uint16_t sequence)
{
    size_t i;

    memset(packet, 0, 12 + 188);
    packet[0] = 0x80;
    packet[1] = S2022_RTP_PT_MP2T;
    write_u16(packet + 2, sequence);
    write_u32(packet + 4, 90000u);
    write_u32(packet + 8, 0x12345678u);
    packet[12] = 0x47;

    for (i = 13; i < 12 + 188; ++i) {
        packet[i] = (uint8_t)(sequence + i);
    }
}

int main(void)
{
    s2022_config config;
    s2022_encoder *encoder;
    uint8_t packet[12 + 188];
    int i;
    s2022_status status;

    s2022_config_init(&config);
    config.columns_l = 5;
    config.rows_d = 5;

    status = s2022_encoder_create(&config, &encoder);
    if (status != S2022_OK) {
        fprintf(stderr, "create failed: %s\n", s2022_status_string(status));
        return 1;
    }

    for (i = 0; i < 30; ++i) {
        make_demo_rtp(packet, (uint16_t)(5000 + i));
        status = s2022_encoder_push_rtp(encoder, packet, sizeof(packet), on_fec, NULL);
        if (status != S2022_OK) {
            fprintf(stderr, "push failed: %s\n", s2022_status_string(status));
            s2022_encoder_destroy(encoder);
            return 1;
        }
    }

    status = s2022_encoder_flush(encoder, on_fec, NULL);
    if (status != S2022_OK) {
        fprintf(stderr, "flush failed: %s\n", s2022_status_string(status));
        s2022_encoder_destroy(encoder);
        return 1;
    }

    s2022_encoder_destroy(encoder);
    return 0;
}
