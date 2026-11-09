/* Conformance test for one configuration.
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 *
 * Encodes a raw s16le input with this encoder, decodes the frames with the SSC decoder (tests/decoder), and
 * checks, per frame:
 *   1. the encoder's final range-coder position equals the decoder's (bitstream consistency);
 *   2. frame size and the 4 header bytes equal those of a reference .ssc file (Samsung's encoder output), if given;
 * then reports the SNR of encoder -> decoder against the input, and of the reference frames -> decoder.
 *
 * usage: ssc_conformance -i input.s16le [-r 44100|48000|96000] [-b bitrate[,bitrate...]] [-bits 16|24] [-mono]
 *                        [-samsung-level] [-ref ref.ssc] [-o out.ssc]
 * A comma-separated bitrate list switches the bitrate every frame (frame k uses entry k mod n).
 * Output: key=value lines. Exit status 0 only if every frame decoded and every tell matched. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssc_decoder.h"
#include "ssc_encoder.h"

extern int ssc_debug_tell, ssc_debug_total_bits, ssc_debug_storage_bits; /* decoder built with SSC_DEBUG_TELL */

#define N SSC_ENC_FRAME_SAMPLES

static unsigned char *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)*len + 1);
    if (buf && fread(buf, 1, (size_t)*len, f) != (size_t)*len) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

typedef struct {
    int n;
    int *len;
    unsigned char **data;
} frame_list;

static void parse_ssc(const unsigned char *d, long size, frame_list *fl) {
    long p = 0;
    int cap = 1024;
    fl->n = 0;
    fl->len = (int *)malloc(sizeof(int) * cap);
    fl->data = (unsigned char **)malloc(sizeof(unsigned char *) * cap);
    while (p + 2 <= size) {
        int l = d[p] | d[p + 1] << 8;
        p += 2;
        if (p + l > size) break;
        if (fl->n == cap) {
            cap *= 2;
            fl->len = (int *)realloc(fl->len, sizeof(int) * cap);
            fl->data = (unsigned char **)realloc(fl->data, sizeof(unsigned char *) * cap);
        }
        fl->len[fl->n] = l;
        fl->data[fl->n] = (unsigned char *)d + p;
        fl->n++;
        p += l;
    }
}

/* SNR in dB of y (decoded, interleaved stereo float in 16-bit units) against x (reference). The decoder output
 * lags the encoder input by exactly the 108-sample MDCT overlap. The first 4096 samples are skipped. */
#define CODEC_DELAY 108
static double snr_db(const float *x, const float *y, long nsamp) {
    double num = 0, den = 0;
    long i;
    for (i = 4096; i + CODEC_DELAY < nsamp; i++) {
        int c;
        for (c = 0; c < 2; c++) {
            double a = x[2 * i + c], e = a - y[2 * (i + CODEC_DELAY) + c];
            num += a * a;
            den += e * e;
        }
    }
    if (den <= 0) return 200.0;
    return 10.0 * log10(num / den);
}

/* Decodes a frame list; pads frames shorter than their header-derived size (the reference harness cut trailing
 * bytes equal to its 0xA5 fill value). Returns the number of decode errors. */
static int decode_all(const frame_list *fl, int rate, float *out, long nsamp_max, int *tells, int *pad_count) {
    ssc_decoder *dec = ssc_decoder_create(rate);
    float pcm[2 * N];
    unsigned char tmp[4096];
    int k, errors = 0;
    *pad_count = 0;
    for (k = 0; k < fl->n && (long)(k + 1) * N <= nsamp_max; k++) {
        const unsigned char *f = fl->data[k];
        int len = fl->len[k], want = ssc_frame_bytes(f, len, rate);
        if (want > len && want <= (int)sizeof tmp) {
            memcpy(tmp, f, (size_t)len);
            memset(tmp + len, 0xA5, (size_t)(want - len));
            f = tmp;
            len = want;
            (*pad_count)++;
        }
        if (ssc_decode_float(dec, f, len, pcm) < 0) errors++;
        if (tells) tells[k] = ssc_debug_tell;
        {
            int j;
            for (j = 0; j < 2 * N; j++) out[(long)k * 2 * N + j] = pcm[j] * 32768.f;
        }
    }
    ssc_decoder_destroy(dec);
    return errors;
}

