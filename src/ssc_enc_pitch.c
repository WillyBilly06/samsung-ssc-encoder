/* SSC encoder: pitch analysis and the comb prefilter (the decoder runs the inverse postfilter).
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 * Adapted from libopus 1.1 celt/pitch.c, celt/celt_lpc.c and celt/celt.c, float build
 * (Copyright 2007-2009 Xiph.Org Foundation, 2007-2008 CSIRO, BSD-3-Clause, see LICENSE).
 *
 * SSC has no tapset symbol: the filter always uses CELT tapset 0. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ssc_enc_internal.h"

static const float comb_gains[3] = {0.3066406250f, 0.2170410156f, 0.1296386719f};

static void celt_lpc(float *lpc, const float *ac, int p) {
    int i, j;
    float error = ac[0];
    for (i = 0; i < p; i++) lpc[i] = 0;
    if (ac[0] == 0) return;
    for (i = 0; i < p; i++) {
        float rr = 0, r;
        for (j = 0; j < i; j++) rr += lpc[j] * ac[i - j];
        rr += ac[i + 1];
        r = -rr / error;
        lpc[i] = r;
        for (j = 0; j < (i + 1) >> 1; j++) {
            float t1 = lpc[j], t2 = lpc[i - 1 - j];
            lpc[j] = t1 + r * t2;
            lpc[i - 1 - j] = t2 + r * t1;
        }
        error = error - r * r * error;
        if (error < .001f * ac[0]) break; /* 30 dB prediction gain is enough */
    }
}

/* Low-pass and decimate by 2, sum of the channels, then a 4th-order LPC whitening (with an extra zero). */
void sscenc_pitch_downsample(const float *x0, const float *x1, float *x_lp, int len) {
    float ac[5], lpc[4], lpc2[5], mem[5] = {0, 0, 0, 0, 0}, tmp = 1.f;
    const float c1 = .8f;
    int i, k;
    for (i = 1; i < len >> 1; i++)
        x_lp[i] = .5f * (.5f * (x0[2 * i - 1] + x0[2 * i + 1]) + x0[2 * i]) +
                  .5f * (.5f * (x1[2 * i - 1] + x1[2 * i + 1]) + x1[2 * i]);
    x_lp[0] = .5f * (.5f * x0[1] + x0[0]) + .5f * (.5f * x1[1] + x1[0]);
    for (k = 0; k <= 4; k++) {
        double s = 0;
        for (i = k; i < len >> 1; i++) s += (double)x_lp[i] * x_lp[i - k];
        ac[k] = (float)s;
    }
    ac[0] *= 1.0001f; /* noise floor -40 dB */
    for (i = 1; i <= 4; i++) ac[i] -= ac[i] * (.008f * i) * (.008f * i); /* lag window */
    celt_lpc(lpc, ac, 4);
    for (i = 0; i < 4; i++) {
        tmp *= .9f;
        lpc[i] *= tmp;
    }
    lpc2[0] = lpc[0] + .8f;
    lpc2[1] = lpc[1] + c1 * lpc[0];
    lpc2[2] = lpc[2] + c1 * lpc[1];
    lpc2[3] = lpc[3] + c1 * lpc[2];
    lpc2[4] = c1 * lpc[3];
    for (i = 0; i < len >> 1; i++) {
        float x = x_lp[i];
        float sum = x + lpc2[0] * mem[0] + lpc2[1] * mem[1] + lpc2[2] * mem[2] + lpc2[3] * mem[3] + lpc2[4] * mem[4];
        mem[4] = mem[3];
        mem[3] = mem[2];
        mem[2] = mem[1];
        mem[1] = mem[0];
        mem[0] = x;
        x_lp[i] = sum;
    }
}

