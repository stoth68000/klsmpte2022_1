/* SPDX-License-Identifier: LGPL-2.1-only */
/* Copyright (c) Kernel Labs Inc 2026. All Rights Reserved. */

#ifndef KLSMPTE2022_1_FEC_H
#define KLSMPTE2022_1_FEC_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file fec.h
 * @brief Public SMPTE ST 2022-1 FEC encoder and receiver API.
 *
 * This library generates and consumes SMPTE ST 2022-1 XOR FEC packets for
 * RTP MPEG-TS streams. Networking is intentionally left to the application:
 * callers pass RTP packets into the encoder or receiver and send/receive UDP
 * packets on the SMPTE-defined media and FEC ports.
 */

/** @brief Library major version. */
#define S2022_VERSION_MAJOR 0
/** @brief Library minor version. */
#define S2022_VERSION_MINOR 1
/** @brief Library patch version. */
#define S2022_VERSION_PATCH 0

/** @brief Fixed RTP header size used by ST 2022-1 media and FEC packets. */
#define S2022_RTP_HEADER_SIZE 12u
/** @brief SMPTE ST 2022-1 extended FEC header size, excluding RTP header. */
#define S2022_FEC_HEADER_SIZE 16u
/** @brief RTP header plus ST 2022-1 FEC header size. */
#define S2022_FEC_PACKET_OVERHEAD (S2022_RTP_HEADER_SIZE + S2022_FEC_HEADER_SIZE)
/** @brief RTP payload type for MPEG-2 transport stream media. */
#define S2022_RTP_PT_MP2T 33u
/** @brief Required ST 2022-1 FEC RTP payload type. */
#define S2022_DEFAULT_FEC_PT 96u

/**
 * @brief Status codes returned by the library.
 */
typedef enum s2022_status {
    /** Operation completed successfully. */
    S2022_OK = 0,
    /** A pointer, count, mode, or configuration value is invalid. */
    S2022_ERROR_INVALID_ARGUMENT = -1,
    /** A packet is malformed or violates a required header constraint. */
    S2022_ERROR_INVALID_PACKET = -2,
    /** The input is valid RTP/FEC but outside the supported profile. */
    S2022_ERROR_UNSUPPORTED = -3,
    /** Memory allocation failed. */
    S2022_ERROR_NO_MEMORY = -4,
    /** Caller-provided output storage is too small. */
    S2022_ERROR_BUFFER_TOO_SMALL = -5,
    /** More media or FEC packets are needed before recovery can complete. */
    S2022_ERROR_NOT_READY = -6,
    /** Recovery was attempted but the missing packet cannot be reconstructed. */
    S2022_ERROR_UNRECOVERABLE = -7
} s2022_status;

/**
 * @brief SMPTE ST 2022-1 FEC stream identifier.
 */
typedef enum s2022_fec_type {
    /** First FEC stream, also called column FEC. Uses Offset=L and NA=D. */
    S2022_FEC_FIRST = 0,
    /** Second FEC stream, also called row FEC. Uses Offset=1 and NA=L. */
    S2022_FEC_SECOND = 1
} s2022_fec_type;

/**
 * @brief FEC operating mode.
 */
typedef enum s2022_fec_mode {
    /** Validate RTP input but do not generate or process FEC. */
    S2022_FEC_MODE_DISABLED = 0,
    /** One FEC stream. The encoder emits first-stream FEC only. */
    S2022_FEC_MODE_LEVEL_A = 1,
    /** Two FEC streams. The encoder emits first-stream and second-stream FEC. */
    S2022_FEC_MODE_LEVEL_B = 2
} s2022_fec_mode;

/**
 * @brief Level A receiver stream selection.
 */
typedef enum s2022_receiver_stream {
    /** Process the first FEC stream seen and ignore the other stream. */
    S2022_RECEIVER_STREAM_AUTO = 0,
    /** Process only the first/column FEC stream. */
    S2022_RECEIVER_STREAM_FIRST = 1,
    /** Process only the second/row FEC stream. */
    S2022_RECEIVER_STREAM_SECOND = 2
} s2022_receiver_stream;

