/* SSC encoder: forward MDCT (one 1728-point block, or eight 216-point blocks).
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 *
 * Definition (N2 = number of coefficients, 864 long or 108 short; the block has 2*N2 samples):
 *     X[k] = 2/N2 * sum_{n=0}^{2*N2-1} w[n] x[n] cos(pi/N2 * (n + 1/2 + N2/2) * (k + 1/2))
 * which is the exact inverse (with overlap-add) of the SSC synthesis. The long block covers the 972 input
 * samples [108 previous | 864 new] in the middle of the 1728-sample window: 378 zeros, the rising half of the
 * 108-sample overlap window, ones, the falling half, 378 zeros. Short block b covers input samples
 * [108*b, 108*b + 216) with the rising and falling overlap halves.
 *
 * The MDCT is folded into an N2-point DCT-IV, computed with an N2/2-point complex FFT
 * (432 = 4*4*3*3*3, 54 = 2*3*3*3). */
#include <math.h>
#include <string.h>

#include "ssc_enc_internal.h"

#define PI_D 3.14159265358979323846

static const int factors_432[] = {4, 108, 4, 27, 3, 9, 3, 3, 3, 1}; /* (radix, remaining length) */
static const int factors_54[] = {2, 27, 3, 9, 3, 3, 3, 1};

static void fill_tw(sscenc_cpx *tw, int n) {
    int k;
    for (k = 0; k < n; k++) {
        tw[k].r = (float)cos(-2.0 * PI_D * k / n);
        tw[k].i = (float)sin(-2.0 * PI_D * k / n);
    }
}

/* DCT-IV of size N2 via an M = N2/2 point FFT:
 *   z[n] = (v[2n] + i v[N2-1-2n]) * exp(-i pi n / N2)
 *   S[k] = FFT_M(z)[k] * exp(-i pi (k + 1/4) / N2)
 *   u[2k] = Re S[k],  u[N2-1-2k] = -Im S[k]
 * The pre/post twiddles also carry the 2/N2 MDCT scale. */
static void fill_prepost(sscenc_cpx *pre, sscenc_cpx *post, int n2) {
    int k, m = n2 / 2;
    double scale = 2.0 / n2;
    for (k = 0; k < m; k++) {
        pre[k].r = (float)cos(-PI_D * k / n2);
        pre[k].i = (float)sin(-PI_D * k / n2);
        post[k].r = (float)(scale * cos(-PI_D * (k + 0.25) / n2));
        post[k].i = (float)(scale * sin(-PI_D * (k + 0.25) / n2));
    }
}

void sscenc_mdct_init(sscenc_mdct *m) {
    int i;
    fill_tw(m->tw_long, 432);
    fill_tw(m->tw_short, 54);
    fill_prepost(m->pre_long, m->post_long, SSCE_N);
    fill_prepost(m->pre_short, m->post_short, SSCE_SHORT);
    /* long window: 378 zeros, rise, ones, fall, 378 zeros */
    for (i = 0; i < 2 * SSCE_N; i++) {
        int n = i - 378;
        float w;
        if (n < 0 || n >= SSCE_N + SSCE_OVERLAP) w = 0.f;
        else if (n < SSCE_OVERLAP) w = sscenc_window[n];
        else if (n < SSCE_N) w = 1.f;
        else w = sscenc_window[SSCE_N + SSCE_OVERLAP - 1 - n];
        m->win_long[i] = w;
    }
    for (i = 0; i < SSCE_OVERLAP; i++) {
        m->win_short[i] = sscenc_window[i];
        m->win_short[SSCE_OVERLAP + i] = sscenc_window[SSCE_OVERLAP - 1 - i];
    }
}

#define CMUL(o, a, b)                                                                                      \
    do {                                                                                                   \
        float ar_ = (a).r, ai_ = (a).i;                                                                    \
        (o).r = ar_ * (b).r - ai_ * (b).i;                                                                 \
        (o).i = ar_ * (b).i + ai_ * (b).r;                                                                 \
    } while (0)

