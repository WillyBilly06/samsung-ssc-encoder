/* SSC encoder: frame header, rate table and the per-frame encode flow.
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 * The frame flow follows libopus 1.1 celt_encode_with_ec (Copyright 2007-2010 Xiph.Org Foundation,
 * 2008 Gregory Maxwell, 2007-2008 CSIRO, BSD-3-Clause, see LICENSE), with the SSC bitstream rules:
 *
 *   - frame = 4-byte header + range-coded payload; LM = 3 (864 samples, 8 short blocks of 108), 22 bands,
 *     always two coded channels, start band 0, no pre-emphasis;
 *   - total_bits = 8 * min(payload, 1275) for every budget check except TF, which uses the full payload;
 *   - the pitch postfilter has no tapset symbol (tapset 0 only);
 *   - "hq" rates (E-family, 256k, 328k) code no TF symbols (tf_res = 0);
 *   - the coded end band comes from the header (22, 21..19 for the E-family, 18 for 152k);
 *   - dynalloc quanta/caps for C=2, LM=3, then the bands 18..21 boost propagation;
 *   - allocation vector table selected by bitrate > 192000;
 *   - bands at or above the end band are reset to -28 after the frame.
 *
 * Sample scale: the codec works in 16-bit PCM units. 24-bit input is divided by 256. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ssc_encoder.h"
#include "ssc_enc_internal.h"

#define FRAME   SSCE_N
#define INLEN   (SSCE_N + SSCE_OVERLAP)
#define NEG28   ((int16_t)(-28 * (1 << SSCE_DB_SHIFT)))

static const unsigned char spread_icdf[4] = {25, 23, 2, 0};
static const unsigned char trim_icdf[11] = {126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};

/* ---- rate table ---- */
typedef struct {
    int bps;
    unsigned char code; /* header byte 3 (low 5 bits identify the rate) */
    unsigned char hq;
    unsigned char efam; /* E-family: 8-bit sequence, end-band side information */
} rate_info;

static const rate_info rates[14] = {
    {88000, 0xF0, 0, 0},  {96000, 0xF1, 0, 0},  {128000, 0xF2, 0, 0}, {192000, 0xF3, 0, 0},
    {229000, 0xF4, 0, 0}, {328000, 0xF5, 1, 0}, {256000, 0xF6, 1, 0}, {291000, 0xE0, 1, 1},
    {308000, 0xE1, 1, 1}, {442000, 0xE2, 1, 1}, {584000, 0xE3, 1, 1}, {886000, 0xE4, 1, 1},
    {250000, 0xE5, 1, 1}, {152000, 0xE6, 1, 1},
};

static const rate_info *find_rate(int bitrate) {
    int i;
    for (i = 0; i < 14; i++)
        if (rates[i].bps == bitrate || (bitrate < 256 && rates[i].code == bitrate)) return &rates[i];
    return NULL;
}

int ssc_encoder_bitrate(int bitrate_or_code) {
    const rate_info *r = find_rate(bitrate_or_code);
    return r ? r->bps : 0;
}

/* Range-coded payload size: base = bitrate*864/(Fs*8), +3/128 for non-hq rates, forced odd, payload = base-1.
 * Fs is 44100 for the 44.1/48 kHz mode. */
static int payload_bytes(int is96k, const rate_info *r) {
    int fs = is96k ? 96000 : 44100;
    int base = (int)(((int64_t)r->bps * FRAME) / ((int64_t)fs * 8));
    if (!r->hq) base += base * 3 >> 7;
    base |= 1;
    return base - 1;
}

int ssc_encoder_frame_bytes(int sample_rate, int bitrate) {
    const rate_info *r = find_rate(bitrate);
    if (!r || (sample_rate != 44100 && sample_rate != 48000 && sample_rate != 96000)) return 0;
    return payload_bytes(sample_rate == 96000, r) + 4;
}

/* ---- state ---- */
typedef struct {
    float in[SSCE_C][INLEN];       /* [108 history | 864 new] per channel */
    float freq[SSCE_C * FRAME];    /* MDCT, channel-major */
    float X[SSCE_C * FRAME];       /* normalised spectrum */
    float bandE[SSCE_C * SSCE_NB];
    float bandLogE[SSCE_C * SSCE_NB], bandLogE2[SSCE_C * SSCE_NB];
    float pre[SSCE_C][SSCE_PF_MAXPERIOD + FRAME]; /* unfiltered input with pitch history */
    float pitch_buf[(SSCE_PF_MAXPERIOD + FRAME) / 2];
} work_t;

