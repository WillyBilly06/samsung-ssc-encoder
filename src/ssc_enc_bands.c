/* SSC encoder: band shape quantisation (PVQ), stereo and time-frequency splitting.
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 * Adapted from libopus 1.1 celt/bands.c, celt/vq.c and celt/cwrs.c
 * (Copyright 2007-2009 Xiph.Org Foundation, 2008-2009 Gregory Maxwell, 2007-2008 CSIRO,
 *  2007-2009 Timothy B. Terriberry, BSD-3-Clause, see LICENSE).
 *
 * This is the encode half of quant_all_bands. It does not resynthesise: the decoder's folding memory,
 * collapse masks and noise filling never change the bitstream syntax, only the decoded audio, so the encoder
 * does not need them. What it must follow exactly is the integer bit bookkeeping (b, balance, qalloc,
 * remaining_bits, pulse counts), which is a mirror of the decoder. Bands are always >= 8 bins wide
 * (LM = 3), so CELT's N == 1 and N == 2 special cases cannot occur. */
#include <math.h>
#include <string.h>

#include "ssc_enc_internal.h"

#define QTHETA_OFFSET   4
#define LOG_MAX_PSEUDO  6
#define NFREQ           SSCE_N

/* cos(pi/2 * x) for x in [0, 1]: even polynomial, max error 2e-5 (the decoder uses the same one, so the spreading
 * rotation it undoes is exactly the one applied here). */
float sscenc_cos_half_pi(float x) {
    float x2 = x * x;
    return 1.f + x2 * (-1.2336979f + x2 * (0.25366235f + x2 * (-0.020863481f + x2 * 0.00091892557f)));
}

/* ---- bit-exact integer helpers (they drive the allocation inside a band) ---- */
static int frac_mul16(int a, int b) { return (16384 + (int32_t)(int16_t)a * (int16_t)b) >> 15; }

static int bitexact_cos(int x) {
    int32_t tmp = (4096 + (int32_t)x * x) >> 13;
    int x2 = (int)tmp;
    x2 = (32767 - x2) + frac_mul16(x2, (-7651 + frac_mul16(x2, (8277 + frac_mul16(-626, x2)))));
    return 1 + x2;
}

static int bitexact_log2tan(int isin, int icos) {
    int lc = sscenc_ilog((uint32_t)icos), ls = sscenc_ilog((uint32_t)isin);
    icos <<= 15 - lc;
    isin <<= 15 - ls;
    return (ls - lc) * (1 << 11) + frac_mul16(isin, frac_mul16(isin, -2597) + 7932) -
           frac_mul16(icos, frac_mul16(icos, -2597) + 7932);
}

static int compute_qn(int N, int b, int offset, int pulse_cap) {
    static const int16_t exp2_table8[8] = {16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048};
    int qn, qb, N2 = 2 * N - 1;
    qb = sscenc_imin(b - pulse_cap - (4 << SSCE_BITRES), (b + N2 * offset) / N2);
    qb = sscenc_imin(8 << SSCE_BITRES, qb);
    if (qb < (1 << SSCE_BITRES >> 1)) {
        qn = 1;
    } else {
        qn = exp2_table8[qb & 0x7] >> (14 - (qb >> SSCE_BITRES));
        qn = (qn + 1) >> 1 << 1;
    }
    return qn;
}

static int get_pulses(int i) { return i < 8 ? i : (8 + (i & 7)) << ((i >> 3) - 1); }

static const unsigned char *band_cache(const sscenc_mode *m, int band, int LM) {
    return m->cache_bits + m->cache_index[(LM + 1) * SSCE_NB + band];
}

static int bits2pulses(const sscenc_mode *m, int band, int LM, int bits) {
    const unsigned char *cache = band_cache(m, band, LM);
    int i, lo = 0, hi = cache[0];
    bits--;
    for (i = 0; i < LOG_MAX_PSEUDO; i++) {
        int mid = (lo + hi + 1) >> 1;
        if ((int)cache[mid] >= bits) hi = mid;
        else lo = mid;
    }
    if (bits - (lo == 0 ? -1 : (int)cache[lo]) <= (int)cache[hi] - bits) return lo;
    return hi;
}

static int pulses2bits(const sscenc_mode *m, int band, int LM, int pulses) {
    const unsigned char *cache = band_cache(m, band, LM);
    return pulses == 0 ? 0 : cache[pulses] + 1;
}

