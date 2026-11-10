# Tests and results

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

## Results

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