struct ssc_encoder {
    int sample_rate, is96k;
    const sscenc_mode *mode;
    sscenc_mdct *mdct;
    work_t *w;
    float in_mem[SSCE_C][SSCE_OVERLAP];             /* prefiltered input history */
    float prefilter_mem[SSCE_C][SSCE_PF_MAXPERIOD]; /* unfiltered input history */
    int prefilter_period;
    float prefilter_gain;
    float overlap_max;
    int16_t oldBandE[SSCE_C * SSCE_NB];
    int seq;
    int spread_decision, tonal_average;
    int consec_transient;
    int lastCodedBands;
    float stereo_saving;
    int have_info;
    ssc_encoder_frame_info info;
    ssc_enc_alloc_fn alloc;
    ssc_enc_free_fn release;
    void *alloc_ctx;
};

int ssc_encoder_size(void) { return (int)(sizeof(ssc_encoder) + sizeof(sscenc_mdct) + sizeof(work_t)); }

void ssc_encoder_reset(ssc_encoder *st) {
    int i;
    if (!st) return;
    memset(st->in_mem, 0, sizeof st->in_mem);
    memset(st->prefilter_mem, 0, sizeof st->prefilter_mem);
    st->prefilter_period = SSCE_PF_MINPERIOD;
    st->prefilter_gain = 0;
    st->overlap_max = 0;
    for (i = 0; i < SSCE_C * SSCE_NB; i++) st->oldBandE[i] = 0;
    st->seq = 0;
    st->spread_decision = SSCE_SPREAD_NORMAL;
    st->tonal_average = 256;
    st->consec_transient = 0;
    st->lastCodedBands = 0;
    st->stereo_saving = 0;
    st->have_info = 0;
}

static void *default_alloc(void *ctx, unsigned size) {
    (void)ctx;
    return malloc(size);
}

static void default_free(void *ctx, void *p) {
    (void)ctx;
    free(p);
}

void ssc_encoder_destroy(ssc_encoder *st) {
    if (!st) return;
    if (st->w) st->release(st->alloc_ctx, st->w);
    if (st->mdct) st->release(st->alloc_ctx, st->mdct);
    st->release(st->alloc_ctx, st);
}

ssc_encoder *ssc_encoder_create_with_allocator(int sample_rate, ssc_enc_alloc_fn alloc, ssc_enc_free_fn release,
                                               void *ctx) {
    ssc_encoder *st;
    if (sample_rate != 44100 && sample_rate != 48000 && sample_rate != 96000) return NULL;
    if (!alloc || !release) return NULL;
    st = (ssc_encoder *)alloc(ctx, sizeof(ssc_encoder));
    if (!st) return NULL;
    memset(st, 0, sizeof *st);
    st->alloc = alloc;
    st->release = release;
    st->alloc_ctx = ctx;
    st->mdct = (sscenc_mdct *)alloc(ctx, sizeof(sscenc_mdct));
    st->w = (work_t *)alloc(ctx, sizeof(work_t));
    if (!st->mdct || !st->w) {
        ssc_encoder_destroy(st);
        return NULL;
    }
    st->sample_rate = sample_rate;
    st->is96k = sample_rate == 96000;
    st->mode = st->is96k ? &sscenc_mode_96k : &sscenc_mode_44k;
    sscenc_mdct_init(st->mdct);
    ssc_encoder_reset(st);
    return st;
}

ssc_encoder *ssc_encoder_create(int sample_rate) {
    return ssc_encoder_create_with_allocator(sample_rate, default_alloc, default_free, NULL);
}

int ssc_encoder_last_frame_info(const ssc_encoder *st, ssc_encoder_frame_info *info) {
    if (!st || !info || !st->have_info) return -1;
    *info = st->info;
    return 0;
}

/* ---- analysis helpers ---- */
static void compute_mdcts(ssc_encoder *st, int shortBlocks) {
    int c;
    for (c = 0; c < SSCE_C; c++) sscenc_mdct_frame(st->mdct, st->w->in[c], shortBlocks, st->w->freq + c * FRAME);
}

/* High-frequency spectral shaping ("headroom"): a fixed gain per MDCT bin, at most -1.7 dB and only above
 * ~6-16 kHz depending on the rate class. Samsung's encoder applies it to every frame; this encoder does the same
 * by default (SSC_ENC_FLAG_FLAT_RESPONSE turns it off). Values are Q15. */
