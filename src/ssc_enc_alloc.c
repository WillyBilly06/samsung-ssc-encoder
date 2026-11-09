/* SSC encoder: bit allocation (encoder side of compute_allocation / interp_bits2pulses).
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 * Adapted from libopus 1.1 celt/rate.c (Copyright 2007-2009 Xiph.Org Foundation, BSD-3-Clause, see LICENSE).
 *
 * SSC specifics: C = 2 and LM = 3 always, start = 0, per-band caps from the mode table, and the allocation
 * vector table is chosen by bitrate (> 192 kb/s uses the second table). Everything here is integer and must
 * follow the decoder exactly; the only encoder choices are the band-skip flags, the intensity band and the
 * dual-stereo flag. */
#include "ssc_enc_internal.h"

#define ALLOC_STEPS 6
#define FINE_OFFSET 21
#define NB_VECTORS  11

static int interp_bits2pulses(const sscenc_mode *m, int end, int skip_start, const int *bits1, const int *bits2,
                              const int *thresh, const int *cap, int32_t total, int32_t *_balance, int skip_rsv,
                              int *intensity, int intensity_rsv, int *dual_stereo, int dual_stereo_rsv, int *bits,
                              int *ebits, int *fine_priority, int prev, int signalBandwidth, sscenc_ec *ec) {
    const int C = SSCE_C, alloc_floor = C << SSCE_BITRES, logM = SSCE_LM << SSCE_BITRES;
    const short *eBands = m->eBands;
    int32_t psum, left, percoeff, balance;
    int lo = 0, hi = 1 << ALLOC_STEPS, i, j, done, codedBands;

    for (i = 0; i < ALLOC_STEPS; i++) {
        int mid = (lo + hi) >> 1;
        psum = 0;
        done = 0;
        for (j = end; j-- > 0;) {
            int tmp = bits1[j] + (int)(((int32_t)mid * bits2[j]) >> ALLOC_STEPS);
            if (tmp >= thresh[j] || done) {
                done = 1;
                psum += sscenc_imin(tmp, cap[j]);
            } else if (tmp >= alloc_floor) {
                psum += alloc_floor;
            }
        }
        if (psum > total) hi = mid;
        else lo = mid;
    }
    psum = 0;
    done = 0;
    for (j = end; j-- > 0;) {
        int tmp = bits1[j] + (int)(((int32_t)lo * bits2[j]) >> ALLOC_STEPS);
        if (tmp < thresh[j] && !done) tmp = tmp >= alloc_floor ? alloc_floor : 0;
        else done = 1;
        tmp = sscenc_imin(tmp, cap[j]);
        bits[j] = tmp;
        psum += tmp;
    }

    /* Decide which bands to skip, working backwards from the end. */
    for (codedBands = end;; codedBands--) {
        int band_width, band_bits, rem;
        j = codedBands - 1;
        if (j <= skip_start) {
            total += skip_rsv; /* give the reserved skip bit back */
            break;
        }
        left = total - psum;
        percoeff = left / (eBands[codedBands] - eBands[0]);
        left -= (eBands[codedBands] - eBands[0]) * percoeff;
        rem = sscenc_imax(left - (eBands[j] - eBands[0]), 0);
        band_width = eBands[codedBands] - eBands[j];
        band_bits = (int)(bits[j] + percoeff * band_width + rem);
        if (band_bits >= sscenc_imax(thresh[j], alloc_floor + (1 << SSCE_BITRES))) {
            /* The only non-normative part of the allocation: keep the band (hysteresis against the
             * previous frame's coded band count) or signal a skip. */
            if (codedBands <= 2 ||
                (band_bits > ((j < prev ? 7 : 9) * band_width << SSCE_LM << SSCE_BITRES) >> 4 &&
                 j <= signalBandwidth)) {
                sscenc_ec_bit_logp(ec, 1, 1);
                break;
            }
            sscenc_ec_bit_logp(ec, 0, 1);
            psum += 1 << SSCE_BITRES;
            band_bits -= 1 << SSCE_BITRES;
        }
        /* Reclaim the bits originally allocated to this band. */
        psum -= bits[j] + intensity_rsv;
        if (intensity_rsv > 0) intensity_rsv = sscenc_log2_frac[j];
        psum += intensity_rsv;
        if (band_bits >= alloc_floor) {
            psum += alloc_floor;
            bits[j] = alloc_floor;
        } else {
            bits[j] = 0;
        }
    }

    /* Intensity and dual stereo parameters. */
    if (intensity_rsv > 0) {
        *intensity = sscenc_imin(*intensity, codedBands);
        sscenc_ec_uint(ec, (uint32_t)*intensity, (uint32_t)(codedBands + 1));
    } else {
        *intensity = 0;
    }
    if (*intensity <= 0) {
        total += dual_stereo_rsv;
        dual_stereo_rsv = 0;
    }
    if (dual_stereo_rsv > 0) sscenc_ec_bit_logp(ec, *dual_stereo, 1);
    else *dual_stereo = 0;

    /* Allocate the remaining bits */
    left = total - psum;
    percoeff = left / (eBands[codedBands] - eBands[0]);
    left -= (eBands[codedBands] - eBands[0]) * percoeff;
    for (j = 0; j < codedBands; j++) bits[j] += (int)(percoeff * (eBands[j + 1] - eBands[j]));
    for (j = 0; j < codedBands; j++) {
        int tmp = (int)sscenc_imin(left, eBands[j + 1] - eBands[j]);
        bits[j] += tmp;
        left -= tmp;
    }

    balance = 0;
    for (j = 0; j < codedBands; j++) {
        int N0 = eBands[j + 1] - eBands[j], den, offset, NClogN;
        int32_t excess, bit;
        bit = (int32_t)bits[j] + balance;
        excess = bit - cap[j];
        if (excess < 0) excess = 0;
        bits[j] = bit - excess;
        den = C * (N0 << SSCE_LM) + ((!*dual_stereo && j < *intensity) ? 1 : 0);
        NClogN = den * (m->logN[j] + logM);
        offset = (NClogN >> 1) - den * FINE_OFFSET;
        if (bits[j] + offset < den * 2 << SSCE_BITRES) offset += NClogN >> 2;
        else if (bits[j] + offset < den * 3 << SSCE_BITRES) offset += NClogN >> 3;
        ebits[j] = sscenc_imax(0, (bits[j] + offset + (den << (SSCE_BITRES - 1))) / (den << SSCE_BITRES));
        if (C * ebits[j] > (bits[j] >> SSCE_BITRES)) ebits[j] = bits[j] >> 1 >> SSCE_BITRES;
        ebits[j] = sscenc_imin(ebits[j], SSCE_MAX_FINE);
        fine_priority[j] = ebits[j] * (den << SSCE_BITRES) >= bits[j] + offset;
        bits[j] -= C * ebits[j] << SSCE_BITRES;
        if (excess > 0) {
            int extra_fine, extra_bits;
            extra_fine = sscenc_imin((int)(excess >> (1 + SSCE_BITRES)), SSCE_MAX_FINE - ebits[j]);
            ebits[j] += extra_fine;
            extra_bits = extra_fine * C << SSCE_BITRES;
            fine_priority[j] = extra_bits >= excess - balance;
            excess -= extra_bits;
        }
        balance = excess;
    }
    *_balance = balance;
    for (; j < end; j++) {
        ebits[j] = bits[j] >> 1 >> SSCE_BITRES;
        bits[j] = 0;
        fine_priority[j] = ebits[j] < 1;
    }
    return codedBands;
}

