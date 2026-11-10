# Encoder design

The bitstream is CELT 1.1 with fixed parameters: LM = 3 (one 1728-point MDCT, or eight 216-point ones for
transients), 108-sample overlap, 22 bands, two coded channels, a 24-bit range coder and no pre-emphasis. On
top of that come the SSC rules: the bit budget is `8*min(payload, 1275)`, the postfilter has no tapset symbol,
the hq rates code no TF symbols, the end band comes from the header, the 18→21 dynalloc propagation, the
allocation table switches above 192 kb/s, and the bands above the end band are reset after each frame.

The encoder decisions are the CELT 1.1 ones, adapted:

- **Pitch prefilter** (pitch search, octave check, comb filter with tapset 0, crossfaded over the overlap).
  The decoder's postfilter inverts it. This matters for tonal content. It is worth about 5 dB SNR on pure tones.
- **Transients**: time-domain detector plus the band-energy "patch" check. Short blocks and anti-collapse.
- **Energy**: two-pass intra/inter coarse quantisation, fine and final energy bits. Integer Q10 arithmetic
  identical to the decoder's, so the prediction state never drifts.
- **TF resolution** (Viterbi over an L1 sparsity metric, non-hq rates), **spreading** decision,
  **dynamic allocation** (CBR limits), **allocation trim**, band-skip hysteresis, **dual stereo** vs mid/side.
  Intensity stereo is not used (Samsung never uses it either).
- **PVQ**: pyramid projection plus greedy pulse search, spreading rotation, recursive band splitting with
  theta quantisation.
- **End-band side information** (header byte 2 bits 2-3 and byte 3 bits 5-7) and the band-21 zeroing below
  192 kb/s, computed like Samsung's encoder. Samsung's fixed high-frequency shaping (at most -1.7 dB above
  about 6-16 kHz, depending on the rate class) is applied by default. `SSC_ENC_FLAG_FLAT_RESPONSE` disables
  it. It lowers the waveform error of the noise-filled top bands. Without it the SNR is up to 0.5 dB lower at
  229-328 kb/s and about the same elsewhere.

The bitstream syntax only depends on integer state, so the output stays decodable whatever the compiler or
floating-point mode. The test suite also passes with `-ffast-math`.