static const short headroom_tab[240] = {
    31308, 31235, 31162, 31089, 31016, 30943, 30870, 30797, 30744, 30691, 30638, 30585, 30532, 30479, 30426,
    30373, 30320, 30267, 30234, 30201, 30168, 30135, 30102, 30069, 30036, 30003, 29970, 29937, 29904, 29871,
    29838, 29805, 29786, 29767, 29748, 29729, 29710, 29691, 29672, 29653, 29634, 29615, 29596, 29577, 29558,
    29539, 29520, 29501, 29482, 29469, 29456, 29443, 29430, 29419, 29412, 29405, 29398, 29391, 29384, 29377,
    29370, 29363, 29356, 29349, 29342, 29335, 29328, 29321, 29314, 29307, 29300, 29293, 29286, 29279, 29272,
    29265, 29258, 29251, 29244, 29237, 29230, 29223, 29216, 29209, 29202, 29195, 29188, 29181, 29174, 29167,
    29160, 29153, 29146, 29139, 29132, 29125, 29110, 29095, 29081, 29066, 29051, 29036, 29021, 29007, 28992,
    28977, 28962, 28947, 28933, 28918, 28903, 28888, 28873, 28859, 28844, 28829, 28814, 28799, 28785, 28770,
    28755, 28740, 28725, 28711, 28696, 28681, 28666, 28651, 28637, 28622, 28607, 28592, 28577, 28563, 28548,
    28533, 28518, 28503, 28489, 28474, 28459, 28444, 28429, 28415, 28400, 28385, 28370, 28355, 28341, 28326,
    28311, 28296, 28281, 28267, 28252, 28237, 28222, 28207, 28193, 28178, 28163, 28148, 28133, 28119, 28104,
    28089, 28074, 28059, 28045, 28030, 28015, 28000, 27985, 27971, 27956, 27941, 27926, 27911, 27897, 27882,
    27867, 27852, 27837, 27823, 27808, 27793, 27778, 27763, 27749, 27734, 27719, 27704, 27689, 27675, 27660,
    27645, 27630, 27615, 27601, 27586, 27571, 27556, 27541, 27527, 27512, 27497, 27482, 27467, 27453, 27438,
    27423, 27408, 27393, 27379, 27364, 27349, 27334, 27319, 27305, 27290, 27275, 27260, 27245, 27231, 27216,
    27201, 27186, 27171, 27157, 27142, 27127, 27112, 27097, 27083, 27068, 27053, 27038, 27023, 27009, 26990,
};

static float headroom_gain(const sscenc_mode *mode, int is96k, int bitrate, int hq, int j) {
    const short *eb = mode->eBands;
    int q = 32768;
    if (!hq) {
        if (bitrate >= 96000) {
            int b0 = eb[20] * 8, lo = b0 + 68, hi = eb[22] * 8;
            if (j >= lo && j < hi && j - b0 < 240) q = headroom_tab[j - b0];
        } else {
            int i0 = eb[17] * 8, i1 = eb[20] * 8 - 15, i2 = eb[22] * 8;
            if (j >= i0 && j < i1) q = 32767 - 3 * (j - i0);
            else if (j >= i1 && j < i2) q = 0x7767 - 3 * (i1 - i0) - 5 * (j - i1);
        }
    } else if (!is96k) {
        if (bitrate < 270000) {
            if (j >= 350 && j < 384) q = 30620;
            else if (j >= 384 && j < 409) q = 28893;
            else if (j >= 409) q = 30620;
        } else {
            if (j >= 354 && j < 384) q = 30720;
            else if (j >= 384 && j < 400) q = 28988;
            else if (j >= 400 && j < 409) q = 28893;
            else if (j >= 409) q = 30620;
        }
    } else {
        if (bitrate < 270000) {
            if (j >= 288 && j < 313) q = 30920;
            else if (j >= 313 && j < 350) q = 30750;
            else if (j >= 350) q = 30620;
        } else {
            if (j >= 288 && j < 313) q = 30920;
            else if (j >= 313 && j < 354) q = 30820;
            else if (j >= 354 && j < 400) q = 30720;
            else if (j >= 400) q = 30620;
        }
    }
    return (float)q * (1.f / 32768.f);
}

static float bin_gain(const ssc_encoder *st, const rate_info *r, int j) {
    return headroom_gain(st->mode, st->is96k, r->bps, r->hq, j);
}

/* Spectrum shaping after each MDCT: the headroom gain (unless flat), and for bitrates below 192 kb/s nothing is
 * coded above eBands[21] (band 21 itself is still coded, empty). Same bin index for long and interleaved short
 * spectra. */
static void shape_spectrum(ssc_encoder *st, const rate_info *r, int shortBlocks, int flat) {
    int c, j, start = st->mode->eBands[21] * 8;
    if (!flat)
        for (j = 0; j < FRAME; j++) {
            float g = bin_gain(st, r, j);
            if (g != 1.f)
                for (c = 0; c < SSCE_C; c++) st->w->freq[c * FRAME + j] *= g;
        }
    if (r->bps >= 192000 || (shortBlocks && r->hq)) return;
    for (c = 0; c < SSCE_C; c++)
        for (j = start; j < FRAME; j++) st->w->freq[c * FRAME + j] = 0.f;
}

