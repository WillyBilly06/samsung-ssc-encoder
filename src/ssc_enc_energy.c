/* SSC encoder: band energy quantisation (coarse, fine, finalise).
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 * Adapted from libopus 1.1 celt/quant_bands.c, fixed-point arithmetic
 * (Copyright 2007-2009 Xiph.Org Foundation, 2008-2009 Gregory Maxwell, BSD-3-Clause, see LICENSE).
 *
 * All values are int16 Q10 log2 amplitudes, layout [c*22 + band]. The prediction state oldE is updated
 * with the same integer arithmetic as the decoder, so encoder and decoder stay in lock-step. SSC always
 * codes two channels, LM = 3 (prediction 0.5, beta 0.2; intra beta 0.15) and start band 0. */
#include <stdlib.h>
#include <string.h>

#include "ssc_enc_internal.h"

#define PRED_COEF   16384 /* 0.5 Q15 */
#define BETA_COEF   6554  /* 0.2 Q15 */
#define BETA_INTRA  4915  /* 0.15 Q15 */
#define Q10(x)      ((int32_t)((x) * (1 << SSCE_DB_SHIFT)))

static const unsigned char small_energy_icdf[3] = {2, 1, 0};

static int coarse_pass(int end, const int16_t *x, int16_t *oldE, int32_t budget, int intra, int32_t max_decay,
                       int16_t *error, sscenc_ec *enc) {
    const unsigned char *prob = sscenc_e_prob_model[intra ? 1 : 0];
    int32_t prev[2] = {0, 0};
    const int32_t coef = intra ? 0 : PRED_COEF;
    const int32_t beta = intra ? BETA_INTRA : BETA_COEF;
    int i, c, badness = 0;

    if (sscenc_ec_tell(enc) + 3 <= budget) sscenc_ec_bit_logp(enc, intra, 3);
    for (i = 0; i < end; i++) {
        for (c = 0; c < SSCE_C; c++) {
            const int k = i + c * SSCE_NB;
            int32_t f, q, tmp, decay_bound, old = oldE[k];
            int32_t oldc = old < -Q10(9) ? -Q10(9) : old;
            int qi, qi0, bits_left, tell;
            f = ((int32_t)x[k] << 7) - ((coef * oldc + 128) >> 8) - prev[c];
            qi = (f + (1 << (SSCE_DB_SHIFT + 6))) >> (SSCE_DB_SHIFT + 7); /* round to nearest */
            decay_bound = old - max_decay;
            if (decay_bound < -Q10(28)) decay_bound = -Q10(28);
            /* do not let the energy drop faster than max_decay per frame */
            if (qi < 0 && x[k] < decay_bound) {
                qi += (int)((decay_bound - x[k]) >> SSCE_DB_SHIFT);
                if (qi > 0) qi = 0;
            }
            qi0 = qi;
            tell = sscenc_ec_tell(enc);
            bits_left = budget - tell - 3 * SSCE_C * (end - i);
            if (i != 0 && bits_left < 30) {
                if (bits_left < 24) qi = sscenc_imin(1, qi);
                if (bits_left < 16) qi = sscenc_imax(-1, qi);
            }
            if (budget - tell >= 15) {
                int pi = 2 * sscenc_imin(i, 20);
                sscenc_laplace_encode(enc, &qi, (unsigned)prob[pi] << 7, prob[pi + 1] << 6);
            } else if (budget - tell >= 2) {
                qi = sscenc_imax(-1, sscenc_imin(qi, 1));
                sscenc_ec_icdf(enc, 2 * qi ^ -(qi < 0), small_energy_icdf, 2);
            } else if (budget - tell >= 1) {
                qi = sscenc_imin(0, qi);
                sscenc_ec_bit_logp(enc, -qi, 1);
            } else {
                qi = -1;
            }
            error[k] = (int16_t)(((f + 64) >> 7) - (qi << SSCE_DB_SHIFT));
            badness += abs(qi0 - qi);
            q = (int32_t)((uint32_t)qi << SSCE_DB_SHIFT);
            tmp = ((coef * oldc + 128) >> 8) + prev[c] + (int32_t)((uint32_t)q << 7);
            if (tmp < -(28 << (SSCE_DB_SHIFT + 7))) tmp = -(28 << (SSCE_DB_SHIFT + 7));
            oldE[k] = (int16_t)((tmp + 64) >> 7);
            prev[c] = prev[c] + (int32_t)((uint32_t)q << 7) - beta * ((q + 128) >> 8);
        }
    }
    return badness;
}