/* ---- PVQ codeword index (cwrs.c, table form) ---- */
#define PVQ_U(n, k) (sscenc_pvq_u_row[sscenc_imin(n, k)][sscenc_imax(n, k)])
#define PVQ_V(n, k) (PVQ_U(n, k) + PVQ_U(n, (k) + 1))

static uint32_t icwrs(int n, const int *y) {
    uint32_t i;
    int j = n - 1, k;
    i = y[j] < 0;
    k = y[j] < 0 ? -y[j] : y[j];
    do {
        j--;
        i += PVQ_U(n - j, k);
        k += y[j] < 0 ? -y[j] : y[j];
        if (y[j] < 0) i += PVQ_U(n - j, k + 1);
    } while (j > 0);
    return i;
}

static void encode_pulses(const int *y, int n, int k, sscenc_ec *ec) {
    sscenc_ec_uint(ec, icwrs(n, y), PVQ_V(n, k));
}

/* ---- vq.c ---- */
static void exp_rotation1(float *X, int len, int stride, float c, float s) {
    int i;
    for (i = 0; i < len - stride; i++) {
        float x1 = X[i], x2 = X[i + stride];
        X[i + stride] = c * x2 + s * x1;
        X[i] = c * x1 - s * x2;
    }
    for (i = len - 2 * stride - 1; i >= 0; i--) {
        float x1 = X[i], x2 = X[i + stride];
        X[i + stride] = c * x2 + s * x1;
        X[i] = c * x1 - s * x2;
    }
}

static void exp_rotation(float *X, int len, int dir, int stride, int K, int spread) {
    static const int SPREAD_FACTOR[3] = {15, 10, 5};
    int i, stride2 = 0, factor;
    float gain, theta, c, s;
    if (2 * K >= len || spread == SSCE_SPREAD_NONE) return;
    factor = SPREAD_FACTOR[spread - 1];
    gain = (float)len / (float)(len + factor * K);
    theta = 0.5f * gain * gain;
    c = sscenc_cos_half_pi(theta);
    s = sscenc_cos_half_pi(1.0f - theta);
    if (len >= 8 * stride) {
        stride2 = 1;
        while ((stride2 * stride2 + stride2) * stride + (stride >> 2) < len) stride2++;
    }
    len /= stride;
    for (i = 0; i < stride; i++) {
        if (dir < 0) {
            if (stride2) exp_rotation1(X + i * len, len, stride2, s, c);
            exp_rotation1(X + i * len, len, 1, c, s);
        } else {
            exp_rotation1(X + i * len, len, 1, c, -s);
            if (stride2) exp_rotation1(X + i * len, len, stride2, s, -c);
        }
    }
}

/* Pulse search: finds the integer vector y with sum|y| = K that maximises <x,y>/|y|. */
static void alg_quant(float *X, int N, int K, int spread, int B, sscenc_ec *ec) {
    float y[SSCE_MAX_BAND_N], absx[SSCE_MAX_BAND_N];
    int iy[SSCE_MAX_BAND_N], j, i, pulsesLeft;
    float sum = 0, xy = 0, yy = 0;

    exp_rotation(X, N, 1, B, K, spread);
    for (j = 0; j < N; j++) {
        absx[j] = fabsf(X[j]);
        iy[j] = 0;
        y[j] = 0;
    }
    pulsesLeft = K;
    /* Pre-search by projecting on the pyramid */
    if (K > (N >> 1)) {
        float rcp;
        for (j = 0; j < N; j++) sum += absx[j];
        if (!(sum > 1e-15f && sum < 64.f)) {
            absx[0] = 1.f;
            for (j = 1; j < N; j++) absx[j] = 0;
            sum = 1.f;
        }
        rcp = (float)(K - 1) / sum;
        for (j = 0; j < N; j++) {
            iy[j] = (int)floorf(rcp * absx[j]);
            y[j] = (float)iy[j];
            yy += y[j] * y[j];
            xy += absx[j] * y[j];
            y[j] *= 2;
            pulsesLeft -= iy[j];
        }
    }
    if (pulsesLeft > N + 3) {
        float tmp = (float)pulsesLeft;
        yy += tmp * tmp + tmp * y[0];
        iy[0] += pulsesLeft;
        pulsesLeft = 0;
    }
    for (i = 0; i < pulsesLeft; i++) {
        int best_id = 0;
        float best_num = -1e10f, best_den = 0;
        yy += 1;
        for (j = 0; j < N; j++) {
            float Rxy = xy + absx[j], Ryy = yy + y[j];
            Rxy = Rxy * Rxy;
            if (best_den * Rxy > Ryy * best_num) {
                best_den = Ryy;
                best_num = Rxy;
                best_id = j;
            }
        }
        xy += absx[best_id];
        yy += y[best_id];
        y[best_id] += 2;
        iy[best_id]++;
    }
    for (j = 0; j < N; j++)
        if (X[j] < 0) iy[j] = -iy[j];
    encode_pulses(iy, N, K, ec);
}

