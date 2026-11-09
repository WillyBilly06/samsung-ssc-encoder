/* SSC encoder: signal analysis (transients, TF resolution, spreading, dynamic allocation, trim, stereo).
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 * Adapted from libopus 1.1 celt/celt_encoder.c and celt/bands.c, float build
 * (Copyright 2007-2010 Xiph.Org Foundation, 2008 Gregory Maxwell, 2007-2008 CSIRO, BSD-3-Clause, see LICENSE).
 *
 * These are encoder decisions only: none of them has to match anything, they just pick legal values for the
 * symbols the bitstream carries. Energies here are float log2 amplitudes relative to eMeans. */
#include <math.h>
#include <string.h>

#include "ssc_enc_internal.h"

const signed char sscenc_tf_select_table[8] = {0, -2, 0, -3, 3, 0, 1, -1}; /* CELT LM = 3 row */

/* Time-domain transient detector. in: two channels, `len` samples each (channel c at in + c*len). */
int sscenc_transient_analysis(const float *in, int len, float *tf_estimate, int *tf_chan) {
    static const unsigned char inv_table[128] = {
        255, 255, 156, 110, 86, 70, 59, 51, 45, 40, 37, 33, 31, 28, 26, 25, 23, 22, 21, 20, 19, 18,
        17,  16,  16,  15,  15, 14, 13, 13, 12, 12, 12, 12, 11, 11, 11, 10, 10, 10, 9,  9,  9,  9,
        9,   9,   8,   8,   8,  8,  8,  7,  7,  7,  7,  7,  7,  6,  6,  6,  6,  6,  6,  6,  6,  6,
        6,   6,   6,   6,   6,  6,  6,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  4,  4,  4,
        4,   4,   4,   4,   4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
        3,   3,   3,   3,   3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  2,
    };
    float tmp[SSCE_N + SSCE_OVERLAP];
    int i, c, len2 = len / 2, mask_metric = 0;
    float tf_max;
    *tf_chan = 0;
    for (c = 0; c < SSCE_C; c++) {
        float mean = 0, mem0 = 0, mem1 = 0, maxE = 0, norm;
        int unmask = 0;
        const float *x = in + c * len;
        /* high-pass filter (1 - 2z^-1 + z^-2) / (1 - z^-1 + .5 z^-2) */
        for (i = 0; i < len; i++) {
            float y = mem0 + x[i];
            mem0 = mem1 + y - 2 * x[i];
            mem1 = x[i] - .5f * y;
            tmp[i] = y;
        }
        for (i = 0; i < 12; i++) tmp[i] = 0;
        mem0 = 0;
        /* forward pass: post-echo threshold */
        for (i = 0; i < len2; i++) {
            float x2 = tmp[2 * i] * tmp[2 * i] + tmp[2 * i + 1] * tmp[2 * i + 1];
            mean += x2;
            tmp[i] = mem0 + .0625f * (x2 - mem0);
            mem0 = tmp[i];
        }
        mem0 = 0;
        /* backward pass: pre-echo threshold */
        for (i = len2 - 1; i >= 0; i--) {
            tmp[i] = mem0 + .125f * (tmp[i] - mem0);
            mem0 = tmp[i];
            if (mem0 > maxE) maxE = mem0;
        }
        mean = sqrtf(mean * maxE * .5f * len2);
        norm = (float)len2 / (1e-15f + mean);
        for (i = 12; i < len2 - 5; i += 4) {
            int id = (int)floorf(64 * norm * tmp[i]);
            if (id < 0) id = 0;
            if (id > 127) id = 127;
            unmask += inv_table[id];
        }
        unmask = 64 * unmask * 4 / (6 * (len2 - 17));
        if (unmask > mask_metric) {
            *tf_chan = c;
            mask_metric = unmask;
        }
    }
    tf_max = sqrtf(27.f * mask_metric) - 42.f;
    if (tf_max < 0) tf_max = 0;
    {
        float t = .0069f * (tf_max < 163.f ? tf_max : 163.f) - .139f;
        *tf_estimate = sqrtf(t > 0 ? t : 0);
    }
    return mask_metric > 200;
}

