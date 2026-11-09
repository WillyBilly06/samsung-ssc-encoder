/* SSC encoder: range encoder.
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 * Adapted from libopus 1.1 celt/entenc.c, celt/entcode.c and celt/laplace.c
 * (Copyright 2001-2011 Timothy B. Terriberry, Xiph.Org Foundation, BSD-3-Clause, see LICENSE).
 *
 * The only change from opus is the coder width: SSC uses EC_CODE_BITS = 24 instead of 32, so the
 * range is kept in [2^15, 2^23]. Raw bits (ec_enc_bits) are packed from the end of the buffer as in
 * opus; the buffer passed to sscenc_ec_init is the whole range-coded payload. */
#include <string.h>

#include "ssc_enc_internal.h"

#define EC_SYM_BITS    8
#define EC_CODE_BITS   24
#define EC_SYM_MAX     ((1U << EC_SYM_BITS) - 1)
#define EC_CODE_SHIFT  (EC_CODE_BITS - EC_SYM_BITS - 1)
#define EC_CODE_TOP    (((uint32_t)1U) << (EC_CODE_BITS - 1))
#define EC_CODE_BOT    (EC_CODE_TOP >> EC_SYM_BITS)
#define EC_WINDOW_SIZE 32
#define EC_UINT_BITS   8

int sscenc_ec_tell(const sscenc_ec *e) { return e->nbits_total - sscenc_ilog(e->rng); }

uint32_t sscenc_ec_tell_frac(const sscenc_ec *e) {
    uint32_t nbits = (uint32_t)e->nbits_total << SSCE_BITRES, r;
    int l = sscenc_ilog(e->rng), i;
    r = e->rng >> (l - 16);
    for (i = SSCE_BITRES; i-- > 0;) {
        int b;
        r = r * r >> 15;
        b = (int)(r >> 16);
        l = l << 1 | b;
        r >>= b;
    }
    return nbits - (uint32_t)l;
}

static int write_byte(sscenc_ec *e, unsigned v) {
    if (e->offs + e->end_offs >= e->storage) return -1;
    e->buf[e->offs++] = (unsigned char)v;
    return 0;
}

static int write_byte_at_end(sscenc_ec *e, unsigned v) {
    if (e->offs + e->end_offs >= e->storage) return -1;
    e->buf[e->storage - ++(e->end_offs)] = (unsigned char)v;
    return 0;
}

/* Outputs a symbol, with a carry bit. Runs of 0xFF are buffered until the carry is known. */
static void carry_out(sscenc_ec *e, int c) {
    if (c != (int)EC_SYM_MAX) {
        int carry = c >> EC_SYM_BITS;
        if (e->rem >= 0) e->error |= write_byte(e, (unsigned)(e->rem + carry));
        if (e->ext > 0) {
            unsigned sym = (EC_SYM_MAX + (unsigned)carry) & EC_SYM_MAX;
            do e->error |= write_byte(e, sym);
            while (--(e->ext) > 0);
        }
        e->rem = c & (int)EC_SYM_MAX;
    } else {
        e->ext++;
    }
}

static void normalize(sscenc_ec *e) {
    while (e->rng <= EC_CODE_BOT) {
        carry_out(e, (int)(e->val >> EC_CODE_SHIFT));
        e->val = (e->val << EC_SYM_BITS) & (EC_CODE_TOP - 1);
        e->rng <<= EC_SYM_BITS;
        e->nbits_total += EC_SYM_BITS;
    }
}

void sscenc_ec_init(sscenc_ec *e, unsigned char *buf, uint32_t size) {
    e->buf = buf;
    e->end_offs = 0;
    e->end_window = 0;
    e->nend_bits = 0;
    e->nbits_total = EC_CODE_BITS + 1;
    e->offs = 0;
    e->rng = EC_CODE_TOP;
    e->rem = -1;
    e->val = 0;
    e->ext = 0;
    e->storage = size;
    e->error = 0;
}

void sscenc_ec_encode(sscenc_ec *e, unsigned fl, unsigned fh, unsigned ft) {
    uint32_t r = e->rng / ft;
    if (fl > 0) {
        e->val += e->rng - r * (ft - fl);
        e->rng = r * (fh - fl);
    } else {
        e->rng -= r * (ft - fh);
    }
    normalize(e);
}

void sscenc_ec_encode_bin(sscenc_ec *e, unsigned fl, unsigned fh, unsigned bits) {
    uint32_t r = e->rng >> bits;
    if (fl > 0) {
        e->val += e->rng - r * ((1U << bits) - fl);
        e->rng = r * (fh - fl);
    } else {
        e->rng -= r * ((1U << bits) - fh);
    }
    normalize(e);
}

void sscenc_ec_bit_logp(sscenc_ec *e, int val, unsigned logp) {
    uint32_t r = e->rng, l = e->val, s = r >> logp;
    r -= s;
    if (val) e->val = l + r;
    e->rng = val ? s : r;
    normalize(e);
}