static void find_best_pitch(const float *xcorr, const float *y, int len, int max_pitch, int *best_pitch) {
    int i, j;
    float Syy = 1, best_num[2] = {-1, -1}, best_den[2] = {0, 0};
    best_pitch[0] = 0;
    best_pitch[1] = 1;
    for (j = 0; j < len; j++) Syy += y[j] * y[j];
    for (i = 0; i < max_pitch; i++) {
        if (xcorr[i] > 0) {
            float xc = xcorr[i] * 1e-12f, num = xc * xc;
            if (num * best_den[1] > best_num[1] * Syy) {
                if (num * best_den[0] > best_num[0] * Syy) {
                    best_num[1] = best_num[0];
                    best_den[1] = best_den[0];
                    best_pitch[1] = best_pitch[0];
                    best_num[0] = num;
                    best_den[0] = Syy;
                    best_pitch[0] = i;
                } else {
                    best_num[1] = num;
                    best_den[1] = Syy;
                    best_pitch[1] = i;
                }
            }
        }
        Syy += y[i + len] * y[i + len] - y[i] * y[i];
        if (Syy < 1) Syy = 1;
    }
}

#define MAX_PITCH_LAG 1024

/* Open-loop pitch search on the decimated signal: coarse at 4x decimation, refined at 2x. */
void sscenc_pitch_search(const float *x_lp, const float *y, int len, int max_pitch, int *pitch) {
    float x_lp4[SSCE_N / 4 + 1], y_lp4[(SSCE_N + MAX_PITCH_LAG) / 4 + 1], xcorr[MAX_PITCH_LAG / 2];
    int i, j, lag = len + max_pitch, best_pitch[2] = {0, 0}, offset;
    for (j = 0; j < len >> 2; j++) x_lp4[j] = x_lp[2 * j];
    for (j = 0; j < lag >> 2; j++) y_lp4[j] = y[2 * j];
    for (i = 0; i < max_pitch >> 2; i++) {
        float s = 0;
        for (j = 0; j < len >> 2; j++) s += x_lp4[j] * y_lp4[i + j];
        xcorr[i] = s;
    }
    find_best_pitch(xcorr, y_lp4, len >> 2, max_pitch >> 2, best_pitch);
    for (i = 0; i < max_pitch >> 1; i++) {
        float s = 0;
        xcorr[i] = 0;
        if (abs(i - 2 * best_pitch[0]) > 2 && abs(i - 2 * best_pitch[1]) > 2) continue;
        for (j = 0; j < len >> 1; j++) s += x_lp[j] * y[i + j];
        xcorr[i] = s > -1 ? s : -1;
    }
    find_best_pitch(xcorr, y, len >> 1, max_pitch >> 1, best_pitch);
    offset = 0;
    if (best_pitch[0] > 0 && best_pitch[0] < (max_pitch >> 1) - 1) {
        float a = xcorr[best_pitch[0] - 1], b = xcorr[best_pitch[0]], c = xcorr[best_pitch[0] + 1];
        if ((c - a) > .7f * (b - a)) offset = 1;
        else if ((a - c) > .7f * (b - c)) offset = -1;
    }
    *pitch = 2 * best_pitch[0] - offset;
}

/* Checks the sub-multiples of the period (octave errors) and returns the pitch gain. x: decimated signal,
 * maxperiod/2 history samples followed by N/2 samples. */