/* Sudden energy increase that the time-domain detector missed. newE/oldE: [c*22+i] float log2. */
int sscenc_patch_transient_decision(const float *newE, const float *oldE, int end) {
    int i, c;
    float mean_diff = 0, spread_old[SSCE_NB];
    spread_old[0] = oldE[0] > oldE[SSCE_NB] ? oldE[0] : oldE[SSCE_NB];
    for (i = 1; i < end; i++) {
        float a = spread_old[i - 1] - 1.f, b = oldE[i] > oldE[i + SSCE_NB] ? oldE[i] : oldE[i + SSCE_NB];
        spread_old[i] = a > b ? a : b;
    }
    for (i = end - 2; i >= 0; i--)
        if (spread_old[i + 1] - 1.f > spread_old[i]) spread_old[i] = spread_old[i + 1] - 1.f;
    for (c = 0; c < SSCE_C; c++)
        for (i = 2; i < end - 1; i++) {
            float x1 = newE[i + c * SSCE_NB] > 0 ? newE[i + c * SSCE_NB] : 0;
            float x2 = spread_old[i] > 0 ? spread_old[i] : 0;
            if (x1 > x2) mean_diff += x1 - x2;
        }
    mean_diff /= SSCE_C * (end - 3);
    return mean_diff > 1.f;
}

static float l1_metric(const float *tmp, int N, int LM, float bias) {
    int i;
    float L1 = 0;
    for (i = 0; i < N; i++) L1 += fabsf(tmp[i]);
    return L1 + LM * bias * L1;
}

/* Chooses the per-band time-frequency resolution (Viterbi over a L1-norm sparsity metric). */
int sscenc_tf_analysis(const sscenc_mode *m, int len, int isTransient, int *tf_res, int lambda, const float *X,
                       float tf_estimate, int tf_chan) {
    const int LM = SSCE_LM;
    int metric[SSCE_NB], path0[SSCE_NB], path1[SSCE_NB], selcost[2];
    float tmp[SSCE_MAX_BAND_N], tmp_1[SSCE_MAX_BAND_N];
    int i, sel, tf_select = 0, cost0, cost1;
    float bias = .04f * (.5f - tf_estimate > -.25f ? .5f - tf_estimate : -.25f);

    for (i = 0; i < len; i++) {
        int j, k, N = (m->eBands[i + 1] - m->eBands[i]) << LM, best_level = 0;
        int narrow = (m->eBands[i + 1] - m->eBands[i]) == 1;
        float L1, best_L1;
        for (j = 0; j < N; j++) tmp[j] = X[tf_chan * SSCE_N + j + (m->eBands[i] << LM)];
        L1 = l1_metric(tmp, N, isTransient ? LM : 0, bias);
        best_L1 = L1;
        if (isTransient && !narrow) {
            memcpy(tmp_1, tmp, sizeof(float) * N);
            sscenc_haar1(tmp_1, N >> LM, 1 << LM);
            L1 = l1_metric(tmp_1, N, LM + 1, bias);
            if (L1 < best_L1) {
                best_L1 = L1;
                best_level = -1;
            }
        }
        for (k = 0; k < LM + !(isTransient || narrow); k++) {
            int B = isTransient ? (LM - k - 1) : k + 1;
            sscenc_haar1(tmp, N >> k, 1 << k);
            L1 = l1_metric(tmp, N, B, bias);
            if (L1 < best_L1) {
                best_L1 = L1;
                best_level = k + 1;
            }
        }
        metric[i] = isTransient ? 2 * best_level : -2 * best_level;
        if (narrow && (metric[i] == 0 || metric[i] == -2 * LM)) metric[i] -= 1;
    }
    for (sel = 0; sel < 2; sel++) {
        cost0 = 0;
        cost1 = isTransient ? 0 : lambda;
        for (i = 1; i < len; i++) {
            int curr0 = sscenc_imin(cost0, cost1 + lambda), curr1 = sscenc_imin(cost0 + lambda, cost1);
            int d0 = metric[i] - 2 * sscenc_tf_select_table[4 * isTransient + 2 * sel + 0];
            int d1 = metric[i] - 2 * sscenc_tf_select_table[4 * isTransient + 2 * sel + 1];
            cost0 = curr0 + (d0 < 0 ? -d0 : d0);
            cost1 = curr1 + (d1 < 0 ? -d1 : d1);
        }
        selcost[sel] = sscenc_imin(cost0, cost1);
    }
    if (selcost[1] < selcost[0] && isTransient) tf_select = 1;
    cost0 = 0;
    cost1 = isTransient ? 0 : lambda;
    for (i = 1; i < len; i++) {
        int from0 = cost0, from1 = cost1 + lambda, curr0, curr1, d0, d1;
        if (from0 < from1) { curr0 = from0; path0[i] = 0; }
        else { curr0 = from1; path0[i] = 1; }
        from0 = cost0 + lambda;
        from1 = cost1;
        if (from0 < from1) { curr1 = from0; path1[i] = 0; }
        else { curr1 = from1; path1[i] = 1; }
        d0 = metric[i] - 2 * sscenc_tf_select_table[4 * isTransient + 2 * tf_select + 0];
        d1 = metric[i] - 2 * sscenc_tf_select_table[4 * isTransient + 2 * tf_select + 1];
        cost0 = curr0 + (d0 < 0 ? -d0 : d0);
        cost1 = curr1 + (d1 < 0 ? -d1 : d1);
    }
    tf_res[len - 1] = cost0 < cost1 ? 0 : 1;
    for (i = len - 2; i >= 0; i--) tf_res[i] = tf_res[i + 1] == 1 ? path1[i + 1] : path0[i + 1];
    return tf_select;
}