static int stereo_itheta(const float *X, const float *Y, int stereo, int N) {
    int i;
    float Emid = 1e-15f, Eside = 1e-15f;
    if (stereo) {
        for (i = 0; i < N; i++) {
            float m = 0.5f * (X[i] + Y[i]), s = 0.5f * (X[i] - Y[i]);
            Emid += m * m;
            Eside += s * s;
        }
    } else {
        for (i = 0; i < N; i++) {
            Emid += X[i] * X[i];
            Eside += Y[i] * Y[i];
        }
    }
    return (int)floor(.5f + 16384 * 0.63662f * atan2(sqrtf(Eside), sqrtf(Emid)));
}

/* ---- bands.c helpers ---- */
void sscenc_haar1(float *X, int N0, int stride) {
    int i, j;
    N0 >>= 1;
    for (i = 0; i < stride; i++)
        for (j = 0; j < N0; j++) {
            float t1 = 0.70710678f * X[stride * 2 * j + i];
            float t2 = 0.70710678f * X[stride * (2 * j + 1) + i];
            X[stride * 2 * j + i] = t1 + t2;
            X[stride * (2 * j + 1) + i] = t1 - t2;
        }
}

static const int ordery_table[] = {
    1, 0, 3, 0, 2, 1, 7, 0, 4, 3, 6, 1, 5, 2, 15, 0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5,
};

static void deinterleave_hadamard(float *X, int N0, int stride, int hadamard) {
    float tmp[SSCE_MAX_BAND_N];
    int i, j, N = N0 * stride;
    if (hadamard) {
        const int *ordery = ordery_table + stride - 2;
        for (i = 0; i < stride; i++)
            for (j = 0; j < N0; j++) tmp[ordery[i] * N0 + j] = X[j * stride + i];
    } else {
        for (i = 0; i < stride; i++)
            for (j = 0; j < N0; j++) tmp[i * N0 + j] = X[j * stride + i];
    }
    memcpy(X, tmp, (size_t)N * sizeof(float));
}

static void intensity_stereo(float *X, const float *Y, float left, float right, int N) {
    int j;
    float norm = 1e-15f + sqrtf(1e-15f + left * left + right * right);
    float a1 = left / norm, a2 = right / norm;
    for (j = 0; j < N; j++) X[j] = a1 * X[j] + a2 * Y[j];
}

static void stereo_split(float *X, float *Y, int N) {
    int j;
    for (j = 0; j < N; j++) {
        float l = 0.70710678f * X[j], r = 0.70710678f * Y[j];
        X[j] = l + r;
        Y[j] = r - l;
    }
}

/* ---- band coding context ---- */
typedef struct {
    const sscenc_mode *m;
    int i;
    int intensity;
    int spread;
    int tf_change;
    sscenc_ec *ec;
    int32_t remaining_bits;
    const float *bandE;
} band_ctx;

typedef struct {
    int inv, imid, iside, delta, itheta, qalloc;
} split_ctx;

