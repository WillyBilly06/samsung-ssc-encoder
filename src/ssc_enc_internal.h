/* SSC encoder: internal declarations shared between the modules.
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 * Parts are adapted from libopus 1.1 (CELT), Copyright Xiph.Org Foundation and contributors,
 * BSD-3-Clause; their notice is reproduced in LICENSE.
 *
 * Conventions: every symbol of the library outside the public API starts with "sscenc_", so the
 * encoder can be linked into the same program as an SSC decoder or as libopus.
 * Band energies live in two forms:
 *   - float log2 amplitude (1.0 = 6.02 dB) relative to eMeans, used by the analysis code;
 *   - int16 Q10 (DB_SHIFT = 10), the exact integer state that the decoder also keeps. Everything
 *     that influences the bitstream syntax uses the integer form and integer arithmetic. */
#ifndef SSC_ENC_INTERNAL_H
#define SSC_ENC_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#define SSCE_N          864  /* samples per channel per frame = long MDCT coefficient count */
#define SSCE_OVERLAP    108  /* window overlap, also the short MDCT size */
#define SSCE_SHORT      108
#define SSCE_M          8    /* short blocks per frame (1 << LM) */
#define SSCE_LM         3
#define SSCE_C          2    /* the bitstream always codes two channels */
#define SSCE_NB         22   /* bands */
#define SSCE_BITRES     3
#define SSCE_DB_SHIFT   10
#define SSCE_MAX_FINE   8
#define SSCE_MAX_BAND_N 144  /* widest band: 18 * 8 bins */

#define SSCE_SPREAD_NONE       0
#define SSCE_SPREAD_LIGHT      1
#define SSCE_SPREAD_NORMAL     2
#define SSCE_SPREAD_AGGRESSIVE 3

static inline int sscenc_imin(int a, int b) { return a < b ? a : b; }
static inline int sscenc_imax(int a, int b) { return a > b ? a : b; }
static inline int sscenc_ilog(uint32_t v) {
    int r = 0;
    while (v) { r++; v >>= 1; }
    return r;
}

/* ---- range encoder (opus 1.1 entenc.c with a 24-bit coder, ssc_enc_ec.c) ---- */
typedef struct {
    unsigned char *buf;
    uint32_t storage;
    uint32_t end_offs;
    uint32_t end_window;
    int nend_bits;
    int nbits_total;
    uint32_t offs;
    uint32_t rng;
    uint32_t val;
    uint32_t ext;
    int rem;
    int error;
} sscenc_ec;

void sscenc_ec_init(sscenc_ec *e, unsigned char *buf, uint32_t size);
int sscenc_ec_tell(const sscenc_ec *e);
uint32_t sscenc_ec_tell_frac(const sscenc_ec *e);
void sscenc_ec_encode(sscenc_ec *e, unsigned fl, unsigned fh, unsigned ft);
void sscenc_ec_encode_bin(sscenc_ec *e, unsigned fl, unsigned fh, unsigned bits);
void sscenc_ec_bit_logp(sscenc_ec *e, int val, unsigned logp);
void sscenc_ec_icdf(sscenc_ec *e, int s, const unsigned char *icdf, unsigned ftb);
void sscenc_ec_uint(sscenc_ec *e, uint32_t fl, uint32_t ft);
void sscenc_ec_bits(sscenc_ec *e, uint32_t fl, unsigned bits);
void sscenc_ec_done(sscenc_ec *e);
void sscenc_laplace_encode(sscenc_ec *e, int *value, unsigned fs, int decay);

/* ---- mode tables (ssc_enc_tables.c) ---- */
typedef struct {
    const short *eBands;                  /* 23 entries, in units of 8 MDCT bins */
    const unsigned char *allocVectors[2]; /* [0]: bitrate <= 192000, [1]: bitrate > 192000 */
    const short *logN;                    /* 22 */
    const short *cache_index;             /* 5 x 22, row = LM + 1 */
    const unsigned char *cache_bits;
    const int *cap;                       /* 22, per-band cap in 1/8 bits (C=2, LM=3) */
} sscenc_mode;

extern const sscenc_mode sscenc_mode_44k;
extern const sscenc_mode sscenc_mode_96k;
extern const float sscenc_window[SSCE_OVERLAP];
extern const float sscenc_eMeans[25];
extern const unsigned char sscenc_log2_frac[24];
extern const unsigned char sscenc_e_prob_model[2][42]; /* [0] inter, [1] intra (LM = 3) */
extern const uint32_t *const sscenc_pvq_u_row[15];