/* Spreading (rotation) decision from the peakiness of the normalised spectrum. */
int sscenc_spreading_decision(const sscenc_mode *m, const float *X, int *average, int last_decision, int end) {
    const short *eBands = m->eBands;
    int i, c, sum = 0, nbBands = 0, decision;
    if (SSCE_M * (eBands[end] - eBands[end - 1]) <= 8) return SSCE_SPREAD_NONE;
    for (c = 0; c < SSCE_C; c++) {
        for (i = 0; i < end; i++) {
            int j, N = SSCE_M * (eBands[i + 1] - eBands[i]), tmp, tcount[3] = {0, 0, 0};
            const float *x = X + SSCE_M * eBands[i] + c * SSCE_N;
            if (N <= 8) continue;
            for (j = 0; j < N; j++) {
                float x2N = x[j] * x[j] * N;
                if (x2N < 0.25f) tcount[0]++;
                if (x2N < 0.0625f) tcount[1]++;
                if (x2N < 0.015625f) tcount[2]++;
            }
            tmp = (2 * tcount[2] >= N) + (2 * tcount[1] >= N) + (2 * tcount[0] >= N);
            sum += tmp * 256;
            nbBands++;
        }
    }
    sum /= nbBands;
    sum = (sum + *average) >> 1;
    *average = sum;
    sum = (3 * sum + (((3 - last_decision) << 7) + 64) + 2) >> 2;
    if (sum < 80) decision = SSCE_SPREAD_AGGRESSIVE;
    else if (sum < 256) decision = SSCE_SPREAD_NORMAL;
    else if (sum < 384) decision = SSCE_SPREAD_LIGHT;
    else decision = SSCE_SPREAD_NONE;
    return decision;
}

/* Dynamic allocation: boosts bands that stick out of a smoothed spectral envelope. offsets[] receives the
 * number of boost quanta wanted per band (the caller codes them). CBR rules: non-transient frames get half
 * the boost, and the total is limited to a quarter of the frame. */
void sscenc_dynalloc_analysis(const sscenc_mode *m, const float *bandLogE, const float *bandLogE2, int end,
                              int *offsets, int lsb_depth, int isTransient, int effectiveBytes) {
    const short *eBands = m->eBands;
    float follower[SSCE_C * SSCE_NB], noise_floor[SSCE_NB];
    int32_t tot_boost = 0;
    int i, c;
    for (i = 0; i < SSCE_NB; i++) offsets[i] = 0;
    if (effectiveBytes <= 50) return;
    for (i = 0; i < end; i++)
        noise_floor[i] = .0625f * m->logN[i] + .5f + (9 - lsb_depth) - sscenc_eMeans[i] + .0062f * (i + 5) * (i + 5);
    for (c = 0; c < SSCE_C; c++) {
        const float *E2 = bandLogE2 + c * SSCE_NB;
        float *f = follower + c * SSCE_NB;
        int last = 0;
        f[0] = E2[0];
        for (i = 1; i < end; i++) {
            if (E2[i] > E2[i - 1] + .5f) last = i;
            f[i] = f[i - 1] + 1.5f < E2[i] ? f[i - 1] + 1.5f : E2[i];
        }
        for (i = last - 1; i >= 0; i--) {
            float a = f[i + 1] + 2.f < E2[i] ? f[i + 1] + 2.f : E2[i];
            if (a < f[i]) f[i] = a;
        }
        for (i = 0; i < end; i++)
            if (f[i] < noise_floor[i]) f[i] = noise_floor[i];
    }
    for (i = 0; i < end; i++) {
        float *f0 = follower + i, *f1 = follower + SSCE_NB + i, d0, d1;
        if (*f0 - 4.f > *f1) *f1 = *f0 - 4.f;
        if (*f1 - 4.f > *f0) *f0 = *f1 - 4.f;
        d0 = bandLogE[i] - *f0;
        d1 = bandLogE[SSCE_NB + i] - *f1;
        follower[i] = .5f * ((d0 > 0 ? d0 : 0) + (d1 > 0 ? d1 : 0));
    }
    if (!isTransient)
        for (i = 0; i < end; i++) follower[i] *= .5f;
    for (i = 0; i < end; i++) {
        int width = SSCE_C * (eBands[i + 1] - eBands[i]) << SSCE_LM, boost, boost_bits;
        int quanta = sscenc_imin(width << SSCE_BITRES, sscenc_imax(6 << SSCE_BITRES, width));
        if (i < 8) follower[i] *= 2;
        if (i >= 12) follower[i] *= .5f;
        if (follower[i] > 4.f) follower[i] = 4.f;
        if (width > 48) {
            boost = (int)(follower[i] * 8);
            boost_bits = (boost * width << SSCE_BITRES) / 8;
        } else {
            boost = (int)(follower[i] * width / 6);
            boost_bits = boost * 6 << SSCE_BITRES;
        }
        /* boost is in units of the band's coding quantum */
        boost = boost_bits / quanta;
        boost_bits = boost * quanta;
        if ((tot_boost + boost_bits) >> SSCE_BITRES >> 3 > effectiveBytes / 4) {
            int32_t cap = (int32_t)(effectiveBytes / 4) << SSCE_BITRES << 3;
            offsets[i] = (int)((cap - tot_boost) / quanta);
            break;
        }
        offsets[i] = boost;
        tot_boost += boost_bits;
    }
}

