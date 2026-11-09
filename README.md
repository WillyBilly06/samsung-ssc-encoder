# samsung-ssc-encoder

An open-source encoder for the **Samsung Scalable Codec**, also called the **Samsung Seamless Codec (SSC)**.
SSC is the Bluetooth **A2DP** vendor codec (vendor ID `0x00000075`, codec ID `0x0103`) that Samsung Galaxy
phones use with **Galaxy Buds**. It covers the plain SSC mode (44.1/48 kHz, 16-bit), the "Seamless" HiFi mode
(48 kHz, 24-bit) and **UHQ** (96 kHz, 24-bit).

The library is portable C99. It writes complete SSC frames: the 4-byte header (sync, sequence number, bitrate
code, end-band side information) and the range-coded payload. The frame sizes, headers and end bands are the
same as Samsung's encoder produces, at every bitrate and mode Samsung uses.

Companion project: [samsung-ssc-decoder](https://github.com/WillyBilly06/samsung-ssc-decoder) (decoder, ESP32 speaker firmware, bitstream docs).

This is a **clean reimplementation for interoperability**. It contains no Samsung code and no Samsung
binaries. SSC is a fork of the CELT layer of Opus 1.1. This encoder adapts the BSD-licensed Opus 1.1 CELT
encoder (range coder, energy quantisation, allocation, PVQ, analysis) to the SSC bitstream. The SSC-specific
rules were determined, for interoperability, by analysing Samsung's encoder and comparing against its output. The tables (band layout, allocation vectors, pulse cache, caps) are the
codec's parameters and are needed to produce a compatible bitstream.

> SSC is *not* Bluetooth LE Audio or LC3. It is a classic A2DP (BR/EDR) vendor codec.

## Status

- Bitstream: all 14 bitrates, both codec modes (44.1/48 kHz and 96 kHz), 16- and 24-bit input. The final
  range-coder position is checked against an SSC decoder on every frame (see [Tests](#tests)): 0 mismatches
  in about 34,600 frames, including silence, transients, postfilter, clipping and per-frame bitrate switching.
- Frame sizes and 4-byte headers are identical to Samsung's encoder for the same input, on every frame of the
  reference recordings.
- Quality matches Samsung's encoder to within about 0.1 dB SNR on music-like input, and is up to 3.4 dB better
  on pure tones at high rates.
- Floating point. On a desktop x86 core it runs about 170x real time at 44.1 kHz / 192 kb/s and about 125x real
  time at 96 kHz / 584 kb/s. No static or global state, no allocation after creation, custom allocator hook.
- **Not verified on real Galaxy Buds yet.** The frames were decoded with an independent SSC decoder that parses
  Samsung's own frames bit-exactly, but this encoder's output has not been played on Samsung hardware.

## Codec modes and bitrates

| Mode (Samsung name) | A2DP flags byte | Sample rate | Input | Bitrates the phone uses | Frames per A2DP packet |
|---|---|---|---|---|---|
| SSC | `0x20` / `0x10` (+`0x08` no limit) | 44.1 / 48 kHz | 16-bit | 192k, 128k, 88k | 1 |
| Seamless HiFi | `0x04` (+`0x08`) | 48 kHz | 24-bit | 192k, 128k, 88k | 1 |
| UHQ (legacy) | `0x80` | 96 kHz | 24-bit in 32 | 328k | 2 |
| UHQ v2 | `0x02` (+`0x08`) | 96 kHz | 24-bit in 32 | 584k, 291k, 152k | 1 or 2 |

44.1 and 48 kHz use the same codec mode, and the sample rate is not signalled in the frame. 96 kHz has its own
band layout. The sink has to know the mode from the A2DP configuration.

All bitrates (bits per second and header rate code). Frame size in bytes includes the 4-byte header:

| Bitrate | Code | hq | 44.1/48 kHz frame | 96 kHz frame | Notes |
|---:|:---:|:---:|---:|---:|---|
| 88k | `F0` | | 224 | 104 | |
| 96k | `F1` | | 244 | 114 | |
| 128k | `F2` | | 324 | 150 | |
| 192k | `F3` | | 484 | 224 | |
| 229k | `F4` | | 576 | 266 | |
| 256k | `F6` | yes | 630 | 292 | |
| 328k | `F5` | yes | 806 | 372 | |
| 152k | `E6` | yes | 376 | 174 | 18 coded bands |
| 250k | `E5` | yes | 616 | 284 | |
| 291k | `E0` | yes | 716 | 330 | |
| 308k | `E1` | yes | 758 | 350 | |
| 442k | `E2` | yes | 1086 | 500 | |
| 584k | `E3` | yes | 1434 | 660 | |
| 886k | `E4` | yes | 2172 | 1000 | |

Frame size: `base = bitrate*864 / (Fs*8)` with `Fs` = 44100 (also used for 48 kHz) or 96000, plus
`base*3 >> 7` for non-hq rates, then `base |= 1`. The payload is `base-1` bytes and the frame is `base+3` bytes.
Each frame holds 864 samples per channel. It always codes two channels.

## Building

CMake (any generator):

```sh
cmake -B build
cmake --build build --config Release
```

It builds the `ssc_encoder` library, `ssc_enc_tool`, `ssc_roundtrip` and `ssc_conformance`.

Plain compiler (gcc or clang):

```sh
gcc -std=c99 -O2 -Iinclude src/*.c examples/ssc_enc_tool.c -o ssc_enc_tool -lm
gcc -std=c99 -O2 -Iinclude -Itests/decoder src/*.c tests/decoder/*.c examples/ssc_roundtrip.c -o ssc_roundtrip -lm
```

The library itself is `src/*.c` and `include/ssc_encoder.h` and has no dependencies besides libm.

## Usage

```c
#include "ssc_encoder.h"

ssc_encoder *enc = ssc_encoder_create(44100);          /* 44100, 48000 or 96000 */
int16_t pcm[2 * SSC_ENC_FRAME_SAMPLES];                /* 864 interleaved stereo samples */
uint8_t frame[SSC_ENC_MAX_FRAME_BYTES];
int len = ssc_encode(enc, pcm, 192000, 0, frame, sizeof frame);   /* bytes written, or < 0 on error */

/* HiFi / UHQ: 24-bit samples in int32 (full scale +-2^23) */
int32_t pcm24[2 * SSC_ENC_FRAME_SAMPLES];
len = ssc_encode_s32(enc, pcm24, 24, 584000, 0, frame, sizeof frame);

ssc_encoder_destroy(enc);
```

- The bitrate can be given in bits per second or as the header rate code (`0xF3`, `0xE3`, ...). It may change
  from frame to frame, as the phone does when the link gets busy.
- `ssc_encode_float()` takes float samples in [-1, 1).
- `ssc_encoder_frame_bytes(rate, bitrate)` gives the frame size. `ssc_encoder_last_frame_info()` reports the
  sequence number, end band, transient and postfilter use, and the final bit position of the last frame.
- Flags: `SSC_ENC_FLAG_NO_POSTFILTER`, `SSC_ENC_FLAG_NO_TRANSIENTS`, `SSC_ENC_FLAG_FLAT_RESPONSE` (see below)
  and `SSC_ENC_FLAG_SAMSUNG_HQ16_LEVEL`. The last one reproduces a Samsung quirk: Samsung scales 16-bit input at
  the hq rates as if it were 24-bit, so it is coded 48 dB quieter. It is only useful for comparisons. Real
  phones always feed 24-bit samples at those rates.
- The decoder output lags the encoder input by 108 samples (the MDCT overlap).

Command-line tool:

```sh
ssc_enc_tool -r 44100 -b 192000 input.s16le out.ssc           # 16-bit stereo PCM
ssc_enc_tool -r 48000 -b 192000 -f s24 input.s24le out.ssc     # HiFi, packed 24-bit
ssc_enc_tool -r 96000 -b 584000 -f s32 input.s32le out.ssc     # UHQ, 24-bit in 32-bit containers
```

The `.ssc` output is a sequence of `[u16 little-endian length][frame]`. On the A2DP link each media packet is
an RTP header followed by one or two whole frames, with no payload header.
`ssc_roundtrip input.s16le [bitrate] [rate] [decoded.s16le]` encodes, decodes again and prints the SNR.

## Encoder design

The bitstream is CELT 1.1 with fixed parameters: LM = 3 (one 1728-point MDCT, or eight 216-point ones for
transients), 108-sample overlap, 22 bands, two coded channels, a 24-bit range coder and no pre-emphasis. On
top of that come the SSC rules: the bit budget is `8*min(payload, 1275)`, the postfilter has no tapset symbol,
the hq rates code no TF symbols, the end band comes from the header, the 18→21 dynalloc propagation, the
allocation table switches above 192 kb/s, and the bands above the end band are reset after each frame.

The encoder decisions are the CELT 1.1 ones, adapted:

- **Pitch prefilter** (pitch search, octave check, comb filter with tapset 0, crossfaded over the overlap).
  The decoder's postfilter inverts it. This matters for tonal content. It is worth about 5 dB SNR on pure tones.
- **Transients**: time-domain detector plus the band-energy "patch" check. Short blocks and anti-collapse.
- **Energy**: two-pass intra/inter coarse quantisation, fine and final energy bits. Integer Q10 arithmetic
  identical to the decoder's, so the prediction state never drifts.
- **TF resolution** (Viterbi over an L1 sparsity metric, non-hq rates), **spreading** decision,
  **dynamic allocation** (CBR limits), **allocation trim**, band-skip hysteresis, **dual stereo** vs mid/side.
  Intensity stereo is not used (Samsung never uses it either).
- **PVQ**: pyramid projection plus greedy pulse search, spreading rotation, recursive band splitting with
  theta quantisation.
- **End-band side information** (header byte 2 bits 2-3 and byte 3 bits 5-7) and the band-21 zeroing below
  192 kb/s, computed like Samsung's encoder. Samsung's fixed high-frequency shaping (at most -1.7 dB above
  about 6-16 kHz, depending on the rate class) is applied by default. `SSC_ENC_FLAG_FLAT_RESPONSE` disables
  it. It lowers the waveform error of the noise-filled top bands. Without it the SNR is up to 0.5 dB lower at
  229-328 kb/s and about the same elsewhere.

The bitstream syntax only depends on integer state, so the output stays decodable whatever the compiler or
floating-point mode. The test suite also passes with `-ffast-math`.

## Tests

`tests/run_tests.py` drives `ssc_conformance` (built by CMake, `ctest` runs it). For each configuration it
encodes the input with this encoder, decodes the frames with the SSC decoder in `tests/decoder/` (a copy of
the companion decoder project), and checks:

1. **Tell mismatches**: frames where the encoder's final range-coder position differs from the decoder's.
   This must be 0. It shows that every symbol is read exactly as it was written.
2. **Size / header vs Samsung**: frames whose size or 4 header bytes differ from Samsung's encoder output for the
   same input and settings.
3. **SNR** of encoder → decoder against the input, next to Samsung's frames → the same decoder.

```sh
cmake -B build && cmake --build build
python3 tests/run_tests.py            # uses tests/vectors (inputs + Samsung reference frames)
```

`tests/vectors/` holds the test inputs and the frames Samsung's own encoder produced from them (the same files as
the decoder project's `test_vectors/`). The script also runs a synthetic stress test: silence, tones, a chirp,
full-scale clipped noise, a square wave, clicks and near-silent noise, at 3 sample rates × 2 bit depths × 14
bitrates, plus per-frame bitrate switching. It checks the tell positions. Current result: 90 runs, 26,100 frames,
0 mismatches.

### Results

`mix` is a 3-second music-like test signal. `mix48k`/`mix96k` are the same kind of content at 48/96 kHz.
`sine` is a pure tone per channel at -24 dBFS. Samsung's reference frames were made with
`libScalable_Encoder.so` on a Galaxy phone. Some reference files are a few frames short (the "of N" column),
and in two UHQ files the reference harness cut one frame short, because it detected frame ends by a 0xA5
fill byte. The "96 kHz, 16-bit" row uses the 44.1 kHz `mix` signal in the 96 kHz mode, as the reference does.

| Mode | Input | Bitrate | Frame bytes | Header (frame 0) | Tell mismatches | Size / header vs Samsung | SNR ours (dB) | SNR Samsung (dB) |
|---|---|---:|---:|---|---:|---|---:|---:|
| 44.1 kHz, 16-bit | mix | 88k | 224 | `ff ee 01 f0` | 0 / 153 | 0 / 0 of 144 | 13.82 | 13.88 |
| 44.1 kHz, 16-bit | mix | 96k | 244 | `ff ee 01 f1` | 0 / 153 | 0 / 0 of 149 | 14.18 | 14.25 |
| 44.1 kHz, 16-bit | mix | 128k | 324 | `ff ee 01 f2` | 0 / 153 | 0 / 0 of 150 | 16.19 | 16.25 |
| 44.1 kHz, 16-bit | mix | 192k | 484 | `ff ee 0d f3` | 0 / 153 | 0 / 0 of 151 | 16.98 | 17.04 |
| 44.1 kHz, 16-bit | mix | 229k | 576 | `ff ee 0d f4` | 0 / 153 | 0 / 0 of 148 | 17.75 | 17.74 |
| 44.1 kHz, 16-bit | mix | 256k * | 630 | `ff ee 01 f6` | 0 / 153 | 0 / 0 of 149 | 17.79 | 17.89 |
| 44.1 kHz, 16-bit | mix | 328k * | 806 | `ff ee 01 f5` | 0 / 153 | 0 / 0 of 152 | 18.30 | 18.33 |
| 44.1 kHz, 16-bit | mix | 152k * | 376 | `ff e0 01 e6` | 0 / 153 | 0 / 0 of 151 | 15.52 | 15.53 |
| 44.1 kHz, 16-bit | mix | 250k * | 616 | `ff e0 0d 05` | 0 / 153 | 0 / 0 of 152 | 17.02 | 17.03 |
| 44.1 kHz, 16-bit | mix | 291k * | 716 | `ff e0 0d 00` | 0 / 153 | 0 / 0 of 148 | 17.07 | 17.07 |
| 44.1 kHz, 16-bit | mix | 308k * | 758 | `ff e0 0d 01` | 0 / 153 | 0 / 0 of 150 | 17.09 | 17.09 |
| 44.1 kHz, 16-bit | mix | 442k * | 1086 | `ff e0 0d 02` | 0 / 153 | 0 / 0 of 150 | 17.11 | 17.10 |
| 44.1 kHz, 16-bit | mix | 584k * | 1434 | `ff e0 0d 03` | 0 / 153 | 0 / 0 of 151 | 17.10 | 17.10 |
| 44.1 kHz, 16-bit | mix | 886k * | 2172 | `ff e0 0d 04` | 0 / 153 | 0 / 0 of 152 | 17.10 | 17.10 |
| 44.1 kHz mode, 24-bit | mix | 88k | 224 | `ff ee 01 f0` | 0 / 153 | n/a | 13.83 | n/a |
| 44.1 kHz mode, 24-bit | mix | 96k | 244 | `ff ee 01 f1` | 0 / 153 | n/a | 14.18 | n/a |
| 44.1 kHz mode, 24-bit | mix | 128k | 324 | `ff ee 01 f2` | 0 / 153 | n/a | 16.19 | n/a |
| 44.1 kHz mode, 24-bit | mix | 192k | 484 | `ff ee 0d f3` | 0 / 153 | 0 / 0 of 153 | 16.98 | 17.04 |
| 44.1 kHz mode, 24-bit | mix | 229k | 576 | `ff ee 0d f4` | 0 / 153 | n/a | 17.75 | n/a |
| 44.1 kHz mode, 24-bit | mix | 256k | 630 | `ff ee 01 f6` | 0 / 153 | 0 / 0 of 153 | 17.85 | 17.82 |
| 44.1 kHz mode, 24-bit | mix | 328k | 806 | `ff ee 01 f5` | 0 / 153 | 0 / 0 of 153 | 18.31 | 18.33 |
| 44.1 kHz mode, 24-bit | mix | 152k | 376 | `ff e0 01 e6` | 0 / 153 | 0 / 0 of 153 | 15.52 | 15.52 |
| 44.1 kHz mode, 24-bit | mix | 250k | 616 | `ff e0 01 e5` | 0 / 153 | 0 / 0 of 153 | 17.78 | 17.76 |
| 44.1 kHz mode, 24-bit | mix | 291k | 716 | `ff e0 01 e0` | 0 / 153 | 0 / 0 of 153 | 18.15 | 18.15 |
| 44.1 kHz mode, 24-bit | mix | 308k | 758 | `ff e0 01 e1` | 0 / 153 | 0 / 0 of 153 | 18.20 | 18.20 |
| 44.1 kHz mode, 24-bit | mix | 442k | 1086 | `ff e0 01 e2` | 0 / 153 | 0 / 0 of 153 | 25.81 | 25.77 |
| 44.1 kHz mode, 24-bit | mix | 584k | 1434 | `ff e0 01 e3` | 0 / 153 | 0 / 0 of 153 | 26.68 | 26.63 |
| 44.1 kHz mode, 24-bit | mix | 886k | 2172 | `ff e0 01 e4` | 0 / 153 | 0 / 0 of 153 | 26.68 | 26.63 |
| 48 kHz, 16-bit | mix | 88k | 224 | `ff ee 01 f0` | 0 / 153 | n/a | 13.83 | n/a |
| 48 kHz, 16-bit | mix | 128k | 324 | `ff ee 01 f2` | 0 / 153 | n/a | 16.19 | n/a |
| 48 kHz, 16-bit | mix | 192k | 484 | `ff ee 0d f3` | 0 / 153 | 0 / 0 of 151 | 16.98 | 17.04 |
| 48 kHz HiFi, 24-bit | mix48k | 88k | 224 | `ff ee 01 f0` | 0 / 166 | n/a | 14.22 | n/a |
| 48 kHz HiFi, 24-bit | mix48k | 128k | 324 | `ff ee 01 f2` | 0 / 166 | n/a | 16.94 | n/a |
| 48 kHz HiFi, 24-bit | mix48k | 192k | 484 | `ff ee 0d f3` | 0 / 166 | 0 / 0 of 166 | 18.31 | 18.32 |
| 96 kHz, 16-bit | mix | 192k | 224 | `ff ee 0d f3` | 0 / 153 | 0 / 0 of 144 | 13.07 | 13.14 |
| 96 kHz UHQ, 24-bit | mix96k | 152k | 174 | `ff e0 01 e6` | 0 / 333 | n/a | 10.98 | n/a |
| 96 kHz UHQ, 24-bit | mix96k | 291k | 330 | `ff e0 01 e0` | 0 / 333 | 1 / 0 of 333 (1 ref frame cut short) | 12.51 | 12.51 |
| 96 kHz UHQ, 24-bit | mix96k | 308k | 350 | `ff e0 01 e1` | 0 / 333 | n/a | 12.92 | n/a |
| 96 kHz UHQ, 24-bit | mix96k | 328k | 372 | `ff ee 01 f5` | 0 / 333 | n/a | 13.53 | n/a |
| 96 kHz UHQ, 24-bit | mix96k | 442k | 500 | `ff e0 01 e2` | 0 / 333 | n/a | 22.50 | n/a |
| 96 kHz UHQ, 24-bit | mix96k | 584k | 660 | `ff e0 01 e3` | 0 / 333 | 1 / 0 of 333 (1 ref frame cut short) | 30.09 | 30.00 |
| 96 kHz UHQ, 24-bit | mix96k | 886k | 1000 | `ff e0 01 e4` | 0 / 333 | n/a | 30.49 | n/a |
| tones | sine | 88k | 224 | `ff ee 01 f0` | 0 / 153 | 0 / 0 of 153 | 32.96 | 33.17 |
| tones | sine | 192k | 484 | `ff ee 01 f3` | 0 / 153 | 0 / 0 of 153 | 41.84 | 41.41 |
| tones | sine | 328k * | 806 | `ff ee 01 f5` | 0 / 153 | 0 / 0 of 153 | 47.27 | 45.79 |
| tones | sine | 886k * | 2172 | `ff e0 0d 04` | 0 / 153 | 0 / 0 of 153 | 51.98 | 48.60 |
| mono input | mono | 192k | 484 | `ff ee 0d f3` | 0 / 153 | n/a | 18.65 | n/a |

\* 16-bit input at an hq rate, encoded with `SSC_ENC_FLAG_SAMSUNG_HQ16_LEVEL` to match Samsung's handling
(coded 48 dB down, which is also why the end band in the header differs from the 24-bit rows). SNR is measured
against the input scaled by 1/256. Mono input is encoded as identical left/right channels.

The SNR is a plain waveform SNR over the whole band, including bands that are noise-filled at the lower rates.
It is meant for comparing the two encoders, not as an absolute quality score.

## Layout

```
include/ssc_encoder.h      public API
src/                       the encoder library
  ssc_encoder.c            frame header, rate table, per-frame flow
  ssc_enc_ec.c             range coder (24-bit), Laplace coder
  ssc_enc_mdct.c           forward MDCT (FFT-based)
  ssc_enc_pitch.c          pitch search and comb prefilter
  ssc_enc_analysis.c       transient, TF, spreading, dynalloc, trim, stereo decisions
  ssc_enc_energy.c         coarse / fine / final energy quantisation
  ssc_enc_alloc.c          bit allocation
  ssc_enc_bands.c          PVQ band coding, stereo and TF splitting
  ssc_enc_tables.c         mode tables
examples/                  ssc_enc_tool (PCM -> .ssc), ssc_roundtrip (encode + decode + SNR)
tests/                     ssc_conformance.c, run_tests.py, decoder/ (SSC decoder copy)
```

## License

BSD 3-Clause, see [LICENSE](LICENSE). The parts adapted from Opus 1.1 keep the Xiph.Org / Opus copyright
notice, which is reproduced in LICENSE.

"Samsung", "Galaxy" and "Galaxy Buds" are trademarks of Samsung Electronics. This project is not affiliated
with or endorsed by Samsung.