int sscenc_compute_allocation(const sscenc_mode *m, int end, const int *offsets, int alloc_trim, int hi_rate,
                              int32_t total, int32_t *balance, int *pulses, int *ebits, int *fine_priority,
                              int *intensity, int *dual_stereo, int prev_coded, int signal_bw, sscenc_ec *ec) {
    const int C = SSCE_C, LM = SSCE_LM, len = SSCE_NB;
    const unsigned char *av = m->allocVectors[hi_rate ? 1 : 0];
    const int *cap = m->cap;
    int lo, hi, j, skip_start = 0, skip_rsv, intensity_rsv, dual_stereo_rsv;
    int bits1[SSCE_NB], bits2[SSCE_NB], thresh[SSCE_NB], trim_offset[SSCE_NB];

    if (total < 0) total = 0;
    skip_rsv = total >= 1 << SSCE_BITRES ? 1 << SSCE_BITRES : 0;
    total -= skip_rsv;
    intensity_rsv = sscenc_log2_frac[end];
    dual_stereo_rsv = 0;
    if (intensity_rsv > total) {
        intensity_rsv = 0;
    } else {
        total -= intensity_rsv;
        dual_stereo_rsv = total >= 1 << SSCE_BITRES ? 1 << SSCE_BITRES : 0;
        total -= dual_stereo_rsv;
    }
    for (j = 0; j < end; j++) {
        int N0 = m->eBands[j + 1] - m->eBands[j];
        thresh[j] = sscenc_imax(C << SSCE_BITRES, (3 * N0 << LM << SSCE_BITRES) >> 4);
        trim_offset[j] = C * N0 * (alloc_trim - 5 - LM) * (end - j - 1);
    }
    lo = 1;
    hi = NB_VECTORS - 1;
    do {
        int done = 0, mid = (lo + hi) >> 1;
        int32_t psum = 0;
        for (j = end; j-- > 0;) {
            int N0 = m->eBands[j + 1] - m->eBands[j];
            int bitsj = C * N0 * av[mid * len + j] << LM >> 2;
            if (bitsj > 0) bitsj = sscenc_imax(0, bitsj + trim_offset[j]);
            bitsj += offsets[j];
            if (bitsj >= thresh[j] || done) {
                done = 1;
                psum += sscenc_imin(bitsj, cap[j]);
            } else if (bitsj >= C << SSCE_BITRES) {
                psum += C << SSCE_BITRES;
            }
        }
        if (psum > total) hi = mid - 1;
        else lo = mid + 1;
    } while (lo <= hi);
    hi = lo--;
    for (j = 0; j < end; j++) {
        int N0 = m->eBands[j + 1] - m->eBands[j];
        int bits1j = C * N0 * av[lo * len + j] << LM >> 2;
        int bits2j = hi >= NB_VECTORS ? cap[j] : C * N0 * av[hi * len + j] << LM >> 2;
        if (bits1j > 0) bits1j = sscenc_imax(0, bits1j + trim_offset[j]);
        if (bits2j > 0) bits2j = sscenc_imax(0, bits2j + trim_offset[j]);
        if (lo > 0) bits1j += offsets[j];
        bits2j += offsets[j];
        if (offsets[j] > 0) skip_start = j;
        bits2j = sscenc_imax(0, bits2j - bits1j);
        bits1[j] = bits1j;
        bits2[j] = bits2j;
    }
    return interp_bits2pulses(m, end, skip_start, bits1, bits2, thresh, cap, total, balance, skip_rsv, intensity,
                              intensity_rsv, dual_stereo, dual_stereo_rsv, pulses, ebits, fine_priority, prev_coded,
                              signal_bw, ec);
}
