#!/usr/bin/env python3
"""Synthesize what the K1 ADC sees on PA4 while a POCSAG transmission is received.

Chain: POCSAG codewords (576-bit preamble, sync + 16-codeword batches) -> NRZ
2-FSK +/-4.5 kHz (logical 1 = lower frequency) with a Gaussian-ish transition
-> carrier offset -> AWGN in a 25 kHz channel -> FM discriminator (RAW RX: no
de-emphasis, no 300 Hz / 3 kHz filters) -> audio-path low-pass and AC coupling
-> ADC at 9.6 kHz (optional clock error) -> 12-bit around the 2048 bias the app
sets on PA4. Output: little-endian uint16 samples.

--wav22k also writes the discriminator audio as 22050 Hz s16 raw, so the same
transmission can be checked with multimon-ng (-t raw -a POCSAG512/1200/2400).

Messages: --msg RIC:FUNC:A:text (alphanumeric) or RIC:FUNC:N:digits (numeric)
or RIC:FUNC:T: (tone only); several --msg go in one transmission.
"""
import argparse
import numpy as np
from scipy.signal import lfilter, firwin

FS_SIM = 96000
FS_ADC = 9600
BIAS = 2048
LSB_PER_HZ = 0.065   # same PA4 scale as the 406 work (~150 LSB for 2.3 kHz)
DEV = 4500.0
SYNC = 0x7CD215D8
IDLE = 0x7A89C197
POLY = 0x769


def bch(data21):
    """21 data bits (codeword bits 31..11) -> full 32-bit codeword."""
    w = data21 << 10
    for i in range(30, 9, -1):
        if w & (1 << i):
            w ^= POLY << (i - 10)
    cw = (data21 << 11) | (w << 1)
    return cw | (bin(cw).count("1") & 1)


def msg_bits(kind, text):
    out = []
    if kind == "A":
        for ch in text.encode("ascii"):
            out += [(ch >> k) & 1 for k in range(7)]
    elif kind == "N":
        table = {c: i for i, c in enumerate("0123456789*U -)(")}
        for ch in text:
            out += [(table[ch] >> k) & 1 for k in range(4)]
    while out and len(out) % 20:
        out += [0] if kind == "A" else [0, 0, 1, 1]      # NUL / numeric space (0xC)
    return out


def codewords(msgs):
    """Codeword stream (sync included), one message after the other."""
    stream = []                      # flat list of codewords after the sync words
    for ric, func, kind, text in msgs:
        frame = ric & 7
        while len(stream) % 16 != frame * 2:
            stream.append(IDLE)
        stream.append(bch(((ric >> 3) << 2) | func))
        bits = msg_bits(kind, text)
        for i in range(0, len(bits), 20):
            v = 0
            for b in bits[i:i + 20]:
                v = (v << 1) | b
            stream.append(bch((1 << 20) | v))
    stream.append(IDLE)
    while len(stream) % 16:
        stream.append(IDLE)
    words = []
    for i in range(0, len(stream), 16):
        words.append(SYNC)
        words += stream[i:i + 16]
    return words


def tx_bits(msgs, flip):
    bits = [1, 0] * 288                                   # 576-bit preamble
    for w in codewords(msgs):
        bits += [(w >> (31 - k)) & 1 for k in range(32)]
    for f in flip:                                        # bit errors, index in the codeword stream
        bits[576 + f] ^= 1
    return bits


def one_pole_hp(x, fc, fs):
    a = np.exp(-2 * np.pi * fc / fs)
    return lfilter([a, -a], [1, -a], x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--msg", action="append", required=True)
    ap.add_argument("--rate", type=int, default=1200, choices=(512, 1200, 2400))
    ap.add_argument("--out", required=True)
    ap.add_argument("--wav22k")
    ap.add_argument("--tx", type=int, default=1, help="transmissions (same messages)")
    ap.add_argument("--gap-ms", type=float, default=300)
    ap.add_argument("--cnr", type=float, default=30, help="carrier/noise in 25 kHz, dB")
    ap.add_argument("--foff", type=float, default=0, help="carrier offset, Hz")
    ap.add_argument("--audio-lpf", type=float, default=5000)
    ap.add_argument("--hpf", type=float, default=30, help="AC coupling corner, Hz")
    ap.add_argument("--gain", type=float, default=LSB_PER_HZ)
    ap.add_argument("--clock-ppm", type=float, default=0, help="transmitter bit clock error")
    ap.add_argument("--flip", type=int, action="append", default=[],
                    help="flip bit N of the codeword stream (0 = first bit of the first sync)")
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()

    msgs = []
    for m in a.msg:
        ric, func, kind, text = m.split(":", 3)
        msgs.append((int(ric), int(func), kind, text))

    rng = np.random.default_rng(a.seed)
    bits = np.array(tx_bits(msgs, a.flip), dtype=float)
    spb = FS_SIM / (a.rate * (1 + a.clock_ppm * 1e-6))
    idx = (np.arange(int(len(bits) * spb)) / spb).astype(int)
    fsk = np.where(bits[idx] > 0, -DEV, DEV)
    fsk = lfilter(firwin(31, a.rate * 0.8, fs=FS_SIM), [1], fsk)   # soft transitions

    gap = np.zeros(int(FS_SIM * a.gap_ms / 1000))
    parts, amps = [gap], [np.zeros_like(gap)]
    for _ in range(a.tx):
        parts += [fsk, gap]
        amps += [np.ones_like(fsk), np.zeros_like(gap)]
    freq = np.concatenate(parts) + a.foff
    amp = np.concatenate(amps)
    z = amp * np.exp(1j * 2 * np.pi * np.cumsum(freq) / FS_SIM)

    nstd = np.sqrt((10 ** (-a.cnr / 10)) * FS_SIM / 25000 / 2)
    z = z + nstd * (rng.standard_normal(len(z)) + 1j * rng.standard_normal(len(z)))
    ch = firwin(127, 12500, fs=FS_SIM)
    z = lfilter(ch, [1], z.real) + 1j * lfilter(ch, [1], z.imag)

    f = np.angle(z[1:] * np.conj(z[:-1])) * FS_SIM / (2 * np.pi)
    f = lfilter(firwin(127, a.audio_lpf, fs=FS_SIM), [1], f)
    if a.hpf > 0:
        f = one_pole_hp(f, a.hpf, FS_SIM)
    if a.invert:
        f = -f

    if a.wav22k:
        t = np.arange(0, len(f) - 1, FS_SIM / 22050)
        w = np.interp(t, np.arange(len(f)), f) / 6000 * 16000
        np.clip(w, -32767, 32767).astype("<i2").tofile(a.wav22k)

    t = np.arange(0, len(f) - 1, FS_SIM / FS_ADC)
    s = np.interp(t, np.arange(len(f)), f)
    np.clip(np.round(BIAS + a.gain * s), 0, 4095).astype("<u2").tofile(a.out)


if __name__ == "__main__":
    main()
