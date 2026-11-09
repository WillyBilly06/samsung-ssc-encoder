#!/usr/bin/env python3
"""Runs the SSC encoder conformance test over every mode and bitrate.

For each configuration it encodes a raw PCM input with this encoder, decodes the result with the SSC decoder
(tests/decoder) and reports:
  1. tell mismatches: frames where the encoder's final range-coder position differs from the decoder's
     (must be 0: it proves the bitstream is parsed exactly as it was written);
  2. against a reference .ssc produced by Samsung's encoder from the same input (if available):
     frame-size and header mismatches;
  3. SNR (dB) of encoder -> decoder against the input, next to Samsung's frames -> the same decoder.

Inputs and Samsung reference frames are in tests/vectors (inputs at the top, ref_frames/*.ssc).
Point --harness at another directory with the same layout to use other data.
A synthetic stress signal (silence, full-scale noise, clipping, clicks, very quiet noise) is always run at
every mode, bit depth and bitrate, plus a run that switches the bitrate every frame (check 1 only).

usage: python3 tests/run_tests.py [--bin build/ssc_conformance] [--harness DIR] [--markdown out.md]
"""
import argparse
import math
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

F_RATES = [88000, 96000, 128000, 192000, 229000, 256000, 328000]
E_RATES = [152000, 250000, 291000, 308000, 442000, 584000, 886000]
HQ = {256000, 328000} | set(E_RATES)


def configs():
    """(group, name, input, sample_rate, bits, bitrate, samsung_level, mono, reference)"""
    c = []
    for b in F_RATES + E_RATES:
        c.append(('44.1 kHz, 16-bit', 'mix', 'mix.s16le', 44100, 16, b, b in HQ, False, 'mix_%d.ssc' % b))
    for b in F_RATES + E_RATES:
        c.append(('44.1 kHz mode, 24-bit', 'mix', 'mix.s16le', 44100, 24, b, False, False, 'mix24_%d.ssc' % b))
    for b in [88000, 128000, 192000]:
        c.append(('48 kHz, 16-bit', 'mix', 'mix.s16le', 48000, 16, b, False, False,
                  'mix48_192000.ssc' if b == 192000 else None))
    for b in [88000, 128000, 192000]:
        c.append(('48 kHz HiFi, 24-bit', 'mix48k', 'mix48k.s16le', 48000, 24, b, False, False,
                  'hifi48_192000.ssc' if b == 192000 else None))
    c.append(('96 kHz, 16-bit', 'mix', 'mix.s16le', 96000, 16, 192000, False, False, 'mix96_192000.ssc'))
    for b in [152000, 291000, 308000, 328000, 442000, 584000, 886000]:
        c.append(('96 kHz UHQ, 24-bit', 'mix96k', 'mix96k.s16le', 96000, 24, b, False, False,
                  'uhq96_%d.ssc' % b if b in (291000, 584000) else None))
    for b in [88000, 192000, 328000, 886000]:
        c.append(('tones', 'sine', 'sine.s16le', 44100, 16, b, b in HQ, False, 'sine_%d.ssc' % b))
    c.append(('mono input', 'mono', 'mono.s16le', 44100, 16, 192000, False, True, None))
    return c