void sscenc_ec_icdf(sscenc_ec *e, int s, const unsigned char *icdf, unsigned ftb) {
    uint32_t r = e->rng >> ftb;
    if (s > 0) {
        e->val += e->rng - r * icdf[s - 1];
        e->rng = r * (uint32_t)(icdf[s - 1] - icdf[s]);
    } else {
        e->rng -= r * icdf[s];
    }
    normalize(e);
}

void sscenc_ec_bits(sscenc_ec *e, uint32_t fl, unsigned bits) {
    uint32_t window = e->end_window;
    int used = e->nend_bits;
    if (used + (int)bits > EC_WINDOW_SIZE) {
        do {
            e->error |= write_byte_at_end(e, window & EC_SYM_MAX);
            window >>= EC_SYM_BITS;
            used -= EC_SYM_BITS;
        } while (used >= EC_SYM_BITS);
    }
    window |= fl << used;
    used += (int)bits;
    e->end_window = window;
    e->nend_bits = used;
    e->nbits_total += (int)bits;
}

void sscenc_ec_uint(sscenc_ec *e, uint32_t fl, uint32_t ft) {
    int ftb;
    ft--;
    ftb = sscenc_ilog(ft);
    if (ftb > EC_UINT_BITS) {
        unsigned ft1, fl1;
        ftb -= EC_UINT_BITS;
        ft1 = (unsigned)(ft >> ftb) + 1;
        fl1 = (unsigned)(fl >> ftb);
        sscenc_ec_encode(e, fl1, fl1 + 1, ft1);
        sscenc_ec_bits(e, fl & (((uint32_t)1 << ftb) - 1U), (unsigned)ftb);
    } else {
        sscenc_ec_encode(e, (unsigned)fl, (unsigned)fl + 1, (unsigned)ft + 1);
    }
}

void sscenc_ec_done(sscenc_ec *e) {
    uint32_t window, msk, end;
    int used, l;
    /* Output the minimum number of bits that ensures that the symbols encoded so far are decoded correctly
     * whatever follows. */
    l = EC_CODE_BITS - sscenc_ilog(e->rng);
    msk = (EC_CODE_TOP - 1) >> l;
    end = (e->val + msk) & ~msk;
    if ((end | msk) >= e->val + e->rng) {
        l++;
        msk >>= 1;
        end = (e->val + msk) & ~msk;
    }
    while (l > 0) {
        carry_out(e, (int)(end >> EC_CODE_SHIFT));
        end = (end << EC_SYM_BITS) & (EC_CODE_TOP - 1);
        l -= EC_SYM_BITS;
    }
    if (e->rem >= 0 || e->ext > 0) carry_out(e, 0);
    window = e->end_window;
    used = e->nend_bits;
    while (used >= EC_SYM_BITS) {
        e->error |= write_byte_at_end(e, window & EC_SYM_MAX);
        window >>= EC_SYM_BITS;
        used -= EC_SYM_BITS;
    }
    if (!e->error) {
        memset(e->buf + e->offs, 0, e->storage - e->offs - e->end_offs);
        if (used > 0) {
            if (e->end_offs >= e->storage) {
                e->error = -1;
            } else {
                l = -l;
                if (e->offs + e->end_offs >= e->storage && l < used) {
                    window &= (1U << l) - 1;
                    e->error = -1;
                }
                e->buf[e->storage - e->end_offs - 1] |= (unsigned char)window;
            }
        }
    }
}

/* ---- Laplace-like energy deltas (laplace.c, LAPLACE_MINP = 1, LAPLACE_NMIN = 16) ---- */
static unsigned laplace_get_freq1(unsigned fs0, int decay) {
    unsigned ft = 32768 - 2 * 16 - fs0;
    return ft * (uint32_t)(16384 - decay) >> 15;
}

void sscenc_laplace_encode(sscenc_ec *e, int *value, unsigned fs, int decay) {
    unsigned fl = 0;
    int val = *value;
    if (val) {
        int s = -(val < 0), i;
        val = (val + s) ^ s;
        fl = fs;
        fs = laplace_get_freq1(fs, decay);
        for (i = 1; fs > 0 && i < val; i++) {
            fs *= 2;
            fl += fs + 2;
            fs = (fs * (uint32_t)decay) >> 15;
        }
        if (!fs) {
            int di, ndi_max;
            ndi_max = (int)(32768 - fl);
            ndi_max = (ndi_max - s) >> 1;
            di = sscenc_imin(val - i, ndi_max - 1);
            fl += (unsigned)(2 * di + 1 + s);
            fs = sscenc_imin(1, (int)(32768 - fl));
            *value = (i + di + s) ^ s;
        } else {
            fs += 1;
            fl += fs & ~(unsigned)s;
        }
    }
    sscenc_ec_encode_bin(e, fl, fl + fs, 15);
}