static void compute_band_energies(ssc_encoder *st) {
    const short *eBands = st->mode->eBands;
    int c, i, j;
    for (c = 0; c < SSCE_C; c++)
        for (i = 0; i < SSCE_NB; i++) {
            const float *x = st->w->freq + c * FRAME;
            float sum = 1e-27f, e;
            for (j = eBands[i] * 8; j < eBands[i + 1] * 8; j++) sum += x[j] * x[j];
            e = sqrtf(sum);
            st->w->bandE[c * SSCE_NB + i] = e;
            e = log2f(e) - sscenc_eMeans[i];
            st->w->bandLogE[c * SSCE_NB + i] = e < -28.f ? -28.f : e;
        }
}

static void normalise_bands(ssc_encoder *st) {
    const short *eBands = st->mode->eBands;
    int c, i, j;
    memset(st->w->X, 0, sizeof st->w->X);
    for (c = 0; c < SSCE_C; c++)
        for (i = 0; i < SSCE_NB; i++) {
            float g = 1.f / (1e-15f + st->w->bandE[c * SSCE_NB + i]);
            for (j = eBands[i] * 8; j < eBands[i + 1] * 8; j++) st->w->X[c * FRAME + j] = st->w->freq[c * FRAME + j] * g;
        }
}

/* L2 norm of bin ranges [prev, parts[k]) of both channels, in the scale of Samsung's fixed-point spectrum
 * (16-bit PCM unit = 4096), as used for the end-band side information. E[c*n + k]. The spectrum is measured with
 * the headroom gain applied (done here if the coded spectrum is flat). */
static void part_energies(const ssc_encoder *st, const rate_info *r, int flat, int start, const short *parts, int n,
                          float *E) {
    const float *freq = st->w->freq;
    int c, k, j;
    for (c = 0; c < SSCE_C; c++) {
        int prev = start;
        for (k = 0; k < n; k++) {
            float s = 0;
            for (j = prev; j < parts[k]; j++) {
                float v = freq[c * FRAME + j] * (flat ? bin_gain(st, r, j) : 1.f);
                s += v * v;
            }
            E[c * n + k] = 4096.f * sqrtf(s) + 1.f;
            if (parts[k] > prev) prev = parts[k];
        }
    }
}

/* Header side information. Returns `red` (number of uncoded top bands); writes the 2-bit field for byte 2
 * and the top 3 bits of byte 3. */
static int end_band_info(ssc_encoder *st, const rate_info *r, int flat, int *b2_field, int *b3_top) {
    static const short parts_nonhq[4] = {708, 744, 780, 816};
    static const short parts_e[4][8] = {
        {738, 756, 774, 792, 810, 828, 846, 864},
        {594, 612, 630, 648, 666, 684, 702, 720},
        {450, 468, 486, 504, 522, 540, 558, 576},
        {306, 324, 342, 360, 378, 396, 414, 432},
    };
    float E[16];
    *b2_field = 0;
    *b3_top = 0;
    if (!r->hq) {
        /* last-band activity of band 21 (side information only, nothing in the payload depends on it) */
        int lb = 0;
        part_energies(st, r, flat, st->mode->eBands[21] * 8, parts_nonhq, 4, E);
        if (E[3] + E[7] >= 30002.f) lb = 3;
        else if (E[2] + E[6] >= 30002.f) lb = 2;
        else if (E[1] + E[5] >= 30002.f) lb = 1;
        *b2_field = lb;
        return 0;
    }
    if (r->code == 0xE6) return 4; /* 152k: 18 bands, nothing signalled */
    if (r->efam) {
        int red, k;
        for (red = 0; red < 4; red++) {
            part_energies(st, r, flat, st->mode->eBands[21 - red] * 8, parts_e[red], 8, E);
            for (k = 7; k >= 0; k--)
                if (E[k] + E[8 + k] > 12001.f) {
                    *b2_field = red;
                    *b3_top = k << 5; /* (sub - 1) << 5 with sub = k + 1 */
                    return red;
                }
        }
        *b2_field = 3;
        return 3;
    }
    return 0; /* 256k, 328k */
}

/* Pitch prefilter: finds a pitch period and gain, and filters the new input samples in place with the comb
 * filter x - g*comb(x) (the decoder's postfilter is the inverse IIR). The filter crossfades from the previous
 * frame's parameters over the first 108 samples. Returns 1 if the postfilter is signalled on. */
