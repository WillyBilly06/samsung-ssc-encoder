/* ssc_roundtrip: encode raw PCM to SSC, decode it again, and print the SNR.
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 *
 * Shows the encoder and decoder APIs together. The decoder is the copy in tests/decoder.
 *
 * usage: ssc_roundtrip input.s16le [bitrate] [sample_rate] [decoded.s16le]
 *   input: interleaved stereo 16-bit little-endian PCM. The decoded output (optional) lags the input by
 *   108 samples (the MDCT overlap); the SNR below accounts for that. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssc_decoder.h"
#include "ssc_encoder.h"

#define N SSC_ENC_FRAME_SAMPLES
#define DELAY 108

int main(int argc, char **argv) {
    const char *in_path, *dec_path;
    int bitrate, rate, frames = 0;
    FILE *fi, *fo = NULL;
    ssc_encoder *enc;
    ssc_decoder *dec;
    int16_t pcm[2 * N], out[2 * N];
    static int16_t hist[2 * (N + DELAY)]; /* input delayed by DELAY to line up with the decoder output */
    unsigned char frame[SSC_ENC_MAX_FRAME_BYTES];
    double sig = 0, err = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: ssc_roundtrip input.s16le [bitrate] [sample_rate] [decoded.s16le]\n");
        return 1;
    }
    in_path = argv[1];
    bitrate = argc > 2 ? (int)strtol(argv[2], NULL, 0) : 192000;
    rate = argc > 3 ? atoi(argv[3]) : 44100;
    dec_path = argc > 4 ? argv[4] : NULL;

    fi = fopen(in_path, "rb");
    if (!fi) {
        perror(in_path);
        return 1;
    }
    if (dec_path && !(fo = fopen(dec_path, "wb"))) {
        perror(dec_path);
        return 1;
    }
    enc = ssc_encoder_create(rate);
    dec = ssc_decoder_create(rate);
    if (!enc || !dec) {
        fprintf(stderr, "unsupported sample rate %d\n", rate);
        return 1;
    }
    while (fread(pcm, sizeof(int16_t) * 2, N, fi) == N) {
        int len = ssc_encode(enc, pcm, bitrate, 0, frame, sizeof frame), i;
        ssc_encoder_frame_info info;
        if (len < 0) {
            fprintf(stderr, "encode error %d (bitrate %d)\n", len, bitrate);
            return 1;
        }
        ssc_encoder_last_frame_info(enc, &info);
        if (ssc_decode(dec, frame, len, out) < 0) {
            fprintf(stderr, "decode error in frame %d\n", frames);
            return 1;
        }
        if (fo) fwrite(out, sizeof(int16_t), 2 * N, fo);
        /* compare out[] with the input of 108 samples earlier */
        memcpy(hist + 2 * DELAY, pcm, sizeof pcm);
        if (frames > 4)
            for (i = 0; i < 2 * N; i++) {
                double x = hist[i], e = x - out[i];
                sig += x * x;
                err += e * e;
            }
        memmove(hist, hist + 2 * N, sizeof(int16_t) * 2 * DELAY);
        if (frames == 0)
            printf("frame size %d bytes, header %02x %02x %02x %02x, end band %d\n", len, frame[0], frame[1],
                   frame[2], frame[3], info.end_band);
        frames++;
    }
    printf("%d frames, SNR %.2f dB\n", frames, err > 0 ? 10 * log10(sig / err) : 200.0);
    ssc_encoder_destroy(enc);
    ssc_decoder_destroy(dec);
    fclose(fi);
    if (fo) fclose(fo);
    return 0;
}