/* Two-pass coarse quantisation: tries intra and inter prediction and keeps the cheaper one (fewer
 * clamped deltas first, then fewer bits). Returns the intra flag that was coded. */
int sscenc_quant_coarse_energy(int end, const int16_t *x, int16_t *oldE, int32_t budget, int nbAvailableBytes,
                               int16_t *error, sscenc_ec *enc) {
    int16_t old_intra[SSCE_C * SSCE_NB], err_intra[SSCE_C * SSCE_NB];
    unsigned char intra_bytes[1300];
    sscenc_ec start = *enc, after_intra;
    int32_t max_decay = Q10(16);
    int badness1, badness2, two_pass = 1;
    uint32_t tell_intra, nstart, nintra;

    if (sscenc_ec_tell(enc) + 3 > budget) two_pass = 0;
    if (end > 10 && ((int32_t)nbAvailableBytes << (SSCE_DB_SHIFT - 3)) < max_decay)
        max_decay = (int32_t)nbAvailableBytes << (SSCE_DB_SHIFT - 3);

    if (!two_pass) {
        coarse_pass(end, x, oldE, budget, 0, max_decay, error, enc);
        return 0;
    }
    memcpy(old_intra, oldE, sizeof old_intra);
    badness1 = coarse_pass(end, x, old_intra, budget, 1, max_decay, err_intra, enc);
    tell_intra = sscenc_ec_tell_frac(enc);
    after_intra = *enc;
    /* The range coder only writes bytes forward from offs; keep the ones the intra pass produced. */
    nstart = start.offs;
    nintra = after_intra.offs;
    if (nintra - nstart > sizeof intra_bytes) nintra = nstart + (uint32_t)sizeof intra_bytes; /* not reachable */
    memcpy(intra_bytes, enc->buf + nstart, nintra - nstart);
    *enc = start;
    badness2 = coarse_pass(end, x, oldE, budget, 0, max_decay, error, enc);
    if (badness1 < badness2 || (badness1 == badness2 && sscenc_ec_tell_frac(enc) > tell_intra)) {
        *enc = after_intra;
        memcpy(enc->buf + nstart, intra_bytes, nintra - nstart);
        memcpy(oldE, old_intra, sizeof old_intra);
        memcpy(error, err_intra, sizeof err_intra);
        return 1;
    }
    return 0;
}

void sscenc_quant_fine_energy(int end, int16_t *oldE, int16_t *error, const int *fine_quant, sscenc_ec *enc) {
    int i, c;
    for (i = 0; i < end; i++) {
        const int fq = fine_quant[i];
        const int frac = 1 << fq;
        if (fq <= 0) continue;
        for (c = 0; c < SSCE_C; c++) {
            const int k = i + c * SSCE_NB;
            int q2 = (error[k] + (1 << (SSCE_DB_SHIFT - 1))) >> (SSCE_DB_SHIFT - fq);
            int16_t offset;
            if (q2 > frac - 1) q2 = frac - 1;
            if (q2 < 0) q2 = 0;
            sscenc_ec_bits(enc, (uint32_t)q2, (unsigned)fq);
            offset = (int16_t)(((((int32_t)q2 << SSCE_DB_SHIFT) + (1 << (SSCE_DB_SHIFT - 1))) >> fq) -
                               (1 << (SSCE_DB_SHIFT - 1)));
            oldE[k] = (int16_t)(oldE[k] + offset);
            error[k] = (int16_t)(error[k] - offset);
        }
    }
}

void sscenc_quant_energy_finalise(int end, int16_t *oldE, int16_t *error, const int *fine_quant,
                                  const int *fine_priority, int bits_left, sscenc_ec *enc) {
    int i, prio, c;
    for (prio = 0; prio < 2; prio++) {
        for (i = 0; i < end && bits_left >= SSCE_C; i++) {
            if (fine_quant[i] >= SSCE_MAX_FINE || fine_priority[i] != prio) continue;
            for (c = 0; c < SSCE_C; c++) {
                const int k = i + c * SSCE_NB;
                int q2 = error[k] < 0 ? 0 : 1;
                int16_t offset;
                sscenc_ec_bits(enc, (uint32_t)q2, 1);
                offset = (int16_t)((((int32_t)q2 << SSCE_DB_SHIFT) - (1 << (SSCE_DB_SHIFT - 1))) >>
                                   (fine_quant[i] + 1));
                oldE[k] = (int16_t)(oldE[k] + offset);
                error[k] = (int16_t)(error[k] - offset);
                bits_left--;
            }
        }
    }
}