static int run_prefilter(ssc_encoder *st, int enabled, int nbAvail, int *pitch_out, int *qg_out) {
    work_t *w = st->w;
    int c, pitch_index = SSCE_PF_MINPERIOD, pf_on, qg = 0;
    float gain1 = 0, pf_threshold;
    for (c = 0; c < SSCE_C; c++) {
        memcpy(w->pre[c], st->prefilter_mem[c], sizeof(float) * SSCE_PF_MAXPERIOD);
        memcpy(w->pre[c] + SSCE_PF_MAXPERIOD, w->in[c] + SSCE_OVERLAP, sizeof(float) * FRAME);
    }
    if (enabled) {
        sscenc_pitch_downsample(w->pre[0], w->pre[1], w->pitch_buf, SSCE_PF_MAXPERIOD + FRAME);
        /* the last 1.5 octaves of the range are skipped (false positives from short-term correlation) */
        sscenc_pitch_search(w->pitch_buf + (SSCE_PF_MAXPERIOD >> 1), w->pitch_buf, FRAME,
                            SSCE_PF_MAXPERIOD - 3 * SSCE_PF_MINPERIOD, &pitch_index);
        pitch_index = SSCE_PF_MAXPERIOD - pitch_index;
        gain1 = sscenc_remove_doubling(w->pitch_buf, SSCE_PF_MAXPERIOD, SSCE_PF_MINPERIOD, FRAME, &pitch_index,
                                       st->prefilter_period, st->prefilter_gain);
        if (pitch_index > SSCE_PF_MAXPERIOD - 2) pitch_index = SSCE_PF_MAXPERIOD - 2;
        gain1 *= .7f;
    }
    /* gain threshold, adjusted for continuity and rate */
    pf_threshold = .2f;
    if (abs(pitch_index - st->prefilter_period) * 10 > pitch_index) pf_threshold += .2f;
    if (nbAvail < 25) pf_threshold += .1f;
    if (nbAvail < 35) pf_threshold += .1f;
    if (st->prefilter_gain > .4f) pf_threshold -= .1f;
    if (st->prefilter_gain > .55f) pf_threshold -= .1f;
    if (pf_threshold < .2f) pf_threshold = .2f;
    if (gain1 < pf_threshold) {
        gain1 = 0;
        pf_on = 0;
    } else {
        if (fabsf(gain1 - st->prefilter_gain) < .1f) gain1 = st->prefilter_gain;
        qg = (int)floorf(.5f + gain1 * 32 / 3) - 1;
        qg = sscenc_imax(0, sscenc_imin(7, qg));
        gain1 = .09375f * (qg + 1);
        pf_on = 1;
    }
    if (st->prefilter_period < SSCE_PF_MINPERIOD) st->prefilter_period = SSCE_PF_MINPERIOD;
    for (c = 0; c < SSCE_C; c++) {
        sscenc_comb_filter(w->in[c] + SSCE_OVERLAP, w->pre[c] + SSCE_PF_MAXPERIOD, st->prefilter_period, pitch_index,
                           FRAME, -st->prefilter_gain, -gain1, SSCE_OVERLAP);
        memcpy(st->prefilter_mem[c], w->pre[c] + FRAME, sizeof(float) * SSCE_PF_MAXPERIOD);
    }
    st->prefilter_period = pitch_index;
    st->prefilter_gain = gain1;
    *pitch_out = pitch_index;
    *qg_out = qg;
    return pf_on;
}