/**
 * @brief Shared encoder and receiver configuration.
 *
 * For a sender, use Level A to emit the first FEC stream on UDP port N+2, or
 * Level B to emit both first-stream FEC on N+2 and second-stream FEC on N+4.
 * Media RTP is sent by the application on port N.
 *
 * For a receiver, the same L and D values used by the sender are required so
 * incoming FEC packet geometry can be validated.
 */
typedef struct s2022_config {
    /** FEC operating mode. Defaults to S2022_FEC_MODE_LEVEL_B. */
    s2022_fec_mode mode;
    /** Level A receiver stream policy. Ignored by encoders and Level B receivers. */
    s2022_receiver_stream receiver_stream;
    /** Number of columns, L. Level B requires L >= 4. */
    uint8_t columns_l;
    /** Number of rows, D. MPEG-TS profile support is currently 4..20. */
    uint8_t rows_d;
    /** SMPTE ST 2022-1 requires payload type 96. */
    uint8_t fec_payload_type;
    /** SMPTE ST 2022-1 requires FEC RTP SSRC 0. */
    uint32_t fec_ssrc;
    /** Initial second-stream FEC RTP sequence number. */
    uint16_t row_sequence;
    /** Initial first-stream FEC RTP sequence number. */
    uint16_t column_sequence;
} s2022_config;

/** @brief Opaque FEC encoder context. */
typedef struct s2022_encoder s2022_encoder;

/**
 * @brief Callback invoked for each generated FEC RTP packet.
 *
 * Applications should transmit S2022_FEC_FIRST packets on media port N+2 and
 * S2022_FEC_SECOND packets on media port N+4, using the same destination IP
 * address and source UDP port as the associated media stream.
 */
typedef void (*s2022_fec_packet_cb)(void *user,
                                    s2022_fec_type type,
                                    const uint8_t *packet,
                                    size_t packet_len);

/**
 * @brief Media RTP packet descriptor used by the standalone recovery helper.
 */
typedef struct s2022_media_packet {
    /** Pointer to a complete RTP media packet, or NULL for the missing packet. */
    const uint8_t *data;
    /** Number of bytes available at @ref data. */
    size_t len;
} s2022_media_packet;

/**
 * @brief Parsed fields from a SMPTE ST 2022-1 FEC packet.
 */
typedef struct s2022_fec_info {
    /** First/column or second/row FEC stream, derived from the FEC D bit. */
    s2022_fec_type type;
    /** Base RTP media sequence number protected by this FEC packet. */
    uint16_t sn_base;
    /** Protection period. First stream uses L; second stream uses 1. */
    uint16_t offset;
    /** Number of associated media packets. First stream uses D; second stream uses L. */
    uint16_t na;
    /** XOR-recovered RTP payload length field. */
    uint16_t length_recovery;
    /** XOR-recovered media RTP payload type. */
    uint8_t payload_type_recovery;
    /** XOR-recovered media RTP timestamp. */
    uint32_t timestamp_recovery;
} s2022_fec_info;

/** @brief Opaque FEC receiver context. */
typedef struct s2022_receiver s2022_receiver;

/**
 * @brief Receiver statistics snapshot.
 *
 * All counters are maintained by the receiver context and may be queried from
 * a monitoring thread. The timestamps allow applications to compute rates over
 * a known interval. @ref recovery_error_rate is recovery_failed_packets divided
 * by recovery_attempts, or 0.0 when no recovery has been attempted.
 */
typedef struct s2022_receiver_stats {
    /** Time when the current statistics interval began. */
    time_t reset_time;
    /** Time when this snapshot was taken. */
    time_t sampled_time;
    /** Successfully processed media RTP packets. */
    uint64_t media_packets_processed;
    /** Bytes in successfully processed media RTP packets. */
    uint64_t media_bytes_processed;
    /** Successfully processed FEC RTP packets. */
    uint64_t fec_packets_processed;
    /** Bytes in successfully processed FEC RTP packets. */
    uint64_t fec_bytes_processed;
    /** FEC packets that attempted to recover one or more missing media packets. */
    uint64_t recovery_attempts;
    /** Media RTP packets recovered by FEC. */
    uint64_t recovered_packets;
    /** Bytes in recovered media RTP packets. */
    uint64_t recovered_bytes;
    /** Recovery attempts that could not reconstruct a media RTP packet. */
    uint64_t recovery_failed_packets;
    /** recovery_failed_packets / recovery_attempts, or 0.0 with no attempts. */
    double recovery_error_rate;
} s2022_receiver_stats;

