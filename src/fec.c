/* SPDX-License-Identifier: LGPL-2.1-only */
/* Copyright (c) Kernel Labs Inc 2026. All Rights Reserved. */

#include "klsmpte2022_1/fec.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_MSC_VER)
#include <windows.h>
#endif

#define S2022_MAX_BLOCK_PACKETS 100u
#define S2022_MIN_L 4u
#define S2022_MAX_L 20u
#define S2022_MIN_D 4u
#define S2022_MAX_D 20u

typedef struct s2022_work_packet {
    uint16_t sequence;
    uint32_t timestamp;
    uint8_t *bitstring;
    size_t payload_offset;
} s2022_work_packet;

typedef struct s2022_store_entry {
    uint16_t sequence;
    uint8_t valid;
    uint8_t *packet;
    size_t packet_len;
    uint64_t stored_at;
} s2022_store_entry;

struct s2022_encoder {
    s2022_config config;
    size_t packet_len;
    size_t protected_len;
    size_t bitstring_len;
    size_t packet_count;
    s2022_work_packet *packets;
    uint8_t *storage;
    uint8_t *fec_buffer;
    uint8_t *pending_columns;
    size_t *pending_column_lens;
    size_t pending_column_count;
    size_t pending_column_next;
    size_t pending_column_delay;
    uint8_t have_expected_sequence;
    uint16_t expected_sequence;
    uint8_t have_extension_profile;
    uint8_t extension_enabled;
    uint16_t extension_profile;
    uint16_t extension_words;
};

struct s2022_receiver {
    s2022_config config;
    size_t packet_len;
    size_t store_capacity;
    s2022_store_entry *store;
    uint8_t *store_packets;
    uint64_t store_counter;
    uint8_t have_extension_profile;
    uint8_t extension_enabled;
    uint16_t extension_profile;
    uint16_t extension_words;
    uint8_t have_level_a_stream;
    s2022_fec_type level_a_stream;
    s2022_receiver_stats stats;
};

static void stats_add_u64(uint64_t *value, uint64_t delta)
{
#if defined(_MSC_VER)
    InterlockedExchangeAdd64((volatile LONG64 *)value, (LONG64)delta);
#else
    __atomic_add_fetch(value, delta, __ATOMIC_RELAXED);
#endif
}

static uint64_t stats_load_u64(const uint64_t *value)
{
#if defined(_MSC_VER)
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_RELAXED);
#endif
}

static void stats_store_u64(uint64_t *value, uint64_t new_value)
{
#if defined(_MSC_VER)
    InterlockedExchange64((volatile LONG64 *)value, (LONG64)new_value);
#else
    __atomic_store_n(value, new_value, __ATOMIC_RELAXED);
#endif
}

static time_t stats_load_time(const time_t *value)
{
#if defined(_MSC_VER)
    if (sizeof(time_t) == sizeof(LONG64)) {
        return (time_t)InterlockedCompareExchange64((volatile LONG64 *)value, 0, 0);
    }
    return (time_t)InterlockedCompareExchange((volatile LONG *)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_RELAXED);
#endif
}

static void stats_store_time(time_t *value, time_t new_value)
{
#if defined(_MSC_VER)
    if (sizeof(time_t) == sizeof(LONG64)) {
        InterlockedExchange64((volatile LONG64 *)value, (LONG64)new_value);
    } else {
        InterlockedExchange((volatile LONG *)value, (LONG)new_value);
    }
#else
    __atomic_store_n(value, new_value, __ATOMIC_RELAXED);
#endif
}