def make_synthetic(path, rate, seconds=4.0):
    """Deterministic stereo stress signal (s16le): silence, tones and a chirp, full-scale noise with clipping,
    a full-scale square wave, clicks, silence again and very quiet noise."""
    rnd = random.Random(1234)
    n = int(rate * seconds)
    out = bytearray()
    for i in range(n):
        t = i / rate
        seg = int(8 * i / n)
        if seg == 0 or seg == 6:  # digital silence
            left = right = 0.0
        elif seg in (1, 2):  # tones, chirp, noise bursts
            burst = 0.25 if (i // 4410) % 4 == 0 else 0.0
            left = (0.3 * math.sin(2 * math.pi * 440 * t) + 0.15 * math.sin(2 * math.pi * (200 + 3000 * t) * t)
                    + burst * rnd.uniform(-1, 1))
            right = 0.3 * math.sin(2 * math.pi * 660 * t + 1) + 0.1 * math.sin(2 * math.pi * 5000 * t)
        elif seg == 3:  # full-scale noise, clipped
            left, right = 1.5 * rnd.uniform(-1, 1), 1.5 * rnd.uniform(-1, 1)
        elif seg == 4:  # full-scale square wave (L) against a sine (R)
            left = 1.0 if math.sin(2 * math.pi * 997 * t) >= 0 else -1.0
            right = math.sin(2 * math.pi * 3001 * t)
        elif seg == 5:  # isolated clicks
            left = right = (0.9 if i % (rate // 7) < 3 else 0.0)
        else:  # very quiet noise (a few LSB)
            left, right = 0.0002 * rnd.uniform(-1, 1), 0.0002 * rnd.uniform(-1, 1)
        out += struct.pack('<hh', int(max(-1, min(1, left)) * 32767), int(max(-1, min(1, right)) * 32767))
    with open(path, 'wb') as f:
        f.write(out)


def stress_configs():
    """(mode, sample_rate, bits, bitrate list)"""
    c = []
    for rate in (44100, 48000, 96000):
        for bits in (16, 24):
            for b in F_RATES + E_RATES:
                c.append((rate, bits, str(b)))
            c.append((rate, bits, ','.join(str(b) for b in F_RATES + E_RATES)))  # switch every frame
    return c


def run_one(binary, inp, rate, bits, bitrate, samsung_level, mono, ref):
    cmd = [binary, '-i', inp, '-r', str(rate), '-b', str(bitrate), '-bits', str(bits)]
    if samsung_level:
        cmd.append('-samsung-level')
    if mono:
        cmd.append('-mono')
    if ref:
        cmd += ['-ref', ref]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    kv = {}
    for line in p.stdout.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            kv[k] = v
    kv['status'] = p.returncode
    kv['stderr'] = p.stderr.strip()
    return kv


def main():
    ap = argparse.ArgumentParser()
    exe = 'ssc_conformance.exe' if os.name == 'nt' else 'ssc_conformance'
    ap.add_argument('--bin', help='path of the ssc_conformance executable (default: searched in build/)')
    ap.add_argument('--harness', default=os.path.join(HERE, 'vectors'))
    ap.add_argument('--markdown', help='also write the table to this file')
    ap.add_argument('--workdir', default=os.path.join(ROOT, 'build', 'test_data'),
                    help='where the synthetic test signals are written')
    args = ap.parse_args()
    if not args.bin:
        cands = [os.path.join(ROOT, 'build', d, exe) for d in ('', 'Release', 'RelWithDebInfo', 'Debug')]
        args.bin = next((c for c in cands if os.path.exists(c)), cands[0])
    if not os.path.exists(args.bin):
        sys.exit('conformance binary not found: %s (build first, see README)' % args.bin)

    harness = os.path.abspath(args.harness)
    refdir = os.path.join(harness, 'ref_frames')
    have_harness = os.path.exists(os.path.join(harness, 'mix.s16le'))
    if not have_harness:
        print('harness inputs not found in %s: running the synthetic stress test only' % harness)

    rows, failures = [], 0
    for group, sig, fname, rate, bits, b, slevel, mono, ref in (configs() if have_harness else []):
        inp = os.path.join(harness, fname)
        refp = os.path.join(refdir, ref) if ref and os.path.exists(os.path.join(refdir, ref)) else None
        r = run_one(args.bin, inp, rate, bits, b, slevel, mono, refp)
        bad = r['status'] != 0
        failures += bad
        rows.append((group, sig, rate, bits, b, slevel, r, refp))
        print('%-22s %-7s %6d %2d-bit %7d: tell mismatches %s/%s, header %s, SNR %s dB%s%s' % (
            group, sig, rate, bits, b, r.get('tell_mismatch', '?'), r.get('frames', '?'), r.get('header', '?'),
            r.get('snr_ours', '?'),
            (', Samsung: header %s, %s size / %s header mismatches, SNR %s dB' % (
                r.get('ref_header'), r.get('size_mismatch'), r.get('header_mismatch'), r.get('snr_ref')))
            if refp else '', '  FAILED: ' + r['stderr'] if bad else ''))

    # synthetic stress signal: every mode, bit depth and bitrate, and per-frame bitrate switching
    synth, stress = {}, []
    for rate, bits, blist in stress_configs():
        if rate not in synth:
            synth[rate] = os.path.join(args.workdir, 'synth_%d.s16le' % rate)
            if not os.path.exists(synth[rate]):
                os.makedirs(args.workdir, exist_ok=True)
                make_synthetic(synth[rate], rate)
        r = run_one(args.bin, synth[rate], rate, bits, blist, False, False, None)
        bad = r['status'] != 0
        failures += bad
        stress.append((rate, bits, blist, r))
        if bad:
            print('stress %d Hz %d-bit %s: FAILED %s' % (rate, bits, blist, r['stderr']))
    nfr = sum(int(r.get('frames', 0)) for _, _, _, r in stress)
    nmis = sum(int(r.get('tell_mismatch', 1)) for _, _, _, r in stress)
    print('stress test: %d runs, %d frames, %d tell mismatches, %d failed runs' % (
        len(stress), nfr, nmis, sum(r['status'] != 0 for _, _, _, r in stress)))

    lines = ['| Mode | Input | Bitrate | Frame bytes | Header (frame 0) | Tell mismatches | '
             'Size / header vs Samsung | SNR ours (dB) | SNR Samsung (dB) |',
             '|---|---|---:|---:|---|---:|---|---:|---:|']
    for group, sig, rate, bits, b, slevel, r, refp in rows:
        if refp:
            cmp_ = '%s / %s of %s' % (r.get('size_mismatch'), r.get('header_mismatch'), r.get('compared_frames'))
            if r.get('ref_short_frames', '0') != '0':
                cmp_ += ' (%s ref frame cut short)' % r['ref_short_frames']
            snr_o, snr_r = r.get('snr_ours_common'), r.get('snr_ref')
        else:
            cmp_, snr_o, snr_r = 'n/a', r.get('snr_ours'), 'n/a'
        note = ' *' if slevel else ''
        lines.append('| %s | %s | %d%s | %s | `%s` | %s / %s | %s | %s | %s |' % (
            group, sig, b // 1000 if b % 1000 == 0 else b, 'k' + note, r.get('frame_bytes'), r.get('header'),
            r.get('tell_mismatch'), r.get('frames'), cmp_, snr_o, snr_r))
    table = '\n'.join(lines)
    print()
    print(table)
    if any(r[5] for r in rows):
        print('\n* 16-bit input at an hq rate, encoded with SSC_ENC_FLAG_SAMSUNG_HQ16_LEVEL (Samsung codes it 48 dB '
              'down); SNR measured against the input scaled by 1/256.')
    if args.markdown:
        with open(args.markdown, 'w') as f:
            f.write(table + '\n')
    print('\n%d runs (%d against the harness inputs, %d stress), %d failed' % (
        len(rows) + len(stress), len(rows), len(stress), failures))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