int main(int argc, char **argv) {
    const char *in_path = NULL, *ref_path = NULL, *out_path = NULL;
    int rate = 44100, bitrate = 192000, bits = 16, mono = 0, samsung_level = 0, a, nrates = 1;
    int rates_list[32] = {192000};
    long in_len, nsamp, i;
    unsigned char *in_raw;
    float *x, *y_ours, *y_ref = NULL;
    int16_t *pcm16;
    int nframes, k, tell_mismatch = 0, enc_errors = 0, dec_errors, transient = 0, silent = 0, pf = 0;
    int *enc_tells, *dec_tells;
    frame_list ours, ref;
    unsigned char *ours_buf;
    long ours_size = 0;
    ssc_encoder *enc;
    FILE *fo = NULL;

    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-i") && a + 1 < argc) in_path = argv[++a];
        else if (!strcmp(argv[a], "-r") && a + 1 < argc) rate = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-b") && a + 1 < argc) {
            char *p = argv[++a];
            nrates = 0;
            while (*p && nrates < 32) {
                rates_list[nrates++] = (int)strtol(p, &p, 0);
                if (*p == ',') p++;
            }
            bitrate = rates_list[0];
        }
        else if (!strcmp(argv[a], "-bits") && a + 1 < argc) bits = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-mono")) mono = 1;
        else if (!strcmp(argv[a], "-samsung-level")) samsung_level = 1;
        else if (!strcmp(argv[a], "-ref") && a + 1 < argc) ref_path = argv[++a];
        else if (!strcmp(argv[a], "-o") && a + 1 < argc) out_path = argv[++a];
        else {
            fprintf(stderr, "unknown argument %s\n", argv[a]);
            return 2;
        }
    }
    if (!in_path || !(in_raw = read_file(in_path, &in_len))) {
        fprintf(stderr, "cannot read input\n");
        return 2;
    }
    nsamp = in_len / 2 / (mono ? 1 : 2);
    nframes = (int)(nsamp / N);
    nsamp = (long)nframes * N;
    /* interleaved stereo reference signal in 16-bit units (mono is duplicated) */
    x = (float *)malloc(sizeof(float) * 2 * nsamp);
    pcm16 = (int16_t *)malloc(sizeof(int16_t) * 2 * nsamp);
    for (i = 0; i < nsamp; i++) {
        int c;
        for (c = 0; c < 2; c++) {
            long idx = mono ? i : 2 * i + c;
            int16_t s = (int16_t)(in_raw[2 * idx] | in_raw[2 * idx + 1] << 8);
            pcm16[2 * i + c] = s;
            x[2 * i + c] = s;
        }
    }

    /* encode */
    enc = ssc_encoder_create(rate);
    if (!enc) {
        fprintf(stderr, "encoder create failed\n");
        return 2;
    }
    ours_buf = (unsigned char *)malloc((size_t)nframes * (SSC_ENC_MAX_FRAME_BYTES + 2));
    enc_tells = (int *)malloc(sizeof(int) * nframes);
    dec_tells = (int *)malloc(sizeof(int) * nframes);
    for (k = 0; k < nframes; k++) {
        unsigned char *f = ours_buf + ours_size + 2;
        ssc_encoder_frame_info info;
        int ret;
        bitrate = rates_list[k % nrates];
        if (bits == 24) {
            int32_t s32[2 * N];
            int j;
            for (j = 0; j < 2 * N; j++) s32[j] = (int32_t)pcm16[(long)k * 2 * N + j] * 256;
            ret = ssc_encode_s32(enc, s32, 24, bitrate, 0, f, SSC_ENC_MAX_FRAME_BYTES);
        } else {
            ret = ssc_encode(enc, pcm16 + (long)k * 2 * N, bitrate, samsung_level ? SSC_ENC_FLAG_SAMSUNG_HQ16_LEVEL : 0,
                             f, SSC_ENC_MAX_FRAME_BYTES);
        }
        if (ret < 0) {
            enc_errors++;
            enc_tells[k] = -1;
            continue;
        }
        ssc_encoder_last_frame_info(enc, &info);
        enc_tells[k] = info.tell_bits;
        transient += info.transient;
        silent += info.silence;
        pf += info.pf_on;
        ours_buf[ours_size] = (unsigned char)(ret & 0xFF);
        ours_buf[ours_size + 1] = (unsigned char)(ret >> 8);
        ours_size += 2 + ret;
    }
    ssc_encoder_destroy(enc);
    if (out_path && (fo = fopen(out_path, "wb"))) {
        fwrite(ours_buf, 1, (size_t)ours_size, fo);
        fclose(fo);
    }
    parse_ssc(ours_buf, ours_size, &ours);

    /* decode our frames, compare tells */
    y_ours = (float *)calloc((size_t)2 * nsamp, sizeof(float));
    {
        int pads;
        dec_errors = decode_all(&ours, rate, y_ours, nsamp, dec_tells, &pads);
    }
    for (k = 0; k < ours.n; k++)
        if (enc_tells[k] != dec_tells[k]) {
            if (tell_mismatch < 5)
                fprintf(stderr, "frame %d: encoder tell %d, decoder tell %d\n", k, enc_tells[k], dec_tells[k]);
            tell_mismatch++;
        }

    /* level of the reference signal: Samsung-level 16-bit hq frames are 48 dB down */
    if (samsung_level && ssc_encoder_frame_bytes(rate, bitrate) > 0) {
        int hq = 0;
        if (ours.n > 0) {
            ssc_frame_info fi;
            if (!ssc_parse_frame_info(ours.data[0], ours.len[0], &fi)) hq = fi.hq;
        }
        if (hq)
            for (i = 0; i < 2 * nsamp; i++) x[i] *= 1.f / 256.f;
    }

    printf("frames=%d\n", ours.n);
    printf("frame_bytes=%d\n", ours.n ? ours.len[0] : 0);
    printf("encode_errors=%d\n", enc_errors);
    printf("decode_errors=%d\n", dec_errors);
    printf("tell_mismatch=%d\n", tell_mismatch);
    printf("transient_frames=%d\n", transient);
    printf("silence_frames=%d\n", silent);
    printf("postfilter_frames=%d\n", pf);
    printf("header=%02x %02x %02x %02x\n", ours.n ? ours.data[0][0] : 0, ours.n ? ours.data[0][1] : 0,
           ours.n ? ours.data[0][2] : 0, ours.n ? ours.data[0][3] : 0);
    printf("snr_ours=%.2f\n", snr_db(x, y_ours, nsamp));

    if (ref_path) {
        long ref_size;
        unsigned char *ref_buf = read_file(ref_path, &ref_size);
        if (ref_buf) {
            int size_mismatch = 0, header_mismatch = 0, ref_errors, pads, ncmp;
            long ref_nsamp;
            parse_ssc(ref_buf, ref_size, &ref);
            ncmp = ref.n < ours.n ? ref.n : ours.n;
            for (k = 0; k < ncmp; k++) {
                if (ref.len[k] != ours.len[k]) size_mismatch++;
                if (memcmp(ref.data[k], ours.data[k], 4)) {
                    if (header_mismatch < 3)
                        fprintf(stderr, "frame %d header: ours %02x %02x %02x %02x, ref %02x %02x %02x %02x\n", k,
                                ours.data[k][0], ours.data[k][1], ours.data[k][2], ours.data[k][3], ref.data[k][0],
                                ref.data[k][1], ref.data[k][2], ref.data[k][3]);
                    header_mismatch++;
                }
            }
            ref_nsamp = (long)ref.n * N < nsamp ? (long)ref.n * N : nsamp;
            y_ref = (float *)calloc((size_t)2 * nsamp, sizeof(float));
            ref_errors = decode_all(&ref, rate, y_ref, ref_nsamp, NULL, &pads);
            printf("ref_frames=%d\n", ref.n);
            printf("ref_frame_bytes=%d\n", ref.n ? ref.len[0] : 0);
            printf("ref_header=%02x %02x %02x %02x\n", ref.n ? ref.data[0][0] : 0, ref.n ? ref.data[0][1] : 0,
                   ref.n ? ref.data[0][2] : 0, ref.n ? ref.data[0][3] : 0);
            printf("compared_frames=%d\n", ncmp);
            printf("size_mismatch=%d\n", size_mismatch);
            printf("ref_short_frames=%d\n", pads);
            printf("header_mismatch=%d\n", header_mismatch);
            printf("ref_decode_errors=%d\n", ref_errors);
            /* SNR over the frames both streams cover, so the two numbers are comparable */
            printf("snr_ours_common=%.2f\n", snr_db(x, y_ours, ref_nsamp));
            printf("snr_ref=%.2f\n", snr_db(x, y_ref, ref_nsamp));
        }
    }
    return (enc_errors || dec_errors || tell_mismatch) ? 1 : 0;
}