/* ---- forward MDCT (ssc_enc_mdct.c) ---- */
typedef struct { float r, i; } sscenc_cpx;
typedef struct {
    sscenc_cpx tw_long[432], tw_short[54];   /* exp(-2 pi i k / n) */
    sscenc_cpx pre_long[432], post_long[432];
    sscenc_cpx pre_short[54], post_short[54];
    float win_long[2 * SSCE_N];               /* 1728-point window of the long block */
    float win_short[2 * SSCE_SHORT];
} sscenc_mdct;

void sscenc_mdct_init(sscenc_mdct *m);
/* in: SSCE_N + SSCE_OVERLAP samples (108 old + 864 new). out: 864 coefficients in 16-bit PCM units,
 * either one long block or 8 interleaved short blocks (out[j*8+b]). */
void sscenc_mdct_frame(const sscenc_mdct *m, const float *in, int shortBlocks, float *out);

/* ---- energy quantisation (ssc_enc_energy.c), Q10 integers ---- */
int sscenc_quant_coarse_energy(int end, const int16_t *x, int16_t *oldE, int32_t budget, int nbAvailableBytes,
                               int16_t *error, sscenc_ec *enc);
void sscenc_quant_fine_energy(int end, int16_t *oldE, int16_t *error, const int *fine_quant, sscenc_ec *enc);
void sscenc_quant_energy_finalise(int end, int16_t *oldE, int16_t *error, const int *fine_quant,
                                  const int *fine_priority, int bits_left, sscenc_ec *enc);

/* ---- bit allocation (ssc_enc_alloc.c) ---- */
int sscenc_compute_allocation(const sscenc_mode *m, int end, const int *offsets, int alloc_trim, int hi_rate,
                              int32_t total, int32_t *balance, int *pulses, int *ebits, int *fine_priority,
                              int *intensity, int *dual_stereo, int prev_coded, int signal_bw, sscenc_ec *ec);

/* ---- band shapes (ssc_enc_bands.c) ---- */
void sscenc_quant_all_bands(const sscenc_mode *m, int end, float *X, float *Y, const float *bandE,
                            const int *pulses, int shortBlocks, int spread, int dual_stereo, int intensity,
                            const int *tf_res, int32_t total_bits, int32_t balance, sscenc_ec *ec, int codedBands);
void sscenc_haar1(float *X, int N0, int stride);
float sscenc_cos_half_pi(float x);

/* ---- analysis (ssc_enc_analysis.c) ---- */
int sscenc_transient_analysis(const float *in, int len, float *tf_estimate, int *tf_chan);
int sscenc_patch_transient_decision(const float *newE, const float *oldE, int end);
int sscenc_tf_analysis(const sscenc_mode *m, int len, int isTransient, int *tf_res, int lambda, const float *X,
                       float tf_estimate, int tf_chan);
int sscenc_spreading_decision(const sscenc_mode *m, const float *X, int *average, int last_decision, int end);
void sscenc_dynalloc_analysis(const sscenc_mode *m, const float *bandLogE, const float *bandLogE2, int end,
                              int *offsets, int lsb_depth, int isTransient, int effectiveBytes);
int sscenc_alloc_trim_analysis(const sscenc_mode *m, const float *X, const float *bandLogE, int end,
                               float *stereo_saving, float tf_estimate, int intensity);
int sscenc_stereo_analysis(const sscenc_mode *m, const float *X);

extern const signed char sscenc_tf_select_table[8];

/* ---- pitch prefilter (ssc_enc_pitch.c) ---- */
#define SSCE_PF_MINPERIOD 15
#define SSCE_PF_MAXPERIOD 1024
void sscenc_pitch_downsample(const float *x0, const float *x1, float *x_lp, int len);
void sscenc_pitch_search(const float *x_lp, const float *y, int len, int max_pitch, int *pitch);
float sscenc_remove_doubling(const float *x, int maxperiod, int minperiod, int N, int *T0_, int prev_period,
                             float prev_gain);
void sscenc_comb_filter(float *y, const float *x, int T0, int T1, int N, float g0, float g1, int overlap);

#endif