/**
 * @brief Callback invoked when the receiver reconstructs a missing RTP packet.
 *
 * If media RTP header extensions are present, the callback receives a normalized
 * RTP packet containing the fixed RTP header and recovered MPEG-TS payload with
 * the RTP extension bit cleared. ST 2022-1 FEC does not protect extension
 * header bytes.
 */
typedef void (*s2022_recovered_packet_cb)(void *user,
                                          const uint8_t *packet,
                                          size_t packet_len);

/**
 * @brief Initialize a configuration structure with interoperable defaults.
 *
 * Defaults are Level B, L=5, D=5, FEC payload type 96, FEC SSRC 0, and
 * receiver stream auto-selection for Level A.
 *
 * @param[out] config Configuration to initialize. May be NULL, in which case
 * this function does nothing.
 */
void s2022_config_init(s2022_config *config);

/**
 * @brief Create a FEC encoder.
 *
 * @param[in] config Encoder configuration. If NULL, defaults are used.
 * @param[out] encoder Receives the allocated encoder context.
 * @return S2022_OK on success, otherwise an error status.
 */
s2022_status s2022_encoder_create(const s2022_config *config,
                                  s2022_encoder **encoder);

/**
 * @brief Destroy an encoder created by @ref s2022_encoder_create.
 *
 * @param[in,out] encoder Encoder to destroy. NULL is allowed.
 */
void s2022_encoder_destroy(s2022_encoder *encoder);

/**
 * @brief Reset an encoder to the start of a new RTP/FEC session.
 *
 * This clears accumulated media packets, queued FEC packets, RTP sequence
 * continuity state, packet-size state, and RTP extension session state.
 */
void s2022_encoder_reset(s2022_encoder *encoder);

/**
 * @brief Push one media RTP MPEG-TS packet into the encoder.
 *
 * The packet must use RTP version 2 with CSRC count zero. If RTP header
 * extensions are present, the extension bit, profile, and extension length
 * must remain constant for the session. Media RTP sequence numbers must be
 * contiguous.
 *
 * Generated FEC packets are delivered synchronously through @p callback.
 * Row/second-stream FEC is emitted immediately after each protected row.
 * Column/first-stream FEC is delayed until at least L subsequent media packets
 * have been observed, satisfying ST 2022-1 traffic-shaping requirements.
 *
 * @param[in,out] encoder Encoder context.
 * @param[in] rtp_packet Complete RTP media packet.
 * @param[in] rtp_packet_len RTP packet length in bytes.
 * @param[in] callback Optional FEC packet callback.
 * @param[in] callback_user User pointer passed to @p callback.
 * @return S2022_OK on success, otherwise an error status.
 */
s2022_status s2022_encoder_push_rtp(s2022_encoder *encoder,
                                    const uint8_t *rtp_packet,
                                    size_t rtp_packet_len,
                                    s2022_fec_packet_cb callback,
                                    void *callback_user);

/**
 * @brief Emit any queued first-stream FEC packets whose required delay elapsed.
 *
 * This is useful when ending a stream or segment. The function returns
 * S2022_ERROR_NOT_READY if queued first-stream packets still have not satisfied
 * the ST 2022-1 minimum L-packet send delay.
 */
s2022_status s2022_encoder_flush(s2022_encoder *encoder,
                                 s2022_fec_packet_cb callback,
                                 void *callback_user);

/**
 * @brief Parse and validate the fixed fields of a SMPTE ST 2022-1 FEC packet.
 *
 * @param[in] fec_packet Complete FEC RTP packet.
 * @param[in] fec_packet_len FEC packet length in bytes.
 * @param[out] info Parsed FEC metadata.
 * @return S2022_OK if the packet header is valid for ST 2022-1.
 */
s2022_status s2022_parse_fec_packet(const uint8_t *fec_packet,
                                    size_t fec_packet_len,
                                    s2022_fec_info *info);

