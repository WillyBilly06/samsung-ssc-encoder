/* ssc_enc_tool: raw PCM -> SSC frames.
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 *
 * Output format: a sequence of [u16 little-endian frame length][frame bytes], one entry per 864-sample frame.
 * The last partial frame is padded with silence.
 *
 * usage: ssc_enc_tool [options] input.raw output.ssc
 *   -r RATE     sample rate: 44100 (default), 48000 or 96000
 *   -b BITRATE  bits per second or header rate code (default 192000), e.g. 328000, 584000, 0xE3
 *   -f FORMAT   input sample format, interleaved stereo little-endian:
 *                 s16     16-bit (default)
 *                 s24     packed 24-bit (3 bytes per sample)
 *                 s32     32-bit container with 24 significant bits (the UHQ 96 kHz format)
 *                 s16as24 16-bit samples fed as 24-bit input (x256), as Samsung's HiFi/UHQ path does
 *   -m          mono input (duplicated to both channels)
 *   -q          quiet
 *   --samsung-level  reproduce Samsung's level for 16-bit input at hq rates (48 dB down)
 *   --no-pf          disable the pitch prefilter
 *   --no-transients  disable short blocks */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssc_encoder.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define N SSC_ENC_FRAME_SAMPLES

static void usage(void) {
    fprintf(stderr,
            "usage: ssc_enc_tool [-r 44100|48000|96000] [-b bitrate] [-f s16|s24|s32|s16as24] [-m] [-q]\n"
            "                    [--samsung-level] [--no-pf] [--no-transients] input.raw output.ssc\n");
}

int main(int argc, char **argv) {
    int rate = 44100, bitrate = 192000, mono = 0, quiet = 0, a, frames = 0, bytes_per_sample = 2;
    unsigned flags = 0;
    const char *fmt = "s16", *in_path = NULL, *out_path = NULL;
    FILE *fi, *fo;
    ssc_encoder *enc;
    static unsigned char raw[2 * N * 4];
    static int32_t s32[2 * N];
    static int16_t s16[2 * N];
    unsigned char out[SSC_ENC_MAX_FRAME_BYTES];
    long total = 0;

    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-r") && a + 1 < argc) rate = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-b") && a + 1 < argc) bitrate = (int)strtol(argv[++a], NULL, 0);
        else if (!strcmp(argv[a], "-f") && a + 1 < argc) fmt = argv[++a];
        else if (!strcmp(argv[a], "-m")) mono = 1;
        else if (!strcmp(argv[a], "-q")) quiet = 1;
        else if (!strcmp(argv[a], "--samsung-level")) flags |= SSC_ENC_FLAG_SAMSUNG_HQ16_LEVEL;
        else if (!strcmp(argv[a], "--no-pf")) flags |= SSC_ENC_FLAG_NO_POSTFILTER;
        else if (!strcmp(argv[a], "--no-transients")) flags |= SSC_ENC_FLAG_NO_TRANSIENTS;
        else if (argv[a][0] == '-' && argv[a][1]) {
            usage();
            return 1;
        } else if (!in_path) in_path = argv[a];
        else if (!out_path) out_path = argv[a];
        else {
            usage();
            return 1;
        }
    }
    if (!in_path || !out_path) {
        usage();
        return 1;
    }
    if (!strcmp(fmt, "s16") || !strcmp(fmt, "s16as24")) bytes_per_sample = 2;
    else if (!strcmp(fmt, "s24")) bytes_per_sample = 3;
    else if (!strcmp(fmt, "s32")) bytes_per_sample = 4;
    else {
        fprintf(stderr, "unknown format %s\n", fmt);
        return 1;
    }
    if (!ssc_encoder_frame_bytes(rate, bitrate)) {
        fprintf(stderr, "unsupported sample rate %d or bitrate %d\n", rate, bitrate);
        return 1;
    }
    if (!strcmp(in_path, "-")) {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        fi = stdin;
    } else {
        fi = fopen(in_path, "rb");
    }
    fo = fopen(out_path, "wb");
    if (!fi || !fo) {
        perror("fopen");
        return 1;
    }
    enc = ssc_encoder_create(rate);
    if (!enc) {
        fprintf(stderr, "cannot create encoder\n");
        return 1;
    }
    for (;;) {
        const int ch_in = mono ? 1 : 2;
        size_t want = (size_t)N * ch_in * bytes_per_sample, got = fread(raw, 1, want, fi);
        int i, ret;
        if (got == 0) break;
        if (got < want) memset(raw + got, 0, want - got);
        for (i = 0; i < 2 * N; i++) {
            const unsigned char *p = raw + (size_t)(mono ? i / 2 : i) * bytes_per_sample;
            int32_t v;
            if (bytes_per_sample == 2) v = (int16_t)(p[0] | p[1] << 8);
            else if (bytes_per_sample == 3) v = (int32_t)((uint32_t)(p[0] | p[1] << 8 | p[2] << 16) << 8) >> 8;
            else v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
            if (!strcmp(fmt, "s16")) s16[i] = (int16_t)v;
            else s32[i] = !strcmp(fmt, "s16as24") ? v * 256 : v;
        }
        if (!strcmp(fmt, "s16")) ret = ssc_encode(enc, s16, bitrate, flags, out, sizeof out);
        else ret = ssc_encode_s32(enc, s32, 24, bitrate, flags, out, sizeof out);
        if (ret < 0) {
            fprintf(stderr, "encode error %d\n", ret);
            return 1;
        }
        fputc(ret & 0xFF, fo);
        fputc(ret >> 8, fo);
        fwrite(out, 1, (size_t)ret, fo);
        total += ret;
        frames++;
        if (got < want) break;
    }
    if (!quiet)
        fprintf(stderr, "%d frames, %d bytes each, %ld bytes of SSC (%d Hz mode, %d bit/s)\n", frames,
                ssc_encoder_frame_bytes(rate, bitrate), total, rate == 96000 ? 96000 : 44100,
                ssc_encoder_bitrate(bitrate));
    ssc_encoder_destroy(enc);
    if (fi != stdin) fclose(fi);
    fclose(fo);
    return 0;
}