static void compute_theta(band_ctx *ctx, split_ctx *sctx, float *X, float *Y, int N, int *b, int B0, int LM,
                          int stereo) {
    const sscenc_mode *m = ctx->m;
    const int i = ctx->i;
    sscenc_ec *ec = ctx->ec;
    int qn, itheta, delta, imid, iside, qalloc, pulse_cap, offset, inv = 0;
    int32_t tell;

    pulse_cap = m->logN[i] + LM * (1 << SSCE_BITRES);
    offset = (pulse_cap >> 1) - QTHETA_OFFSET;
    qn = compute_qn(N, *b, offset, pulse_cap);
    if (stereo && i >= ctx->intensity) qn = 1;
    itheta = stereo_itheta(X, Y, stereo, N);
    tell = (int32_t)sscenc_ec_tell_frac(ec);
    if (qn != 1) {
        itheta = (itheta * qn + 8192) >> 14;
        if (stereo && N > 2) {
            /* step pdf: probability 3 up to qn/2, then 1 */
            const int p0 = 3, x = itheta, x0 = qn / 2, ft = p0 * (x0 + 1) + x0;
            sscenc_ec_encode(ec, (unsigned)(x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0),
                             (unsigned)(x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0), (unsigned)ft);
        } else if (B0 > 1 || stereo) {
            sscenc_ec_uint(ec, (uint32_t)itheta, (uint32_t)qn + 1); /* uniform */
        } else {
            /* triangular */
            int ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
            int fs = itheta <= (qn >> 1) ? itheta + 1 : qn + 1 - itheta;
            int fl = itheta <= (qn >> 1) ? itheta * (itheta + 1) >> 1
                                         : ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
            sscenc_ec_encode(ec, (unsigned)fl, (unsigned)(fl + fs), (unsigned)ft);
        }
        itheta = (int32_t)itheta * 16384 / qn;
        if (stereo) {
            if (itheta == 0)
                intensity_stereo(X, Y, ctx->bandE[i], ctx->bandE[i + SSCE_NB], N);
            else
                stereo_split(X, Y, N);
        }
    } else if (stereo) {
        int j;
        inv = itheta > 8192;
        if (inv)
            for (j = 0; j < N; j++) Y[j] = -Y[j];
        intensity_stereo(X, Y, ctx->bandE[i], ctx->bandE[i + SSCE_NB], N);
        if (*b > 2 << SSCE_BITRES && ctx->remaining_bits > 2 << SSCE_BITRES) sscenc_ec_bit_logp(ec, inv, 2);
        else inv = 0;
        itheta = 0;
    } else {
        itheta = 0; /* qn == 1 in a time/frequency split: nothing is coded */
    }
    qalloc = (int)((int32_t)sscenc_ec_tell_frac(ec) - tell);
    *b -= qalloc;

    if (itheta == 0) {
        imid = 32767;
        iside = 0;
        delta = -16384;
    } else if (itheta == 16384) {
        imid = 0;
        iside = 32767;
        delta = 16384;
    } else {
        imid = bitexact_cos(itheta);
        iside = bitexact_cos(16384 - itheta);
        delta = frac_mul16((N - 1) << 7, bitexact_log2tan(iside, imid));
    }
    sctx->inv = inv;
    sctx->imid = imid;
    sctx->iside = iside;
    sctx->delta = delta;
    sctx->itheta = itheta;
    sctx->qalloc = qalloc;
}

static void quant_partition(band_ctx *ctx, float *X, int N, int b, int B, int LM) {
    const sscenc_mode *m = ctx->m;
    const int i = ctx->i, B0 = B;
    const unsigned char *cache = band_cache(m, i, LM);

    if (LM != -1 && b > cache[cache[0]] + 12 && N > 2) {
        int mbits, sbits, delta, itheta, qalloc;
        split_ctx sctx;
        float *Y;
        int32_t rebalance;

        N >>= 1;
        Y = X + N;
        LM -= 1;
        B = (B + 1) >> 1;
        compute_theta(ctx, &sctx, X, Y, N, &b, B0, LM, 0);
        delta = sctx.delta;
        itheta = sctx.itheta;
        qalloc = sctx.qalloc;
        if (B0 > 1 && (itheta & 0x3fff)) {
            if (itheta > 8192) delta -= delta >> (4 - LM);
            else delta = sscenc_imin(0, delta + (N << SSCE_BITRES >> (5 - LM)));
        }
        mbits = sscenc_imax(0, sscenc_imin(b, (b - delta) / 2));
        sbits = b - mbits;
        ctx->remaining_bits -= qalloc;
        rebalance = ctx->remaining_bits;
        if (mbits >= sbits) {
            quant_partition(ctx, X, N, mbits, B, LM);
            rebalance = mbits - (rebalance - ctx->remaining_bits);
            if (rebalance > 3 << SSCE_BITRES && itheta != 0) sbits += rebalance - (3 << SSCE_BITRES);
            quant_partition(ctx, Y, N, sbits, B, LM);
        } else {
            quant_partition(ctx, Y, N, sbits, B, LM);
            rebalance = sbits - (rebalance - ctx->remaining_bits);
            if (rebalance > 3 << SSCE_BITRES && itheta != 16384) mbits += rebalance - (3 << SSCE_BITRES);
            quant_partition(ctx, X, N, mbits, B, LM);
        }
    } else {
        int q = bits2pulses(m, i, LM, b), curr_bits = pulses2bits(m, i, LM, q);
        ctx->remaining_bits -= curr_bits;
        while (ctx->remaining_bits < 0 && q > 0) {
            ctx->remaining_bits += curr_bits;
            q--;
            curr_bits = pulses2bits(m, i, LM, q);
            ctx->remaining_bits -= curr_bits;
        }
        if (q != 0) alg_quant(X, N, get_pulses(q), ctx->spread, B, ctx->ec);
    }
}