/**
 * @brief Recover one missing media RTP packet from a FEC packet and peer media.
 *
 * @p media_packets must contain exactly the number of packet slots specified by
 * the FEC packet's NA field. The missing slot is identified by @p missing_index;
 * all other slots must reference available RTP media packets.
 *
 * @param[in] fec_packet Complete FEC RTP packet.
 * @param[in] fec_packet_len FEC packet length in bytes.
 * @param[in] media_packets Protected media packet set with one missing slot.
 * @param[in] media_packet_count Number of entries in @p media_packets.
 * @param[in] missing_index Index of the missing packet in the protected set.
 * @param[out] out_packet Destination buffer for the recovered RTP packet.
 * @param[in] out_capacity Capacity of @p out_packet in bytes.
 * @param[out] out_packet_len Recovered packet length, or required capacity on
 * S2022_ERROR_BUFFER_TOO_SMALL.
 * @return S2022_OK on successful recovery, otherwise an error status.
 */
s2022_status s2022_recover_media_packet(const uint8_t *fec_packet,
                                        size_t fec_packet_len,
                                        const s2022_media_packet *media_packets,
                                        size_t media_packet_count,
                                        size_t missing_index,
                                        uint8_t *out_packet,
                                        size_t out_capacity,
                                        size_t *out_packet_len);

/**
 * @brief Create a stateful FEC receiver.
 *
 * @param[in] config Receiver configuration. If NULL, defaults are used.
 * @param[out] receiver Receives the allocated receiver context.
 * @return S2022_OK on success, otherwise an error status.
 */
s2022_status s2022_receiver_create(const s2022_config *config,
                                   s2022_receiver **receiver);

/**
 * @brief Destroy a receiver created by @ref s2022_receiver_create.
 *
 * @param[in,out] receiver Receiver to destroy. NULL is allowed.
 */
void s2022_receiver_destroy(s2022_receiver *receiver);

/**
 * @brief Reset a receiver to the start of a new RTP/FEC session.
 *
 * This clears buffered media, packet-size state, RTP extension session state,
 * and Level A FEC stream auto-selection state.
 */
void s2022_receiver_reset(s2022_receiver *receiver);

/**
 * @brief Store one received media RTP packet for future FEC recovery.
 *
 * The receiver normalizes RTP extension-bearing packets internally because
 * ST 2022-1 protects the fixed RTP header fields and media payload, not the
 * extension header bytes.
 */
s2022_status s2022_receiver_push_media(s2022_receiver *receiver,
                                       const uint8_t *rtp_packet,
                                       size_t rtp_packet_len);

/**
 * @brief Process one received FEC RTP packet and recover if possible.
 *
 * If exactly one associated media packet is missing and all other protected
 * media packets are available, the missing RTP packet is reconstructed and
 * delivered through @p callback. If no packet is missing, the function succeeds
 * without invoking the callback. If more than one packet is missing, it returns
 * S2022_ERROR_NOT_READY.
 */
s2022_status s2022_receiver_push_fec(s2022_receiver *receiver,
                                     const uint8_t *fec_packet,
                                     size_t fec_packet_len,
                                     s2022_recovered_packet_cb callback,
                                     void *callback_user);

/**
 * @brief Query a thread-safe snapshot of receiver statistics.
 *
 * This function is safe to call from a monitoring thread while another thread
 * is feeding packets into the same receiver context.
 *
 * @param[in] receiver Receiver context.
 * @param[out] stats Receives the statistics snapshot.
 * @return S2022_OK on success, otherwise an error status.
 */
s2022_status s2022_receiver_get_stats(s2022_receiver *receiver,
                                      s2022_receiver_stats *stats);

/**
 * @brief Reset receiver statistics and start a new statistics interval.
 *
 * This function is safe to call from a monitoring thread while another thread
 * is feeding packets into the same receiver context.
 *
 * @param[in,out] receiver Receiver context.
 */
void s2022_receiver_reset_stats(s2022_receiver *receiver);

/**
 * @brief Convert a status code to a short static string.
 */
const char *s2022_status_string(s2022_status status);

#ifdef __cplusplus
}
#endif

#endif
