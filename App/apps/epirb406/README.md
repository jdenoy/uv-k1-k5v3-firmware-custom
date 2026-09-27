# EPIRB 406: on-radio 406 MHz beacon decoder (work in progress)

Goal: an overlay app that decodes first-generation Cospas-Sarsat 406 MHz beacon
messages directly on the UV-K1 / UV-K5 v3 and shows the 15-hex ID, country,
protocol, position and BCH status.

Status:

| Step | State |
|---|---|
| Feasibility: can an app read the demodulated signal? | **Done**: yes, on PA4 (see `../lab406/README.md`) |
| Decoder core (`dec406.c`), host-tested on synthetic audio | **Done**: 17/17 tests pass |
| Radio app (sampling, trigger, display) | To do |
| Bench test with the beacon generator on 433.650 MHz | To do |

## Signal path on the radio

```
beacon RF --> BK4829 (RAW: no HPF300 / LPF3K / de-emphasis, AFC off)
          --> AF output (FM) --> audio circuit --> PA4 --> ADC channel 4 @ 9.6 kHz
```

- **RAW receive** is set by the app itself (REG_2B bits 10/9/8, REG_73 bit 4), as
  `BK4819_EnterRaw` does. The firmware restores all radio registers on app exit.
- **PA4** is the voice-prompt DAC pin. Voice is disabled in every preset, so the
  app switches the DAC off and reads PA4 as an analog input. Measured on the K1:
  idle 518, message range 165-894 (about 0.6 V p-p), no clipping.
- The receive audio must be on (AF output enabled, audio path on): the speaker
  plays the burst. The volume knob is analog; whether it changes the PA4 level
  still has to be checked.
- Tuning: first-generation channels span 406.025-406.040 MHz. One VFO at
  406.031 MHz with the wide filter covers them all; a carrier offset only shifts
  the audio DC level, which the decoder removes.

## First-generation message (C/S T.001)

400 bps biphase-L, ±1.1 rad phase modulation. About 160 ms of unmodulated carrier,
then 112 (short) or 144 (long) bits, one burst about every 50 s.

| Bits | Content |
|---|---|
| 1-15 | Bit sync, all ones |
| 16-24 | Frame sync: `000101111` normal, `011010000` self-test |
| 25 | Format flag: 1 = long |
| 26 | Protocol flag: 0 = location protocols, 1 = user protocols |
| 27-36 | Country code (France 227/228) |
| 37-40 | Protocol code (37-39 for user protocols) |
| 41-85 | Identification and coarse position (PDF-1) |
| 86-106 | BCH-1, BCH(82,61) over bits 25-106 |
| 107-132 | Fine position offsets and flags (PDF-2, long messages) |
| 133-144 | BCH-2, BCH(26,14) over bits 107-144 |

Standard location protocols (codes 0010-0111, 1100, 1110): bits 41-64
identification, 65-85 coarse position (N/S, degrees, 15' steps), 107-110 `1101`,
111 position source, 112 121.5 MHz homing, 113-132 offsets (±minutes, 4 s steps).
The 15-hex ID is bits 26-85 with the position replaced by the default pattern.

## Decoder (`dec406.c`)

Streaming, one call per ADC sample, integer only, no division per sample,
freestanding (no libc). About 150 bytes of state.

1. **DC removal**: exponential tracker, fast while searching (~13 ms, so the
   noise before the burst does not bias it), slow once locked (~107 ms).
2. **Phase rebuild**: the discriminator outputs dphi/dt, a pulse per phase flip.
   A leaky integrator (~1.7 ms) rebuilds the ±1.1 rad biphase waveform.
3. **Clock recovery**: a DPLL (gain 1/8) aligns 12-sample half-bit slots on the
   zero crossings of the rebuilt waveform.
4. **Sync search**: the signs of the last 44 half-bits are compared with 13
   preamble ones + frame sync, normal and self-test, in both polarities (the
   receive chain may invert). Up to 2 mismatches allowed.
5. **Bits**: soft decision per bit, first half minus second half.
6. **Parsing**: BCH-1 and BCH-2 syndromes, country, protocol, 15-hex ID, position
   for standard location protocols (coarse + fine offsets).

Size for Cortex-M0+ (`-Os`): 2,233 bytes of code. With sampling and display the
app should be around 3.5 of the 4 KiB overlay. If space runs short: drop the user
protocol name table, or shorten the names.

## Tests (`test/`)

```
test/run_tests.sh
```

- `frame406.py` builds frames: the bench generator's reference frame (long,
  standard location test, country 227, ID `1C7C2468ACFFBFF`), a self-test
  variant, a short message, or a frame with one bit flipped.
- `gen406.py` synthesizes what PA4 sees: biphase phase modulation with
  raised-cosine transitions, carrier offset, noise in a 25 kHz channel, FM
  discriminator, audio low-pass, AC coupling, 9.6 kHz ADC with clock error,
  12-bit around the measured 518 bias, full-scale noise when no carrier.
- `host_dec406.c` runs the decoder over the samples and prints each message.

Results (2026-09-27): 17/17 pass, covering CNR 12-15 dB, ±5 kHz offset,
inverted chain, ±3000 ppm clock error, 50-250 µs rise time, 3 kHz audio
low-pass, 150 Hz AC coupling, a combined worst case, self-test, short message,
a corrupted bit (BCH-1 reported as failed) and 3 bursts in a row.

Sensitivity (5 seeds per point, CNR in 25 kHz):

| CNR | 12 dB | 10 dB | 9 dB | 8 dB | 7 dB | 6 dB |
|---|---|---|---|---|---|---|
| Decoded, BCH ok | 5/5 | 4/5 | 4/5 | 3/5 | 1/5 | 0/5 |

Possible gains later: BCH error correction (BCH-1 corrects up to 3 bit errors,
BCH-2 up to 2), and a 12.5 kHz filter when the beacon channel is known (+3 dB).

## Open points

- **Default position pattern** in the 15-hex ID (`0 1111111 11 0 11111111 11`)
  follows the reference decoder; to confirm against T.001.
- **BCH generators** are the T.001 ones. They are only checked against frames
  built with the same code; the bench generator's real frame
  (`FFFE2F8E3E12345631401FB07DF58521EDA3` expected) will confirm them.
- **Protocol name table** to check against T.001; national location, RLS and
  ELT-DT positions are not decoded, and their ID is shown as raw bits 26-85.
- The synthetic chain is a model: the first real captures from PA4 may need the
  integrator or DC constants retuned (`dec406_init(..., integrate)` also allows a
  phase-like input if the hardware turns out to integrate already).
- Second-generation beacons (spread-spectrum OQPSK) are out of scope.
