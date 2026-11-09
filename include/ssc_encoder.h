/* Open-source encoder for the Samsung Scalable Codec / Samsung Seamless Codec (SSC), the Bluetooth A2DP
 * vendor codec (vendor 0x00000075, codec 0x0103) used by Galaxy phones and Galaxy Buds.
 *
 * Copyright (c) 2026 WillyBilly06. BSD-3-Clause, see LICENSE.
 *
 * One SSC frame carries 864 stereo samples: a 4-byte header (FF Ex, sequence, bitrate code) followed by a
 * range-coded CELT-style payload. The frame size is fixed by the bitrate and the codec mode; see
 * ssc_encoder_frame_bytes(). There is no length field on the wire, so a receiver derives it from the header.
 *
 * Codec modes: 44100 and 48000 Hz share one mode (the sample rate is not signalled), 96000 Hz has its own.
 * Bitrates (bits per second, or the header rate code):
 *   F-family: 88000 (0xF0), 96000 (0xF1), 128000 (0xF2), 192000 (0xF3), 229000 (0xF4), 328000 (0xF5),
 *             256000 (0xF6)
 *   E-family: 291000 (0xE0), 308000 (0xE1), 442000 (0xE2), 584000 (0xE3), 886000 (0xE4), 250000 (0xE5),
 *             152000 (0xE6)
 * The encoder is not thread-safe per instance; separate instances are independent. No static state. */
#ifndef SSC_ENCODER_H
#define SSC_ENCODER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSC_ENC_FRAME_SAMPLES 864  /* samples per channel per frame */
#define SSC_ENC_MAX_FRAME_BYTES 2172 /* largest frame: 886 kb/s in the 44.1/48 kHz mode */

/* Error codes (negative return values). */
#define SSC_ENC_ERR_ARG     -1 /* bad argument */
#define SSC_ENC_ERR_BITRATE -2 /* unsupported bitrate */
#define SSC_ENC_ERR_BUFFER  -3 /* output buffer too small */
#define SSC_ENC_ERR_INTERNAL -4

/* Flags for ssc_encode*(). */
/* Reproduce the level of Samsung's encoder for 16-bit input at the "hq" rates (250k, 256k, 291k, 308k,
 * 328k, 442k, 584k, 886k): it scales such input as if it were 24-bit, so the coded signal is 48 dB quieter
 * and the end-band side information differs. Only useful to compare against Samsung's output. */
#define SSC_ENC_FLAG_SAMSUNG_HQ16_LEVEL 0x1u
/* Disable the transient detector (always long blocks). */
#define SSC_ENC_FLAG_NO_TRANSIENTS 0x2u
/* Disable the pitch prefilter (the frame then signals "postfilter off"). */
#define SSC_ENC_FLAG_NO_POSTFILTER 0x4u
/* Do not apply Samsung's fixed high-frequency shaping (up to -1.7 dB above ~6-16 kHz, depending on the rate).
 * The default applies it, like Samsung's encoder; it slightly lowers the waveform error of the noise-filled
 * top bands. The header side information is computed the same way either way. */
#define SSC_ENC_FLAG_FLAT_RESPONSE 0x8u

typedef struct ssc_encoder ssc_encoder;

/* Per-frame information about the last encoded frame. */
typedef struct {
    int bytes;          /* total frame size, header included */
    int bitrate;        /* bits per second */
    int hq;             /* "hq" rate class (E-family, 256k, 328k): no TF symbols, 24-bit scale */
    int seq;            /* sequence number written into the header (0..255) */
    int end_band;       /* number of coded bands (18..22) */
    int transient;      /* short blocks used */
    int pf_on;          /* pitch postfilter enabled */
    int pf_period;      /* postfilter period in samples (15..1022), if pf_on */
    int pf_gain_q;      /* postfilter gain index 0..7 (gain = 0.09375 * (q + 1)), if pf_on */
    int silence;        /* silence frame */
    int coded_bands;    /* bands that received PVQ bits */
    int tell_bits;      /* final range-coder position in bits (ec_tell before ec_enc_done) */
    int total_bits;     /* bit budget 8 * min(payload, 1275) */
} ssc_encoder_frame_info;

typedef void *(*ssc_enc_alloc_fn)(void *ctx, unsigned size);
typedef void (*ssc_enc_free_fn)(void *ctx, void *ptr);

/* sample_rate: 44100, 48000 or 96000. Returns NULL on a bad rate or out of memory. */
ssc_encoder *ssc_encoder_create(int sample_rate);
ssc_encoder *ssc_encoder_create_with_allocator(int sample_rate, ssc_enc_alloc_fn alloc, ssc_enc_free_fn release,
                                               void *ctx);
void ssc_encoder_destroy(ssc_encoder *enc);
/* Clears all history and restarts the sequence counter at 0. */
void ssc_encoder_reset(ssc_encoder *enc);
/* Bytes allocated by one encoder instance. */
int ssc_encoder_size(void);

/* Frame size in bytes (header included) for a bitrate (bps or rate code) in a codec mode
 * (sample_rate 44100/48000 or 96000). Returns 0 for an unsupported bitrate. */
int ssc_encoder_frame_bytes(int sample_rate, int bitrate);
/* Normalises a bitrate given as bps or as a rate code (0xF0..0xF6, 0xE0..0xE6) to bps; 0 if unsupported. */
int ssc_encoder_bitrate(int bitrate_or_code);

/* Encodes one frame of 864 interleaved stereo int16 samples (L R L R ...).
 * bitrate: bps or rate code; it may change from frame to frame. flags: SSC_ENC_FLAG_*.
 * Returns the number of bytes written to out (the full frame), or a negative error code. */
int ssc_encode(ssc_encoder *enc, const int16_t *pcm, int bitrate, unsigned flags, uint8_t *out, int max_bytes);

/* Same, for wider samples: interleaved stereo int32, right-justified with bits_per_sample significant bits
 * (16..32; 24 for HiFi / UHQ input, i.e. full scale +-2^23). */
int ssc_encode_s32(ssc_encoder *enc, const int32_t *pcm, int bits_per_sample, int bitrate, unsigned flags,
                   uint8_t *out, int max_bytes);

/* Same, float samples in [-1, 1). */
int ssc_encode_float(ssc_encoder *enc, const float *pcm, int bitrate, unsigned flags, uint8_t *out,
                     int max_bytes);

/* Details of the last frame (returns 0, or -1 if no frame was encoded yet). */
int ssc_encoder_last_frame_info(const ssc_encoder *enc, ssc_encoder_frame_info *info);

#ifdef __cplusplus
}
#endif
#endif