static void stats_reset_receiver(s2022_receiver_stats *stats)
{
    time_t now = time(NULL);

    stats_store_u64(&stats->media_packets_processed, 0);
    stats_store_u64(&stats->media_bytes_processed, 0);
    stats_store_u64(&stats->fec_packets_processed, 0);
    stats_store_u64(&stats->fec_bytes_processed, 0);
    stats_store_u64(&stats->recovery_attempts, 0);
    stats_store_u64(&stats->recovery_deferred_packets, 0);
    stats_store_u64(&stats->recovered_packets, 0);
    stats_store_u64(&stats->recovered_bytes, 0);
    stats_store_u64(&stats->recovery_failed_packets, 0);
    stats_store_time(&stats->reset_time, now);
    stats_store_time(&stats->sampled_time, now);
    stats->recovery_error_rate = 0.0;
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

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

static void xor_into(uint8_t *dst, const uint8_t *src, size_t len)
{
    size_t i;

    for (i = 0; i < len; ++i) {
        dst[i] ^= src[i];
    }
}

static s2022_status validate_config(const s2022_config *config)
{
    unsigned int product;

    if (!config) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    if (config->mode != S2022_FEC_MODE_DISABLED &&
        config->mode != S2022_FEC_MODE_LEVEL_A &&
        config->mode != S2022_FEC_MODE_LEVEL_B) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    if (config->receiver_stream != S2022_RECEIVER_STREAM_AUTO &&
        config->receiver_stream != S2022_RECEIVER_STREAM_FIRST &&
        config->receiver_stream != S2022_RECEIVER_STREAM_SECOND) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    if (config->columns_l < 1u || config->columns_l > S2022_MAX_L ||
        config->rows_d < S2022_MIN_D || config->rows_d > S2022_MAX_D) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    if (config->mode == S2022_FEC_MODE_LEVEL_B && config->columns_l < S2022_MIN_L) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    product = (unsigned int)config->columns_l * (unsigned int)config->rows_d;
    if (product > S2022_MAX_BLOCK_PACKETS) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    if (config->fec_payload_type != S2022_DEFAULT_FEC_PT || config->fec_ssrc != 0u) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    return S2022_OK;
}

static s2022_status parse_rtp_packet(const uint8_t *packet,
                                     size_t len,
                                     uint8_t *extension_enabled,
                                     uint16_t *extension_profile,
                                     uint16_t *extension_words,
                                     size_t *payload_offset)
{
    size_t offset;

    if (!packet || len < S2022_RTP_HEADER_SIZE || len > (size_t)UINT16_MAX + S2022_RTP_HEADER_SIZE) {
        return S2022_ERROR_INVALID_PACKET;
    }

    if ((packet[0] & 0xc0u) != 0x80u) {
        return S2022_ERROR_INVALID_PACKET;
    }

    if ((packet[0] & 0x0fu) != 0u) {
        return S2022_ERROR_UNSUPPORTED;
    }

    offset = S2022_RTP_HEADER_SIZE;
    *extension_enabled = (uint8_t)((packet[0] & 0x10u) != 0u);
    *extension_profile = 0;
    *extension_words = 0;

    if (*extension_enabled) {
        size_t extension_bytes;

        if (len < offset + 4u) {
            return S2022_ERROR_INVALID_PACKET;
        }

        *extension_profile = read_u16(packet + offset);
        *extension_words = read_u16(packet + offset + 2u);
        extension_bytes = (size_t)*extension_words * 4u;
        offset += 4u;
        if (len < offset + extension_bytes) {
            return S2022_ERROR_INVALID_PACKET;
        }
        offset += extension_bytes;
    }

    if (payload_offset) {
        *payload_offset = offset;
    }

    return S2022_OK;
}

static void make_bitstring(uint8_t *bitstring,
                           const uint8_t *packet,
                           size_t packet_len,
                           size_t payload_offset)
{
    uint16_t protected_len = (uint16_t)(packet_len - payload_offset);

    bitstring[0] = (uint8_t)(packet[0] & 0x3fu);
    bitstring[1] = packet[1];
    memcpy(bitstring + 2, packet + 4, 4);
    write_u16(bitstring + 6, protected_len);
    memcpy(bitstring + 8, packet + payload_offset, protected_len);
}

static s2022_status build_fec(s2022_encoder *encoder,
                              s2022_fec_type type,
                              size_t first_index,
                              size_t offset,
                              size_t na,
                              uint8_t *out,
                              size_t *out_len)
{
    uint8_t *recovery;
    size_t i;
    uint16_t sequence;
    s2022_work_packet *first;

    first = &encoder->packets[first_index];
    recovery = encoder->fec_buffer + S2022_FEC_PACKET_OVERHEAD;
    memset(recovery, 0, encoder->bitstring_len);

    for (i = 0; i < na; ++i) {
        xor_into(recovery, encoder->packets[first_index + (i * offset)].bitstring, encoder->bitstring_len);
    }

    memset(out, 0, S2022_FEC_PACKET_OVERHEAD);

    sequence = type == S2022_FEC_FIRST ? ++encoder->config.column_sequence : ++encoder->config.row_sequence;

    out[0] = (uint8_t)(0x80u | (recovery[0] & 0x3fu));
    out[1] = (uint8_t)((recovery[1] & 0x80u) | S2022_DEFAULT_FEC_PT);
    write_u16(out + 2, sequence);
    write_u32(out + 4, first->timestamp);
    write_u32(out + 8, 0u);

    write_u16(out + 12, first->sequence);
    memcpy(out + 14, recovery + 6, 2);
    out[16] = (uint8_t)(0x80u | (recovery[1] & 0x7fu));
    out[17] = 0;
    out[18] = 0;
    out[19] = 0;
    memcpy(out + 20, recovery + 2, 4);
    out[24] = type == S2022_FEC_SECOND ? 0x40u : 0x00u;
    out[25] = (uint8_t)offset;
    out[26] = (uint8_t)na;
    out[27] = 0;
    memcpy(out + S2022_FEC_PACKET_OVERHEAD, recovery + 8, encoder->protected_len);
    *out_len = S2022_FEC_PACKET_OVERHEAD + encoder->protected_len;

    return S2022_OK;
}

static s2022_status emit_fec(s2022_encoder *encoder,
                             s2022_fec_type type,
                             size_t first_index,
                             size_t offset,
                             size_t na,
                             s2022_fec_packet_cb callback,
                             void *callback_user)
{
    size_t packet_len;
    s2022_status status;

    if (!callback) {
        return S2022_OK;
    }

    status = build_fec(encoder, type, first_index, offset, na, encoder->fec_buffer, &packet_len);
    if (status != S2022_OK) {
        return status;
    }

    callback(callback_user, type, encoder->fec_buffer, packet_len);
    return S2022_OK;
}

void s2022_config_init(s2022_config *config)
{
    if (!config) {
        return;
    }

    config->columns_l = 5;
    config->rows_d = 5;
    config->mode = S2022_FEC_MODE_LEVEL_B;
    config->receiver_stream = S2022_RECEIVER_STREAM_AUTO;
    config->fec_payload_type = S2022_DEFAULT_FEC_PT;
    config->fec_ssrc = 0;
    config->row_sequence = 0;
    config->column_sequence = 0;
}

s2022_status s2022_encoder_create(const s2022_config *config, s2022_encoder **encoder)
{
    s2022_config local_config;
    s2022_encoder *ctx;
    size_t block_packets;
    size_t i;
    s2022_status status;

    if (!encoder) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    if (config) {
        local_config = *config;
    } else {
        s2022_config_init(&local_config);
    }

    status = validate_config(&local_config);
    if (status != S2022_OK) {
        return status;
    }

    ctx = (s2022_encoder *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return S2022_ERROR_NO_MEMORY;
    }

    block_packets = (size_t)local_config.columns_l * (size_t)local_config.rows_d;
    ctx->packets = (s2022_work_packet *)calloc(block_packets, sizeof(*ctx->packets));
    if (!ctx->packets) {
        free(ctx);
        return S2022_ERROR_NO_MEMORY;
    }

    for (i = 0; i < block_packets; ++i) {
        ctx->packets[i].bitstring = NULL;
    }

    ctx->config = local_config;
    *encoder = ctx;
    return S2022_OK;
}

void s2022_encoder_destroy(s2022_encoder *encoder)
{
    if (!encoder) {
        return;
    }

    free(encoder->fec_buffer);
    free(encoder->pending_column_lens);
    free(encoder->pending_columns);
    free(encoder->storage);
    free(encoder->packets);
    free(encoder);
}

void s2022_encoder_reset(s2022_encoder *encoder)
{
    size_t block_packets;
    size_t i;

    if (!encoder) {
        return;
    }

    block_packets = (size_t)encoder->config.columns_l * (size_t)encoder->config.rows_d;

    free(encoder->fec_buffer);
    free(encoder->pending_column_lens);
    free(encoder->pending_columns);
    free(encoder->storage);

    encoder->packet_len = 0;
    encoder->protected_len = 0;
    encoder->bitstring_len = 0;
    encoder->packet_count = 0;
    encoder->storage = NULL;
    encoder->fec_buffer = NULL;
    encoder->pending_columns = NULL;
    encoder->pending_column_lens = NULL;
    encoder->pending_column_count = 0;
    encoder->pending_column_next = 0;
    encoder->pending_column_delay = 0;
    encoder->have_expected_sequence = 0;
    encoder->expected_sequence = 0;
    encoder->have_extension_profile = 0;
    encoder->extension_enabled = 0;
    encoder->extension_profile = 0;
    encoder->extension_words = 0;

    for (i = 0; i < block_packets; ++i) {
        encoder->packets[i].sequence = 0;
        encoder->packets[i].timestamp = 0;
        encoder->packets[i].bitstring = NULL;
        encoder->packets[i].payload_offset = 0;
    }
}

s2022_status s2022_encoder_push_rtp(s2022_encoder *encoder,
                                    const uint8_t *rtp_packet,
                                    size_t rtp_packet_len,
                                    s2022_fec_packet_cb callback,
                                    void *callback_user)
{
    size_t block_packets;
    size_t i;
    s2022_status status;
    s2022_work_packet *slot;
    uint8_t extension_enabled;
    uint16_t extension_profile;
    uint16_t extension_words;
    size_t payload_offset;

    if (!encoder) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    status = parse_rtp_packet(rtp_packet, rtp_packet_len,
                              &extension_enabled, &extension_profile,
                              &extension_words, &payload_offset);
    if (status != S2022_OK) {
        return status;
    }

    if (encoder->have_extension_profile) {
        if (encoder->extension_enabled != extension_enabled ||
            encoder->extension_profile != extension_profile ||
            encoder->extension_words != extension_words) {
            return S2022_ERROR_UNSUPPORTED;
        }
    } else {
        encoder->have_extension_profile = 1;
        encoder->extension_enabled = extension_enabled;
        encoder->extension_profile = extension_profile;
        encoder->extension_words = extension_words;
    }

    if (encoder->config.mode == S2022_FEC_MODE_DISABLED) {
        return S2022_OK;
    }

    block_packets = (size_t)encoder->config.columns_l * (size_t)encoder->config.rows_d;

    if (encoder->packet_len == 0) {
        encoder->packet_len = rtp_packet_len;
        encoder->protected_len = rtp_packet_len - payload_offset;
        encoder->bitstring_len = encoder->protected_len + 8u;
        encoder->storage = (uint8_t *)calloc(block_packets, encoder->bitstring_len);
        encoder->fec_buffer = (uint8_t *)calloc(1, S2022_FEC_PACKET_OVERHEAD + encoder->bitstring_len);
        encoder->pending_columns = (uint8_t *)calloc(encoder->config.columns_l,
                                                     S2022_FEC_PACKET_OVERHEAD + encoder->protected_len);
        encoder->pending_column_lens = (size_t *)calloc(encoder->config.columns_l,
                                                        sizeof(*encoder->pending_column_lens));
        if (!encoder->storage || !encoder->fec_buffer ||
            !encoder->pending_columns || !encoder->pending_column_lens) {
            return S2022_ERROR_NO_MEMORY;
        }

        for (i = 0; i < block_packets; ++i) {
            encoder->packets[i].bitstring = encoder->storage + (i * encoder->bitstring_len);
        }
    } else if (rtp_packet_len != encoder->packet_len) {
        return S2022_ERROR_UNSUPPORTED;
    }

    if (encoder->have_expected_sequence &&
        read_u16(rtp_packet + 2) != encoder->expected_sequence) {
        return S2022_ERROR_INVALID_PACKET;
    }

    slot = &encoder->packets[encoder->packet_count];
    slot->sequence = read_u16(rtp_packet + 2);
    encoder->expected_sequence = (uint16_t)(slot->sequence + 1u);
    encoder->have_expected_sequence = 1;
    slot->timestamp = read_u32(rtp_packet + 4);
    slot->payload_offset = payload_offset;
    make_bitstring(slot->bitstring, rtp_packet, rtp_packet_len, payload_offset);
    encoder->packet_count++;

    if (encoder->pending_column_delay > 0u) {
        encoder->pending_column_delay--;
    }

    if (encoder->pending_column_delay == 0u &&
        encoder->pending_column_next < encoder->pending_column_count) {
        size_t idx = encoder->pending_column_next;
        if (callback) {
            callback(callback_user, S2022_FEC_FIRST,
                     encoder->pending_columns + (idx * (S2022_FEC_PACKET_OVERHEAD + encoder->protected_len)),
                     encoder->pending_column_lens[idx]);
        }
        encoder->pending_column_next++;
        if (encoder->pending_column_next == encoder->pending_column_count) {
            encoder->pending_column_count = 0;
            encoder->pending_column_next = 0;
        }
    }

    if (encoder->config.mode == S2022_FEC_MODE_LEVEL_B &&
        (encoder->packet_count % encoder->config.columns_l) == 0u) {
        size_t row = (encoder->packet_count / encoder->config.columns_l) - 1u;
        status = emit_fec(encoder, S2022_FEC_SECOND,
                          row * encoder->config.columns_l, 1u,
                          encoder->config.columns_l,
                          callback, callback_user);
        if (status != S2022_OK) {
            return status;
        }
    }

    if (encoder->packet_count == block_packets) {
        for (i = 0; i < encoder->config.columns_l; ++i) {
            size_t pending_len = 0;
            uint8_t *pending = encoder->pending_columns +
                (i * (S2022_FEC_PACKET_OVERHEAD + encoder->protected_len));

            status = build_fec(encoder, S2022_FEC_FIRST,
                               i, encoder->config.columns_l,
                               encoder->config.rows_d,
                               pending, &pending_len);
            if (status != S2022_OK) {
                return status;
            }
            encoder->pending_column_lens[i] = pending_len;
        }

        encoder->pending_column_count = encoder->config.columns_l;
        encoder->pending_column_next = 0;
        encoder->pending_column_delay = encoder->config.columns_l;
        encoder->packet_count = 0;
    }

    return S2022_OK;
}

s2022_status s2022_encoder_flush(s2022_encoder *encoder,
                                 s2022_fec_packet_cb callback,
                                 void *callback_user)
{
    if (!encoder) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    if (encoder->pending_column_delay > 0u &&
        encoder->pending_column_next < encoder->pending_column_count) {
        return S2022_ERROR_NOT_READY;
    }

    while (encoder->pending_column_next < encoder->pending_column_count) {
        size_t idx = encoder->pending_column_next;
        if (callback) {
            callback(callback_user, S2022_FEC_FIRST,
                     encoder->pending_columns + (idx * (S2022_FEC_PACKET_OVERHEAD + encoder->protected_len)),
                     encoder->pending_column_lens[idx]);
        }
        encoder->pending_column_next++;
    }

    encoder->pending_column_count = 0;
    encoder->pending_column_next = 0;
    encoder->pending_column_delay = 0;
    return S2022_OK;
}

s2022_status s2022_parse_fec_packet(const uint8_t *fec_packet,
                                    size_t fec_packet_len,
                                    s2022_fec_info *info)
{
    uint8_t type_bit;

    if (!fec_packet || !info || fec_packet_len < S2022_FEC_PACKET_OVERHEAD) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    if ((fec_packet[0] & 0xc0u) != 0x80u) {
        return S2022_ERROR_INVALID_PACKET;
    }

    type_bit = (uint8_t)((fec_packet[24] >> 6) & 0x01u);
    info->type = type_bit ? S2022_FEC_SECOND : S2022_FEC_FIRST;
    info->sn_base = read_u16(fec_packet + 12);
    info->offset = fec_packet[25];
    info->na = fec_packet[26];
    info->length_recovery = read_u16(fec_packet + 14);
    info->payload_type_recovery = (uint8_t)(fec_packet[16] & 0x7fu);
    info->timestamp_recovery = read_u32(fec_packet + 20);

    if (info->offset == 0 || info->na == 0 || info->na > S2022_MAX_BLOCK_PACKETS) {
        return S2022_ERROR_INVALID_PACKET;
    }

    if ((fec_packet[1] & 0x7fu) != S2022_DEFAULT_FEC_PT ||
        read_u32(fec_packet + 8) != 0u ||
        (fec_packet[16] & 0x80u) == 0u ||
        fec_packet[17] != 0u || fec_packet[18] != 0u || fec_packet[19] != 0u ||
        (fec_packet[24] & 0x80u) != 0u ||
        (fec_packet[24] & 0x0fu) != 0u ||
        fec_packet[27] != 0u) {
        return S2022_ERROR_INVALID_PACKET;
    }

    return S2022_OK;
}

s2022_status s2022_recover_media_packet(const uint8_t *fec_packet,
                                        size_t fec_packet_len,
                                        const s2022_media_packet *media_packets,
                                        size_t media_packet_count,
                                        size_t missing_index,
                                        uint8_t *out_packet,
                                        size_t out_capacity,
                                        size_t *out_packet_len)
{
    s2022_fec_info info;
    uint8_t *work;
    size_t bitstring_len;
    size_t payload_recovery_len;
    size_t recovered_payload_len;
    size_t recovered_packet_len;
    size_t i;
    uint32_t media_ssrc;
    s2022_status status;

    if (!media_packets || !out_packet || !out_packet_len) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    status = s2022_parse_fec_packet(fec_packet, fec_packet_len, &info);
    if (status != S2022_OK) {
        return status;
    }

    if (media_packet_count != info.na || missing_index >= media_packet_count) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    if ((fec_packet[24] & 0x30u) != 0u) {
        return S2022_ERROR_UNSUPPORTED;
    }

    payload_recovery_len = fec_packet_len - S2022_FEC_PACKET_OVERHEAD;
    bitstring_len = 8u + payload_recovery_len;
    work = (uint8_t *)malloc(bitstring_len);
    if (!work) {
        return S2022_ERROR_NO_MEMORY;
    }

    memset(work, 0, bitstring_len);
    work[0] = (uint8_t)(fec_packet[0] & 0x3fu);
    work[1] = (uint8_t)(fec_packet[16] & 0x7fu);
    memcpy(work + 2, fec_packet + 20, 4);
    memcpy(work + 6, fec_packet + 14, 2);
    memcpy(work + 8, fec_packet + S2022_FEC_PACKET_OVERHEAD, payload_recovery_len);

    media_ssrc = 0;
    for (i = 0; i < media_packet_count; ++i) {
        uint8_t *tmp;
        uint8_t extension_enabled;
        uint16_t extension_profile;
        uint16_t extension_words;
        size_t payload_offset;

        if (i == missing_index) {
            continue;
        }

        status = parse_rtp_packet(media_packets[i].data, media_packets[i].len,
                                  &extension_enabled, &extension_profile,
                                  &extension_words, &payload_offset);
        if (status != S2022_OK ||
            media_packets[i].len > payload_offset + payload_recovery_len) {
            free(work);
            return S2022_ERROR_UNRECOVERABLE;
        }

        if (media_ssrc == 0) {
            media_ssrc = read_u32(media_packets[i].data + 8);
        }

        tmp = (uint8_t *)malloc(bitstring_len);
        if (!tmp) {
            free(work);
            return S2022_ERROR_NO_MEMORY;
        }
        memset(tmp, 0, bitstring_len);
        make_bitstring(tmp, media_packets[i].data, media_packets[i].len, payload_offset);
        xor_into(work, tmp, bitstring_len);
        free(tmp);
    }

    recovered_payload_len = read_u16(work + 6);
    if (recovered_payload_len > payload_recovery_len) {
        free(work);
        return S2022_ERROR_UNRECOVERABLE;
    }

    recovered_packet_len = S2022_RTP_HEADER_SIZE + recovered_payload_len;
    if (out_capacity < recovered_packet_len) {
        *out_packet_len = recovered_packet_len;
        free(work);
        return S2022_ERROR_BUFFER_TOO_SMALL;
    }

    memset(out_packet, 0, recovered_packet_len);
    out_packet[0] = (uint8_t)(0x80u | (work[0] & 0x20u));
    out_packet[1] = work[1];
    write_u16(out_packet + 2, (uint16_t)(info.sn_base + (uint16_t)(missing_index * info.offset)));
    memcpy(out_packet + 4, work + 2, 4);
    write_u32(out_packet + 8, media_ssrc);
    memcpy(out_packet + S2022_RTP_HEADER_SIZE, work + 8, recovered_payload_len);

    *out_packet_len = recovered_packet_len;
    free(work);
    return S2022_OK;
}

static s2022_store_entry *receiver_find_entry(s2022_receiver *receiver, uint16_t sequence)
{
    size_t i;

    for (i = 0; i < receiver->store_capacity; ++i) {
        if (receiver->store[i].valid && receiver->store[i].sequence == sequence) {
            return &receiver->store[i];
        }
    }

    return NULL;
}

static s2022_store_entry *receiver_alloc_entry(s2022_receiver *receiver, uint16_t sequence)
{
    size_t i;

    for (i = 0; i < receiver->store_capacity; ++i) {
        if (!receiver->store[i].valid) {
            receiver->store[i].valid = 1;
            receiver->store[i].sequence = sequence;
            return &receiver->store[i];
        }
    }

    {
        size_t oldest = 0;
        for (i = 1; i < receiver->store_capacity; ++i) {
            if (receiver->store[i].stored_at < receiver->store[oldest].stored_at) {
                oldest = i;
            }
        }
        receiver->store[oldest].valid = 1;
        receiver->store[oldest].sequence = sequence;
        return &receiver->store[oldest];
    }
}

static s2022_status receiver_ensure_storage(s2022_receiver *receiver, size_t packet_len)
{
    uint8_t *packets;
    size_t i;

    if (receiver->packet_len == 0) {
        receiver->packet_len = packet_len;
        packets = (uint8_t *)calloc(receiver->store_capacity, packet_len);
        if (!packets) {
            return S2022_ERROR_NO_MEMORY;
        }
        receiver->store_packets = packets;
        for (i = 0; i < receiver->store_capacity; ++i) {
            receiver->store[i].packet = receiver->store_packets + (i * packet_len);
        }
    } else if (receiver->packet_len != packet_len) {
        return S2022_ERROR_UNSUPPORTED;
    }

    return S2022_OK;
}

static s2022_status receiver_store_normalized_packet(s2022_receiver *receiver,
                                                     const uint8_t *packet,
                                                     size_t packet_len)
{
    s2022_status status;
    s2022_store_entry *entry;
    uint16_t sequence;

    status = receiver_ensure_storage(receiver, packet_len);
    if (status != S2022_OK) {
        return status;
    }

    sequence = read_u16(packet + 2);
    entry = receiver_find_entry(receiver, sequence);
    if (!entry) {
        entry = receiver_alloc_entry(receiver, sequence);
    }

    memcpy(entry->packet, packet, packet_len);
    entry->packet[0] = (uint8_t)(entry->packet[0] & ~0x10u);
    entry->packet_len = packet_len;
    entry->stored_at = ++receiver->store_counter;
    return S2022_OK;
}

static s2022_status receiver_store_packet(s2022_receiver *receiver,
                                          const uint8_t *packet,
                                          size_t packet_len)
{
    s2022_status status;
    s2022_store_entry *entry;
    uint16_t sequence;
    uint8_t extension_enabled;
    uint16_t extension_profile;
    uint16_t extension_words;
    size_t payload_offset;
    size_t normalized_len;
    size_t payload_len;

    status = parse_rtp_packet(packet, packet_len,
                              &extension_enabled, &extension_profile,
                              &extension_words, &payload_offset);
    if (status != S2022_OK) {
        return status;
    }

    if (receiver->have_extension_profile) {
        if (receiver->extension_enabled != extension_enabled ||
            receiver->extension_profile != extension_profile ||
            receiver->extension_words != extension_words) {
            return S2022_ERROR_UNSUPPORTED;
        }
    } else {
        receiver->have_extension_profile = 1;
        receiver->extension_enabled = extension_enabled;
        receiver->extension_profile = extension_profile;
        receiver->extension_words = extension_words;
    }

    payload_len = packet_len - payload_offset;
    normalized_len = S2022_RTP_HEADER_SIZE + payload_len;

    status = receiver_ensure_storage(receiver, normalized_len);
    if (status != S2022_OK) {
        return status;
    }

    sequence = read_u16(packet + 2);
    entry = receiver_find_entry(receiver, sequence);
    if (!entry) {
        entry = receiver_alloc_entry(receiver, sequence);
    }

    memcpy(entry->packet, packet, S2022_RTP_HEADER_SIZE);
    entry->packet[0] = (uint8_t)(entry->packet[0] & ~0x10u);
    memcpy(entry->packet + S2022_RTP_HEADER_SIZE, packet + payload_offset, payload_len);
    entry->packet_len = normalized_len;
    entry->stored_at = ++receiver->store_counter;
    return S2022_OK;
}

static s2022_status validate_fec_geometry(const s2022_receiver *receiver,
                                          const s2022_fec_info *info)
{
    if (info->type == S2022_FEC_FIRST) {
        if (info->offset != receiver->config.columns_l ||
            info->na != receiver->config.rows_d) {
            return S2022_ERROR_INVALID_PACKET;
        }
    } else {
        if (info->offset != 1u ||
            info->na != receiver->config.columns_l) {
            return S2022_ERROR_INVALID_PACKET;
        }
    }

    return S2022_OK;
}

s2022_status s2022_receiver_create(const s2022_config *config, s2022_receiver **receiver)
{
    s2022_config local_config;
    s2022_receiver *ctx;
    size_t block_packets;
    size_t store_capacity;
    s2022_status status;

    if (!receiver) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    if (config) {
        local_config = *config;
    } else {
        s2022_config_init(&local_config);
    }

    status = validate_config(&local_config);
    if (status != S2022_OK) {
        return status;
    }

    block_packets = (size_t)local_config.columns_l * (size_t)local_config.rows_d;
    store_capacity = (block_packets * 4u) + local_config.columns_l + local_config.rows_d + 16u;
    if (store_capacity < 64u) {
        store_capacity = 64u;
    }

    ctx = (s2022_receiver *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return S2022_ERROR_NO_MEMORY;
    }

    ctx->store = (s2022_store_entry *)calloc(store_capacity, sizeof(*ctx->store));
    if (!ctx->store) {
        free(ctx);
        return S2022_ERROR_NO_MEMORY;
    }

    ctx->config = local_config;
    ctx->store_capacity = store_capacity;
    stats_reset_receiver(&ctx->stats);
    *receiver = ctx;
    return S2022_OK;
}

void s2022_receiver_destroy(s2022_receiver *receiver)
{
    if (!receiver) {
        return;
    }

    free(receiver->store_packets);
    free(receiver->store);
    free(receiver);
}

void s2022_receiver_reset(s2022_receiver *receiver)
{
    size_t i;

    if (!receiver) {
        return;
    }

    if (receiver->store) {
        for (i = 0; i < receiver->store_capacity; ++i) {
            receiver->store[i].valid = 0;
            receiver->store[i].packet = NULL;
            receiver->store[i].packet_len = 0;
            receiver->store[i].sequence = 0;
            receiver->store[i].stored_at = 0;
        }
    }
    free(receiver->store_packets);
    receiver->store_packets = NULL;
    receiver->packet_len = 0;
    receiver->store_counter = 0;
    receiver->have_extension_profile = 0;
    receiver->extension_enabled = 0;
    receiver->extension_profile = 0;
    receiver->extension_words = 0;
    receiver->have_level_a_stream = 0;
    receiver->level_a_stream = S2022_FEC_FIRST;
}

s2022_status s2022_receiver_push_media(s2022_receiver *receiver,
                                       const uint8_t *rtp_packet,
                                       size_t rtp_packet_len)
{
    s2022_status status;

    if (!receiver) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    status = receiver_store_packet(receiver, rtp_packet, rtp_packet_len);
    if (status == S2022_OK) {
        stats_add_u64(&receiver->stats.media_packets_processed, 1);
        stats_add_u64(&receiver->stats.media_bytes_processed, (uint64_t)rtp_packet_len);
    }

    return status;
}

s2022_status s2022_receiver_push_fec(s2022_receiver *receiver,
                                     const uint8_t *fec_packet,
                                     size_t fec_packet_len,
                                     s2022_recovered_packet_cb callback,
                                     void *callback_user)
{
    s2022_fec_info info;
    s2022_media_packet *media_packets;
    uint8_t *recovered;
    size_t recovered_len;
    size_t payload_recovery_len;
    size_t missing_count;
    size_t missing_index;
    size_t i;
    s2022_status status;

    if (!receiver) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    status = s2022_parse_fec_packet(fec_packet, fec_packet_len, &info);
    if (status != S2022_OK) {
        return status;
    }

    if ((fec_packet[24] & 0x30u) != 0u) {
        return S2022_OK;
    }

    status = validate_fec_geometry(receiver, &info);
    if (status != S2022_OK) {
        return status;
    }

    if (receiver->config.mode == S2022_FEC_MODE_DISABLED) {
        return S2022_OK;
    }

    if (receiver->config.mode == S2022_FEC_MODE_LEVEL_A) {
        s2022_fec_type selected = info.type;

        if (receiver->config.receiver_stream == S2022_RECEIVER_STREAM_FIRST) {
            selected = S2022_FEC_FIRST;
        } else if (receiver->config.receiver_stream == S2022_RECEIVER_STREAM_SECOND) {
            selected = S2022_FEC_SECOND;
        } else if (receiver->have_level_a_stream) {
            selected = receiver->level_a_stream;
        } else {
            receiver->have_level_a_stream = 1;
            receiver->level_a_stream = info.type;
        }

        if (info.type != selected) {
            return S2022_OK;
        }
    }

    stats_add_u64(&receiver->stats.fec_packets_processed, 1);
    stats_add_u64(&receiver->stats.fec_bytes_processed, (uint64_t)fec_packet_len);

    payload_recovery_len = fec_packet_len - S2022_FEC_PACKET_OVERHEAD;
    status = receiver_ensure_storage(receiver, S2022_RTP_HEADER_SIZE + payload_recovery_len);
    if (status != S2022_OK) {
        return status;
    }

    media_packets = (s2022_media_packet *)calloc(info.na, sizeof(*media_packets));
    recovered = (uint8_t *)malloc(receiver->packet_len);
    if (!media_packets || !recovered) {
        free(media_packets);
        free(recovered);
        return S2022_ERROR_NO_MEMORY;
    }

    missing_count = 0;
    missing_index = 0;
    for (i = 0; i < info.na; ++i) {
        uint16_t sequence = (uint16_t)(info.sn_base + (uint16_t)(i * info.offset));
        s2022_store_entry *entry = receiver_find_entry(receiver, sequence);

        if (!entry) {
            missing_count++;
            missing_index = i;
            continue;
        }

        media_packets[i].data = entry->packet;
        media_packets[i].len = entry->packet_len;
    }

    if (missing_count == 0u) {
        free(media_packets);
        free(recovered);
        return S2022_OK;
    }

    if (missing_count > 1u) {
        stats_add_u64(&receiver->stats.recovery_deferred_packets, 1);
        free(media_packets);
        free(recovered);
        return S2022_ERROR_NOT_READY;
    }

    stats_add_u64(&receiver->stats.recovery_attempts, 1);

    recovered_len = 0;
    status = s2022_recover_media_packet(fec_packet, fec_packet_len,
                                        media_packets, info.na, missing_index,
                                        recovered, receiver->packet_len, &recovered_len);
    if (status == S2022_OK) {
        status = receiver_store_normalized_packet(receiver, recovered, recovered_len);
        if (status == S2022_OK && callback) {
            callback(callback_user, recovered, recovered_len);
        }
    }
    if (status == S2022_OK) {
        stats_add_u64(&receiver->stats.recovered_packets, 1);
        stats_add_u64(&receiver->stats.recovered_bytes, (uint64_t)recovered_len);
    } else {
        stats_add_u64(&receiver->stats.recovery_failed_packets, 1);
    }

    free(media_packets);
    free(recovered);
    return status;
}

s2022_status s2022_receiver_get_stats(s2022_receiver *receiver,
                                      s2022_receiver_stats *stats)
{
    uint64_t attempts;
    uint64_t failures;

    if (!receiver || !stats) {
        return S2022_ERROR_INVALID_ARGUMENT;
    }

    memset(stats, 0, sizeof(*stats));
    stats->reset_time = stats_load_time(&receiver->stats.reset_time);
    stats->sampled_time = time(NULL);
    stats->media_packets_processed = stats_load_u64(&receiver->stats.media_packets_processed);
    stats->media_bytes_processed = stats_load_u64(&receiver->stats.media_bytes_processed);
    stats->fec_packets_processed = stats_load_u64(&receiver->stats.fec_packets_processed);
    stats->fec_bytes_processed = stats_load_u64(&receiver->stats.fec_bytes_processed);
    stats->recovery_attempts = stats_load_u64(&receiver->stats.recovery_attempts);
    stats->recovery_deferred_packets =
        stats_load_u64(&receiver->stats.recovery_deferred_packets);
    stats->recovered_packets = stats_load_u64(&receiver->stats.recovered_packets);
    stats->recovered_bytes = stats_load_u64(&receiver->stats.recovered_bytes);
    stats->recovery_failed_packets = stats_load_u64(&receiver->stats.recovery_failed_packets);

    attempts = stats->recovery_attempts;
    failures = stats->recovery_failed_packets;
    stats->recovery_error_rate = attempts == 0u ? 0.0 : (double)failures / (double)attempts;
    return S2022_OK;
}

void s2022_receiver_reset_stats(s2022_receiver *receiver)
{
    if (!receiver) {
        return;
    }

    stats_reset_receiver(&receiver->stats);
}

const char *s2022_status_string(s2022_status status)
{
    switch (status) {
    case S2022_OK:
        return "ok";
    case S2022_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case S2022_ERROR_INVALID_PACKET:
        return "invalid packet";
    case S2022_ERROR_UNSUPPORTED:
        return "unsupported packet or mode";
    case S2022_ERROR_NO_MEMORY:
        return "out of memory";
    case S2022_ERROR_BUFFER_TOO_SMALL:
        return "buffer too small";
    case S2022_ERROR_NOT_READY:
        return "not ready";
    case S2022_ERROR_UNRECOVERABLE:
        return "unrecoverable";
    default:
        return "unknown status";
    }
}