/* Allocation trim: spectral tilt, inter-channel correlation and transient-ness. */
int sscenc_alloc_trim_analysis(const sscenc_mode *m, const float *X, const float *bandLogE, int end,
                               float *stereo_saving, float tf_estimate, int intensity) {
    const int LM = SSCE_LM;
    int i, c, trim_index;
    float diff = 0, trim = 5.f, sum = 0, minXC, logXC, logXC2;
    for (i = 0; i < 8; i++) {
        int j;
        float partial = 0;
        for (j = m->eBands[i] << LM; j < m->eBands[i + 1] << LM; j++) partial += X[j] * X[SSCE_N + j];
        sum += partial;
    }
    sum *= 1.f / 8;
    sum = fabsf(sum) < 1.f ? fabsf(sum) : 1.f;
    minXC = sum;
    for (i = 8; i < intensity; i++) {
        int j;
        float partial = 0;
        for (j = m->eBands[i] << LM; j < m->eBands[i + 1] << LM; j++) partial += X[j] * X[SSCE_N + j];
        if (fabsf(partial) < minXC) minXC = fabsf(partial);
    }
    if (minXC > 1.f) minXC = 1.f;
    logXC = log2f(1.001f - sum * sum);
    logXC2 = log2f(1.001f - minXC * minXC);
    if (.5f * logXC > logXC2) logXC2 = .5f * logXC;
    trim += (.75f * logXC > -4.f) ? .75f * logXC : -4.f;
    if (*stereo_saving + .25f < -.5f * logXC2) *stereo_saving += .25f;
    else *stereo_saving = -.5f * logXC2;

    for (c = 0; c < SSCE_C; c++)
        for (i = 0; i < end - 1; i++) diff += bandLogE[i + c * SSCE_NB] * (float)(2 + 2 * i - end);
    diff /= SSCE_C * (end - 1);
    {
        float t = (diff + 1.f) / 6;
        if (t > 2.f) t = 2.f;
        if (t < -2.f) t = -2.f;
        trim -= t;
    }
    trim -= 2 * tf_estimate;
    trim_index = (int)floorf(.5f + trim);
    if (trim_index < 0) trim_index = 0;
    if (trim_index > 10) trim_index = 10;
    return trim_index;
}

/* Dual (L/R) stereo vs mid/side, from an L1 entropy estimate over the 13 lowest bands. */
int sscenc_stereo_analysis(const sscenc_mode *m, const float *X) {
    const int LM = SSCE_LM;
    int i, j;
    float sumLR = 1e-15f, sumMS = 1e-15f;
    for (i = 0; i < 13; i++)
        for (j = m->eBands[i] << LM; j < m->eBands[i + 1] << LM; j++) {
            float L = X[j], R = X[SSCE_N + j], Mi = L + R, S = L - R;
            sumLR += fabsf(L) + fabsf(R);
            sumMS += fabsf(Mi) + fabsf(S);
        }
    sumMS *= 0.707107f;
    return ((m->eBands[13] << (LM + 1)) + 13) * sumMS > (m->eBands[13] << (LM + 1)) * sumLR;
}
