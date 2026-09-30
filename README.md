# klsmpte2022_1

Standalone C library for SMPTE ST 2022-1 style XOR forward error correction for RTP MPEG-TS streams.

The library does not own sockets. Applications feed RTP MPEG-TS packets into an encoder and receive generated first-stream and second-stream FEC packets through a callback. That keeps it simple to embed in existing C or C++ UDP transmitters, receivers, gateways, and test tools.

## Features

- C99 public API with C++ compatible headers.
- CMake package and target: `klsmpte2022_1::klsmpte2022_1`, producing `libklsmpte2022_1`.
- First-stream and second-stream FEC generation over configurable `L x D` blocks.
- Sender modes for disabled FEC, Level A, and Level B operation.
- Single-packet recovery helper for a FEC packet when exactly one protected RTP packet is missing.
- Receiver state that buffers RTP media packets and recovers from FEC packets using `SNBase`, `Offset`, and `NA`.
- Thread-safe receiver statistics snapshots for processed media/FEC bytes, recovered bytes, recovery attempts, and recovery error rate.
- No external runtime dependencies.

## License

This project is licensed under the GNU Lesser General Public License version 2.1 only. See [LICENSE](LICENSE).

## Scope

This first version supports fixed-size RTP packets with no CSRC list. RTP header extensions are supported when the extension bit, profile, and length remain constant for the session. It is intentionally socket-agnostic; transmit first-stream FEC packets on RTP base port + 2 and second-stream FEC packets on RTP base port + 4.

For MPEG-TS profile use, the encoder accepts `D` from 4 to 20, `L` from 1 to 20 for Level A, `L` from 4 to 20 for Level B, and `L * D <= 100`.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Generate the developer API documentation with Doxygen:

```sh
cmake --build build --target docs
```

The HTML entry point is `build/docs/html/index.html`.

Disable examples or tests when embedding:

```sh
cmake -S . -B build -DSMPTE2022_BUILD_EXAMPLES=OFF -DSMPTE2022_BUILD_TESTS=OFF
```

## Demo Transmitter

The `klsmpte2022-1_transmit_demo` example sends a paced RTP MPEG-TS stream plus SMPTE ST 2022-1 FEC packets over UDP:

```sh
./build/klsmpte2022-1_transmit_demo --url udp://239.10.10.10:5000 --bitrate 20m --fec b --l 5 --d 5
```

The media RTP stream is sent to the configured UDP port, first-stream FEC is sent to port `N+2`, and second-stream FEC is sent to port `N+4`. The example uses one UDP socket so all three streams share the same source port. It generates valid fixed-size RTP packets containing MPEG-TS PAT/PMT/null packets; this is useful for FEC interoperability and lock tests, but it does not contain a decodable video elementary stream.

The companion receiver listens on the same port triplet, runs the ST 2022-1 recovery engine, and can forward a repaired RTP media stream after a configurable packet-latency buffer:

```sh
./build/klsmpte2022-1_receive_demo --url udp://239.10.10.10:5000 --fec b --l 5 --d 5 --latency 256 --output udp://127.0.0.1:6000
```

Use `--interface A.B.C.D` when joining multicast on a specific local interface. The `--drop-every N` option is a local test aid that discards every Nth received media packet before FEC processing, making recovery behavior easy to observe with the demo transmitter.

## Embed With CMake

```cmake
add_subdirectory(path/to/smpte2022)
target_link_libraries(your_target PRIVATE klsmpte2022_1::klsmpte2022_1)
```

Or after installation:

```cmake
find_package(klsmpte2022_1 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE klsmpte2022_1::klsmpte2022_1)
```

## Basic Use

```c
#include <klsmpte2022_1/fec.h>

static void on_fec(void *user, s2022_fec_type type,
                   const uint8_t *packet, size_t packet_len)
{
    /* Send S2022_FEC_FIRST on N+2 and S2022_FEC_SECOND on N+4. */
    (void)user;
    (void)type;
    (void)packet;
    (void)packet_len;
}

void feed_packets(const uint8_t *rtp, size_t rtp_len)
{
    s2022_config config;
    s2022_encoder *encoder;

    s2022_config_init(&config);
    config.mode = S2022_FEC_MODE_LEVEL_B;
    config.columns_l = 5;
    config.rows_d = 5;

    if (s2022_encoder_create(&config, &encoder) != S2022_OK) {
        return;
    }

    (void)s2022_encoder_push_rtp(encoder, rtp, rtp_len, on_fec, NULL);
    s2022_encoder_destroy(encoder);
}
```

Create one encoder per RTP stream and keep it alive across packets so it can accumulate complete `L x D` FEC blocks.

The SMPTE ST 2022-1 FEC RTP payload type is fixed to 96 and the FEC RTP SSRC is fixed to zero. `s2022_encoder_create()` rejects configurations that change those reserved values.

Level A emits only the first FEC stream. Level B emits the first and second FEC streams. Disabled mode validates RTP input but emits no FEC packets. First-stream FEC is delayed until at least `L` media packets after the last protected media packet, as required by ST 2022-1 traffic shaping. `s2022_encoder_flush()` drains queued first-stream FEC only after that delay has elapsed; otherwise it returns `S2022_ERROR_NOT_READY`. A Level A receiver auto-selects the first FEC stream it receives by default, or can be pinned to the first or second stream with `receiver_stream`.

When RTP header extensions are present, recovery callbacks return a normalized RTP packet containing the fixed RTP header and recovered MPEG-TS payload. The extension bit is cleared because the RTP extension header bytes are not protected by ST 2022-1 FEC.

Receivers also expose `s2022_receiver_get_stats()` and `s2022_receiver_reset_stats()`. The snapshot includes `reset_time` and `sampled_time` so applications can compute packet and byte rates over their own reporting windows.

## Notes

SMPTE ST 2022-1 is derived from the Pro-MPEG Code of Practice #3 Release 2 FEC model: L columns, D rows, row parity with offset 1, and column parity with offset L. Public context for the RTP FEC format is also described by RFC 6015.