/* ---- one frame ---- */
static int encode_frame(ssc_encoder *st, const float *pcm, float scale, int lsb_depth, int bitrate_in,
                        unsigned flags, uint8_t *out, int max_bytes) {
    const sscenc_mode *mode = st->mode;
    const short *eBands = mode->eBands;
    work_t *w = st->w;
    const rate_info *r = find_rate(bitrate_in);
    sscenc_ec enc;
    int payload, frame_len, nbAvail, total_bits, i, c, end, red, b2_field, b3_top;
    int silence, isTransient, shortBlocks, transient_got_disabled = 0, tf_chan = 0, tf_select = 0, spread;
    int tf_res[SSCE_NB], offsets[SSCE_NB], req[SSCE_NB], pulses[SSCE_NB], fine_quant[SSCE_NB],
        fine_priority[SSCE_NB];
    int16_t x16[SSCE_C * SSCE_NB], error[SSCE_C * SSCE_NB];
    int32_t total_bits8, total_boost, tell, bits, balance;
    int dynalloc_logp, alloc_trim, anti_collapse_rsv, codedBands, intensity, dual_stereo;
    float tf_estimate = 0, sample_max;
    int pf_on, pf_pitch, pf_qg;
    const int flat = (flags & SSC_ENC_FLAG_FLAT_RESPONSE) != 0;

    if (!r) return SSC_ENC_ERR_BITRATE;
    payload = payload_bytes(st->is96k, r);
    frame_len = payload + 4;
    if (max_bytes < frame_len) return SSC_ENC_ERR_BUFFER;
    nbAvail = payload < 1275 ? payload : 1275;
    total_bits = 8 * nbAvail;

    /* input: [previous 108 | new 864] */
    sample_max = st->overlap_max;
    for (c = 0; c < SSCE_C; c++) {
        memcpy(w->in[c], st->in_mem[c], sizeof(float) * SSCE_OVERLAP);
        for (i = 0; i < FRAME; i++) w->in[c][SSCE_OVERLAP + i] = pcm[2 * i + c] * scale;
    }
    st->overlap_max = 0;
    for (c = 0; c < SSCE_C; c++)
        for (i = 0; i < FRAME; i++) {
            float a = fabsf(w->in[c][SSCE_OVERLAP + i]);
            if (i >= FRAME - SSCE_OVERLAP) {
                if (a > st->overlap_max) st->overlap_max = a;
            } else if (a > sample_max) {
                sample_max = a;
            }
        }
    if (st->overlap_max > sample_max) sample_max = st->overlap_max;
    silence = sample_max == 0.f;

    sscenc_ec_init(&enc, out + 4, (uint32_t)payload);

    /* silence */
    sscenc_ec_bit_logp(&enc, silence, 15);
    if (silence) enc.nbits_total += total_bits - sscenc_ec_tell(&enc);

    /* pitch prefilter / postfilter parameters (no tapset symbol in SSC) */
    pf_on = run_prefilter(st, !silence && nbAvail >= 25 && !(flags & SSC_ENC_FLAG_NO_POSTFILTER), nbAvail, &pf_pitch,
                          &pf_qg);
    if (!pf_on) {
        if (sscenc_ec_tell(&enc) + 16 <= total_bits) sscenc_ec_bit_logp(&enc, 0, 1);
    } else {
        int octave, p1 = pf_pitch + 1;
        sscenc_ec_bit_logp(&enc, 1, 1);
        octave = sscenc_ilog((uint32_t)p1) - 5;
        sscenc_ec_uint(&enc, (uint32_t)octave, 6);
        sscenc_ec_bits(&enc, (uint32_t)(p1 - (16 << octave)), (unsigned)(4 + octave));
        sscenc_ec_bits(&enc, (uint32_t)pf_qg, 3);
    }

    /* transient decision and MDCT */
    isTransient = 0;
    if (!silence && !(flags & SSC_ENC_FLAG_NO_TRANSIENTS))
        isTransient = sscenc_transient_analysis(&w->in[0][0], INLEN, &tf_estimate, &tf_chan);
    if (sscenc_ec_tell(&enc) + 3 > total_bits) {
        isTransient = 0;
        transient_got_disabled = 1;
    }
    shortBlocks = isTransient ? SSCE_M : 0;
    compute_mdcts(st, 0);
    shape_spectrum(st, r, 0, flat);
    compute_band_energies(st);
    memcpy(w->bandLogE2, w->bandLogE, sizeof w->bandLogE2);
    if (!isTransient && !transient_got_disabled && !silence && !(flags & SSC_ENC_FLAG_NO_TRANSIENTS)) {
        float oldf[SSCE_C * SSCE_NB];
        for (i = 0; i < SSCE_C * SSCE_NB; i++) oldf[i] = st->oldBandE[i] * (1.f / (1 << SSCE_DB_SHIFT));
        if (sscenc_patch_transient_decision(w->bandLogE, oldf, SSCE_NB)) {
            isTransient = 1;
            tf_estimate = .2f;
        }
    }
    if (isTransient) {
        /* bandLogE2 keeps the long-block energies (+1.5 for the short/long scale) for dynalloc */
        for (i = 0; i < SSCE_C * SSCE_NB; i++) w->bandLogE2[i] += .5f * SSCE_LM;
        shortBlocks = SSCE_M;
        compute_mdcts(st, shortBlocks);
        shape_spectrum(st, r, shortBlocks, flat);
        compute_band_energies(st);
    }
    if (sscenc_ec_tell(&enc) + 3 <= total_bits) sscenc_ec_bit_logp(&enc, isTransient, 3);

    /* header */
    red = end_band_info(st, r, flat, &b2_field, &b3_top);
    end = SSCE_NB - red;
    out[0] = 0xFF;
    out[1] = r->efam ? (uint8_t)(0xE0 | ((st->seq >> 4) & 0xF)) : 0xEE;
    out[2] = (uint8_t)(((st->seq & 0xF) << 4) | (b2_field << 2) | 1);
    /* E-family (except 152k): the top 3 bits of byte 3 carry sub-1 of the last active part (0 if none) */
    out[3] = (uint8_t)((r->efam && r->code != 0xE6) ? (r->code & 0x1F) | b3_top : r->code);

    normalise_bands(st);

    /* TF resolution (not coded for hq rates) */
    for (i = 0; i < SSCE_NB; i++) tf_res[i] = isTransient;
    if (!r->hq && nbAvail >= 15 * SSCE_C) {
        int lambda = nbAvail < 40 ? 12 : nbAvail < 60 ? 6 : nbAvail < 100 ? 4 : 3;
        tf_select = sscenc_tf_analysis(mode, SSCE_NB, isTransient, tf_res, 2 * lambda, w->X, tf_estimate, tf_chan);
    }

    /* coarse energy */
    for (i = 0; i < SSCE_C * SSCE_NB; i++) {
        float v = w->bandLogE[i] * (1 << SSCE_DB_SHIFT);
        x16[i] = (int16_t)floorf(v + .5f);
    }
    sscenc_quant_coarse_energy(end, x16, st->oldBandE, total_bits, nbAvail, error, &enc);

    /* TF symbols: budget is the full payload */
    if (!r->hq) {
        uint32_t budget = enc.storage * 8, t = (uint32_t)sscenc_ec_tell(&enc);
        int logp = isTransient ? 2 : 4, curr = 0, tf_changed = 0;
        int tf_select_rsv = t + logp + 1 <= budget;
        budget -= tf_select_rsv;
        for (i = 0; i < SSCE_NB; i++) {
            if (t + logp <= budget) {
                sscenc_ec_bit_logp(&enc, tf_res[i] ^ curr, (unsigned)logp);
                t = (uint32_t)sscenc_ec_tell(&enc);
                curr = tf_res[i];
                tf_changed |= curr;
            } else {
                tf_res[i] = curr;
            }
            logp = isTransient ? 4 : 5;
        }
        if (tf_select_rsv && sscenc_tf_select_table[4 * isTransient + 0 + tf_changed] !=
                                 sscenc_tf_select_table[4 * isTransient + 2 + tf_changed])
            sscenc_ec_bit_logp(&enc, tf_select, 1);
        else
            tf_select = 0;
        for (i = 0; i < SSCE_NB; i++) tf_res[i] = sscenc_tf_select_table[4 * isTransient + 2 * tf_select + tf_res[i]];
    } else {
        for (i = 0; i < SSCE_NB; i++) tf_res[i] = 0;
    }

    /* spreading */
    spread = SSCE_SPREAD_NORMAL;
    if (sscenc_ec_tell(&enc) + 4 <= total_bits) {
        if (!shortBlocks && nbAvail >= 10 * SSCE_C)
            st->spread_decision = spread =
                sscenc_spreading_decision(mode, w->X, &st->tonal_average, st->spread_decision, end);
        else
            st->spread_decision = spread = SSCE_SPREAD_NORMAL;
        sscenc_ec_icdf(&enc, spread, spread_icdf, 5);
    }

    /* dynamic allocation */
    sscenc_dynalloc_analysis(mode, w->bandLogE, w->bandLogE2, end, req, lsb_depth, isTransient, nbAvail);
    dynalloc_logp = 6;
    total_bits8 = total_bits << SSCE_BITRES;
    total_boost = 0;
    tell = (int32_t)sscenc_ec_tell_frac(&enc);
    for (i = 0; i < SSCE_NB; i++) offsets[i] = 0;
    for (i = 0; i < end; i++) {
        int quanta = sscenc_imax(48, (eBands[i + 1] - eBands[i]) << 4), loop_logp = dynalloc_logp, boost = 0, j;
        for (j = 0; tell + (loop_logp << SSCE_BITRES) < total_bits8 - total_boost && boost < mode->cap[i]; j++) {
            int flag = j < req[i];
            sscenc_ec_bit_logp(&enc, flag, (unsigned)loop_logp);
            tell = (int32_t)sscenc_ec_tell_frac(&enc);
            if (!flag) break;
            boost += quanta;
            total_boost += quanta;
            loop_logp = 1;
        }
        if (boost > 0) dynalloc_logp = sscenc_imax(2, dynalloc_logp - 1);
        offsets[i] = boost;
    }
    /* SSC rule: a large boost in band 18/19/20 also applies to the next band, depending on channel 0's
     * coarse energies (applied identically by the decoder, nothing is coded). */
    {
        const int16_t *E = st->oldBandE;
        if (offsets[18] > 1000 && offsets[19] == 0) {
            if (E[19] > 7000 || (E[18] >> 1) < E[19]) offsets[19] = offsets[18];
        } else if (offsets[19] > 1000 && offsets[20] == 0) {
            if (E[20] > 7000 || (E[19] >> 1) < E[20]) offsets[20] = offsets[19];
        } else if (offsets[20] > 1000 && offsets[21] == 0) {
            if (E[21] > 7000 || (E[20] >> 1) < E[21]) offsets[21] = offsets[20];
        }
    }

    /* stereo decisions: mid/side for every band (no intensity), dual stereo from an L1 estimate */
    dual_stereo = sscenc_stereo_analysis(mode, w->X);
    intensity = end;

    /* allocation trim */
    alloc_trim = 5;
    if (tell + (6 << SSCE_BITRES) <= total_bits8 - total_boost) {
        alloc_trim = sscenc_alloc_trim_analysis(mode, w->X, w->bandLogE, end, &st->stereo_saving, tf_estimate,
                                                intensity);
        sscenc_ec_icdf(&enc, alloc_trim, trim_icdf, 7);
    }

    /* bit allocation */
    bits = total_bits8 - (int32_t)sscenc_ec_tell_frac(&enc) - 1;
    anti_collapse_rsv = (isTransient && bits >= ((SSCE_LM + 2) << SSCE_BITRES)) ? (1 << SSCE_BITRES) : 0;
    bits -= anti_collapse_rsv;
    codedBands = sscenc_compute_allocation(mode, end, offsets, alloc_trim, r->bps > 192000, bits, &balance, pulses,
                                           fine_quant, fine_priority, &intensity, &dual_stereo, st->lastCodedBands,
                                           end - 1, &enc);
    if (st->lastCodedBands)
        st->lastCodedBands = sscenc_imin(st->lastCodedBands + 1, sscenc_imax(st->lastCodedBands - 1, codedBands));
    else
        st->lastCodedBands = codedBands;

    sscenc_quant_fine_energy(end, st->oldBandE, error, fine_quant, &enc);
    sscenc_quant_all_bands(mode, end, w->X, w->X + FRAME, w->bandE, pulses, shortBlocks, spread, dual_stereo,
                           intensity, tf_res, total_bits8 - anti_collapse_rsv, balance, &enc, codedBands);
    if (anti_collapse_rsv > 0) sscenc_ec_bits(&enc, st->consec_transient < 2, 1);
    sscenc_quant_energy_finalise(end, st->oldBandE, error, fine_quant, fine_priority,
                                 total_bits - sscenc_ec_tell(&enc), &enc);

    /* end-of-frame state, as in the decoder */
    if (silence)
        for (i = 0; i < SSCE_C * SSCE_NB; i++) st->oldBandE[i] = NEG28;
    for (c = 0; c < SSCE_C; c++)
        for (i = end; i < SSCE_NB; i++) st->oldBandE[c * SSCE_NB + i] = NEG28;
    if (isTransient || transient_got_disabled) st->consec_transient++;
    else st->consec_transient = 0;

    st->info.tell_bits = sscenc_ec_tell(&enc);
    sscenc_ec_done(&enc);
    if (enc.error) return SSC_ENC_ERR_INTERNAL;

    for (c = 0; c < SSCE_C; c++) memcpy(st->in_mem[c], w->in[c] + FRAME, sizeof(float) * SSCE_OVERLAP);

    st->info.bytes = frame_len;
    st->info.bitrate = r->bps;
    st->info.hq = r->hq;
    st->info.seq = st->seq;
    st->info.end_band = end;
    st->info.transient = isTransient;
    st->info.pf_on = pf_on;
    st->info.pf_period = pf_on ? pf_pitch : 0;
    st->info.pf_gain_q = pf_on ? pf_qg : 0;
    st->info.silence = silence;
    st->info.coded_bands = codedBands;
    st->info.total_bits = total_bits;
    st->have_info = 1;
    st->seq = (st->seq + 1) & 0xFF;
    return frame_len;
}