/* Recursive decimation-in-time mixed-radix DFT: out[k] = sum_n in[n*in_stride] exp(-2 pi i n k / N). */
static void fft_rec(sscenc_cpx *out, const sscenc_cpx *in, int fstride, const int *factors, const sscenc_cpx *tw) {
    const int p = factors[0], m = factors[1];
    int k, q;
    if (m == 1) {
        for (q = 0; q < p; q++) out[q] = in[q * fstride];
    } else {
        for (q = 0; q < p; q++) fft_rec(out + q * m, in + q * fstride, fstride * p, factors + 2, tw);
    }
    if (p == 4) {
        sscenc_cpx *F = out;
        for (k = 0; k < m; k++, F++) {
            sscenc_cpx s0, s1, s2, s3, s4, s5;
            CMUL(s0, F[m], tw[k * fstride]);
            CMUL(s1, F[2 * m], tw[2 * k * fstride]);
            CMUL(s2, F[3 * m], tw[3 * k * fstride]);
            s5.r = F[0].r - s1.r; s5.i = F[0].i - s1.i;
            F[0].r += s1.r; F[0].i += s1.i;
            s3.r = s0.r + s2.r; s3.i = s0.i + s2.i;
            s4.r = s0.r - s2.r; s4.i = s0.i - s2.i;
            F[2 * m].r = F[0].r - s3.r; F[2 * m].i = F[0].i - s3.i;
            F[0].r += s3.r; F[0].i += s3.i;
            F[m].r = s5.r + s4.i; F[m].i = s5.i - s4.r;
            F[3 * m].r = s5.r - s4.i; F[3 * m].i = s5.i + s4.r;
        }
    } else if (p == 3) {
        const float epi3_i = tw[m * fstride].i; /* -sin(2 pi / 3) */
        sscenc_cpx *F = out;
        for (k = 0; k < m; k++, F++) {
            sscenc_cpx s0, s1, s2, s3;
            CMUL(s1, F[m], tw[k * fstride]);
            CMUL(s2, F[2 * m], tw[2 * k * fstride]);
            s3.r = s1.r + s2.r; s3.i = s1.i + s2.i;
            s0.r = (s1.r - s2.r) * epi3_i; s0.i = (s1.i - s2.i) * epi3_i;
            F[m].r = F[0].r - 0.5f * s3.r; F[m].i = F[0].i - 0.5f * s3.i;
            F[0].r += s3.r; F[0].i += s3.i;
            F[2 * m].r = F[m].r + s0.i; F[2 * m].i = F[m].i - s0.r;
            F[m].r -= s0.i; F[m].i += s0.r;
        }
    } else { /* p == 2 */
        sscenc_cpx *F = out;
        for (k = 0; k < m; k++, F++) {
            sscenc_cpx t;
            CMUL(t, F[m], tw[k * fstride]);
            F[m].r = F[0].r - t.r; F[m].i = F[0].i - t.i;
            F[0].r += t.r; F[0].i += t.i;
        }
    }
}

/* One MDCT block. x: 2*n2 samples (unwindowed), win: 2*n2 window. Output with stride. */
static void mdct_block(const sscenc_mdct *m, const float *x, const float *win, int n2, float *out, int stride) {
    sscenc_cpx z[432], Z[432];
    float xw[2 * SSCE_N], v[SSCE_N];
    const int q = n2 / 2, half = n2 / 2;
    const sscenc_cpx *pre = n2 == SSCE_N ? m->pre_long : m->pre_short;
    const sscenc_cpx *post = n2 == SSCE_N ? m->post_long : m->post_short;
    int j, k;
    for (j = 0; j < 2 * n2; j++) xw[j] = x[j] * win[j];
    /* fold the 2*n2 windowed samples into the n2-point DCT-IV input */
    for (j = 0; j < q; j++) v[j] = -xw[3 * q - 1 - j] - xw[j + 3 * q];
    for (j = q; j < n2; j++) v[j] = xw[j - q] - xw[3 * q - 1 - j];
    for (k = 0; k < half; k++) {
        sscenc_cpx t;
        t.r = v[2 * k];
        t.i = v[n2 - 1 - 2 * k];
        CMUL(z[k], t, pre[k]);
    }
    if (n2 == SSCE_N) fft_rec(Z, z, 1, factors_432, m->tw_long);
    else fft_rec(Z, z, 1, factors_54, m->tw_short);
    for (k = 0; k < half; k++) {
        sscenc_cpx s;
        CMUL(s, Z[k], post[k]);
        out[(2 * k) * stride] = s.r;
        out[(n2 - 1 - 2 * k) * stride] = -s.i;
    }
}

void sscenc_mdct_frame(const sscenc_mdct *m, const float *in, int shortBlocks, float *out) {
    if (!shortBlocks) {
        /* Long block: the 972 input samples start at offset 378 of the 1728-point block. Only the windowed
         * part is non-zero, so build the block from in[] with zero padding. */
        float x[2 * SSCE_N];
        memset(x, 0, sizeof x);
        memcpy(x + 378, in, sizeof(float) * (SSCE_N + SSCE_OVERLAP));
        mdct_block(m, x, m->win_long, SSCE_N, out, 1);
    } else {
        int b;
        for (b = 0; b < SSCE_M; b++) mdct_block(m, in + SSCE_SHORT * b, m->win_short, SSCE_SHORT, out + b, SSCE_M);
    }
}