float sscenc_remove_doubling(const float *x, int maxperiod, int minperiod, int N, int *T0_, int prev_period,
                             float prev_gain) {
    static const int second_check[16] = {0, 0, 3, 2, 3, 2, 5, 2, 3, 2, 3, 2, 5, 2, 3, 2};
    float yy_lookup[MAX_PITCH_LAG / 2 + 1], xcorr[3];
    float xx = 0, xy = 0, yy, xy2, g, g0, pg, best_xy, best_yy;
    int k, i, T, T0, offset, minperiod0 = minperiod;
    maxperiod /= 2;
    minperiod /= 2;
    *T0_ /= 2;
    prev_period /= 2;
    N /= 2;
    x += maxperiod;
    if (*T0_ >= maxperiod) *T0_ = maxperiod - 1;
    T = T0 = *T0_;
    for (i = 0; i < N; i++) {
        xx += x[i] * x[i];
        xy += x[i] * x[i - T0];
    }
    yy_lookup[0] = xx;
    yy = xx;
    for (i = 1; i <= maxperiod; i++) {
        yy = yy + x[-i] * x[-i] - x[N - i] * x[N - i];
        yy_lookup[i] = yy > 0 ? yy : 0;
    }
    yy = yy_lookup[T0];
    best_xy = xy;
    best_yy = yy;
    g = g0 = xy / sqrtf(1 + xx * yy);
    for (k = 2; k <= 15; k++) {
        int T1, T1b;
        float g1, cont, thresh;
        T1 = (2 * T0 + k) / (2 * k);
        if (T1 < minperiod) break;
        if (k == 2) T1b = T1 + T0 > maxperiod ? T0 : T0 + T1;
        else T1b = (2 * second_check[k] * T0 + k) / (2 * k);
        xy = 0;
        xy2 = 0;
        for (i = 0; i < N; i++) {
            xy += x[i] * x[i - T1];
            xy2 += x[i] * x[i - T1b];
        }
        xy += xy2;
        yy = yy_lookup[T1] + yy_lookup[T1b];
        g1 = xy / sqrtf(1 + 2.f * xx * yy);
        if (abs(T1 - prev_period) <= 1) cont = prev_gain;
        else if (abs(T1 - prev_period) <= 2 && 5 * k * k < T0) cont = .5f * prev_gain;
        else cont = 0;
        thresh = .7f * g0 - cont;
        if (thresh < .3f) thresh = .3f;
        if (T1 < 3 * minperiod) {
            thresh = .85f * g0 - cont;
            if (thresh < .4f) thresh = .4f;
        } else if (T1 < 2 * minperiod) {
            thresh = .9f * g0 - cont;
            if (thresh < .5f) thresh = .5f;
        }
        if (g1 > thresh) {
            best_xy = xy;
            best_yy = yy;
            T = T1;
            g = g1;
        }
    }
    if (best_xy < 0) best_xy = 0;
    pg = best_yy <= best_xy ? 1.f : best_xy / (best_yy + 1);
    for (k = 0; k < 3; k++) {
        int T1 = T + k - 1;
        float s = 0;
        for (i = 0; i < N; i++) s += x[i] * x[i - T1];
        xcorr[k] = s;
    }
    if ((xcorr[2] - xcorr[0]) > .7f * (xcorr[1] - xcorr[0])) offset = 1;
    else if ((xcorr[0] - xcorr[2]) > .7f * (xcorr[1] - xcorr[2])) offset = -1;
    else offset = 0;
    if (pg > g) pg = g;
    *T0_ = 2 * T + offset;
    if (*T0_ < minperiod0) *T0_ = minperiod0;
    return pg;
}

/* y[i] = x[i] + g * (3-tap comb at period T), crossfading from (T0, g0) to (T1, g1) over the first `overlap`
 * samples with the squared window. x needs T + 2 samples of history. y and x must not overlap. The encoder
 * calls it with negative gains (FIR prefilter); the decoder runs the IIR inverse with positive gains. */
void sscenc_comb_filter(float *y, const float *x, int T0, int T1, int N, float g0, float g1, int overlap) {
    float g00 = g0 * comb_gains[0], g01 = g0 * comb_gains[1], g02 = g0 * comb_gains[2];
    float g10 = g1 * comb_gains[0], g11 = g1 * comb_gains[1], g12 = g1 * comb_gains[2];
    float x0, x1, x2, x3, x4;
    int i;
    if (g0 == 0 && g1 == 0) {
        memmove(y, x, sizeof(float) * N);
        return;
    }
    x1 = x[-T1 + 1];
    x2 = x[-T1];
    x3 = x[-T1 - 1];
    x4 = x[-T1 - 2];
    for (i = 0; i < overlap; i++) {
        float f = sscenc_window[i] * sscenc_window[i];
        x0 = x[i - T1 + 2];
        y[i] = x[i] + (1.f - f) * g00 * x[i - T0] + (1.f - f) * g01 * (x[i - T0 + 1] + x[i - T0 - 1]) +
               (1.f - f) * g02 * (x[i - T0 + 2] + x[i - T0 - 2]) + f * g10 * x2 + f * g11 * (x1 + x3) +
               f * g12 * (x0 + x4);
        x4 = x3;
        x3 = x2;
        x2 = x1;
        x1 = x0;
    }
    for (; i < N; i++) {
        x0 = x[i - T1 + 2];
        y[i] = x[i] + g10 * x2 + g11 * (x1 + x3) + g12 * (x0 + x4);
        x4 = x3;
        x3 = x2;
        x2 = x1;
        x1 = x0;
    }
}