/* Interleaved float staging buffer for the integer front ends. */
#define STAGE_SAMPLES (2 * FRAME)

int ssc_encode(ssc_encoder *st, const int16_t *pcm, int bitrate, unsigned flags, uint8_t *out, int max_bytes) {
    float buf[STAGE_SAMPLES];
    const rate_info *r = find_rate(bitrate);
    float scale = 1.f;
    int i;
    if (!st || !pcm || !out) return SSC_ENC_ERR_ARG;
    if (!r) return SSC_ENC_ERR_BITRATE;
    if ((flags & SSC_ENC_FLAG_SAMSUNG_HQ16_LEVEL) && r->hq) scale = 1.f / 256.f;
    for (i = 0; i < STAGE_SAMPLES; i++) buf[i] = (float)pcm[i];
    return encode_frame(st, buf, scale, 16, bitrate, flags, out, max_bytes);
}

int ssc_encode_s32(ssc_encoder *st, const int32_t *pcm, int bits_per_sample, int bitrate, unsigned flags,
                   uint8_t *out, int max_bytes) {
    float buf[STAGE_SAMPLES];
    int i;
    if (!st || !pcm || !out || bits_per_sample < 16 || bits_per_sample > 32) return SSC_ENC_ERR_ARG;
    for (i = 0; i < STAGE_SAMPLES; i++) buf[i] = (float)pcm[i];
    return encode_frame(st, buf, ldexpf(1.f, 16 - bits_per_sample), bits_per_sample > 24 ? 24 : bits_per_sample,
                        bitrate, flags, out, max_bytes);
}

int ssc_encode_float(ssc_encoder *st, const float *pcm, int bitrate, unsigned flags, uint8_t *out, int max_bytes) {
    if (!st || !pcm || !out) return SSC_ENC_ERR_ARG;
    return encode_frame(st, pcm, 32768.f, 24, bitrate, flags, out, max_bytes);
}