static void quant_band(band_ctx *ctx, float *X, int N, int b, int B, int LM) {
    int N_B = N / B, B0, k, recombine = 0, longBlocks = B == 1, tf_change = ctx->tf_change;
    if (tf_change > 0) recombine = tf_change;
    for (k = 0; k < recombine; k++) sscenc_haar1(X, N >> k, 1 << k);
    B >>= recombine;
    N_B <<= recombine;
    while ((N_B & 1) == 0 && tf_change < 0) {
        sscenc_haar1(X, N_B, B);
        B <<= 1;
        N_B >>= 1;
        tf_change++;
    }
    B0 = B;
    if (B0 > 1) deinterleave_hadamard(X, N_B >> recombine, B0 << recombine, longBlocks);
    quant_partition(ctx, X, N, b, B, LM);
}

static void quant_band_stereo(band_ctx *ctx, float *X, float *Y, int N, int b, int B, int LM) {
    split_ctx sctx;
    int mbits, sbits, delta, itheta, qalloc;
    int32_t rebalance;

    compute_theta(ctx, &sctx, X, Y, N, &b, B, LM, 1);
    delta = sctx.delta;
    itheta = sctx.itheta;
    qalloc = sctx.qalloc;
    mbits = sscenc_imax(0, sscenc_imin(b, (b - delta) / 2));
    sbits = b - mbits;
    ctx->remaining_bits -= qalloc;
    rebalance = ctx->remaining_bits;
    if (mbits >= sbits) {
        quant_band(ctx, X, N, mbits, B, LM);
        rebalance = mbits - (rebalance - ctx->remaining_bits);
        if (rebalance > 3 << SSCE_BITRES && itheta != 0) sbits += rebalance - (3 << SSCE_BITRES);
        quant_band(ctx, Y, N, sbits, B, LM);
    } else {
        quant_band(ctx, Y, N, sbits, B, LM);
        rebalance = sbits - (rebalance - ctx->remaining_bits);
        if (rebalance > 3 << SSCE_BITRES && itheta != 16384) mbits += rebalance - (3 << SSCE_BITRES);
        quant_band(ctx, X, N, mbits, B, LM);
    }
}

/* X, Y: normalised spectra of the two channels (modified in place). bandE: linear band amplitudes [c*22+i]. */
void sscenc_quant_all_bands(const sscenc_mode *m, int end, float *X_, float *Y_, const float *bandE,
                            const int *pulses, int shortBlocks, int spread, int dual_stereo, int intensity,
                            const int *tf_res, int32_t total_bits, int32_t balance, sscenc_ec *ec, int codedBands) {
    const short *eBands = m->eBands;
    const int M = SSCE_M, LM = SSCE_LM, B = shortBlocks ? M : 1;
    band_ctx ctx;
    int i;

    ctx.m = m;
    ctx.ec = ec;
    ctx.intensity = intensity;
    ctx.spread = spread;
    ctx.bandE = bandE;
    for (i = 0; i < end; i++) {
        int32_t tell, remaining_bits, curr_balance;
        int b, N;
        float *X = X_ + M * eBands[i], *Y = Y_ + M * eBands[i];

        ctx.i = i;
        N = M * eBands[i + 1] - M * eBands[i];
        tell = (int32_t)sscenc_ec_tell_frac(ec);
        if (i != 0) balance -= tell;
        remaining_bits = total_bits - tell - 1;
        ctx.remaining_bits = remaining_bits;
        if (i <= codedBands - 1) {
            curr_balance = balance / sscenc_imin(3, codedBands - i);
            b = sscenc_imax(0, sscenc_imin(16383, sscenc_imin(remaining_bits + 1, pulses[i] + curr_balance)));
        } else {
            b = 0;
        }
        ctx.tf_change = tf_res[i];
        if (dual_stereo && i == intensity) dual_stereo = 0;
        if (dual_stereo) {
            quant_band(&ctx, X, N, b / 2, B, LM);
            quant_band(&ctx, Y, N, b / 2, B, LM);
        } else {
            quant_band_stereo(&ctx, X, Y, N, b, B, LM);
        }
        balance += pulses[i] + tell;
    }
}
