# EPIRB 406: on-radio 406 MHz beacon decoder (work in progress)

Goal: an overlay app that decodes first-generation Cospas-Sarsat 406 MHz beacon
messages directly on the UV-K1 / UV-K5 v3 and shows the 15-hex ID, country,
protocol, position and BCH status.

Status:

| Step | State |
|---|---|
| Feasibility: can an app read the demodulated signal? | **Done**: yes, on PA4 (see `../lab406/README.md`) |
| Decoder core (`dec406.c`), host-tested on synthetic audio | **Done**: 17/17 tests pass |
| Radio app (`epirb406_app.c`: trigger, sampling, display) | **v1.4 decodes the full reference frame on the K1** (position, `END F58521EDA3`) |
| Bench test with the beacon generator on 433.650 MHz | **Done** (2026-09-27) |

## Using the app

1. Set the VFO to the beacon frequency, **FM, wide bandwidth**: 406.031 MHz for
   real beacons, or the generator frequency (433.650 MHz on the bench).
2. Launch **EPIRB 406** with no burst in progress: the noise floor is measured at
   launch.
3. Lower the volume: the RX audio must stay on (it is what reaches PA4), so the
   speaker plays each burst.

Each burst is detected on RSSI (10 dB above the floor), sampled for up to 900 ms
and decoded. The last 2 decoded messages are kept.

| Screen | Content |
|---|---|
| Line 0 | 15-hex ID |
| Line 1 | Country code and protocol |
| Line 2 | Position (5 decimals, truncated) or "no position" |
| Line 3 | `END` + raw bits 105-144 of the last frame received, in hex (bench diagnosis) |
| Small rows | SELF-TEST, LONG/SHORT, BCH-1/BCH-2; `#` decode number, RSSI of the burst, internal/external position source, 121.5 homing, `coarse` (no PDF-2 offsets), `rawID` (not a standard location protocol, ID = raw bits 26-85); `1/2` history position |
| Bottom row | RSSI / floor, decodes ok, errors, last error: `nosync` (no frame sync found) or `cut` (sync found, message incomplete) |

Keys (UV-K5 and UV-K1): UP/DOWN browse history · MENU clear · EXIT quit. The
backlight stays on while the app runs (since v1.4), so bursts can be watched. The
decoder integrates the discriminator pulses (INT); the INT/DIR toggle of v1.1-v1.2
(key 1) was removed in v1.3 once INT was proven on the radio.

Space: the app uses 4,068 of the 4,096 bytes (v1.4). To make it fit, the sync search
compares 32-bit words (the inverted-polarity distance is 44 minus the normal
one), number formatting uses subtraction instead of division (no `__udivsi3`),
protocol names are packed in one string, the history holds 2 entries, and the
app is built with `-fno-jump-tables` (48 bytes saved on the key `switch`).

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
  plays the burst.
- **PA4 bias**: the pin has no DC reference; the app holds it at mid-scale with
  the MCU DAC (output buffer off, code 2048) for the whole run. The volume knob is analog; whether it changes the PA4 level
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

Size for Cortex-M0+ (firmware toolchain, `-Os`): 1,468 bytes of code, after the
size work described above.

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

Results (2026-09-27): 29/29 pass. The first 17 cases, covering CNR 12-15 dB, ±5 kHz offset,
inverted chain, ±3000 ppm clock error, 50-250 µs rise time, 3 kHz audio
low-pass, 150 Hz AC coupling, a combined worst case, self-test, short message,
a corrupted bit (BCH-1 reported as failed) and 3 bursts in a row.

Sensitivity (5 seeds per point, CNR in 25 kHz):

| CNR | 12 dB | 10 dB | 9 dB | 8 dB | 7 dB | 6 dB |
|---|---|---|---|---|---|---|
| Decoded, BCH ok | 5/5 | 4/5 | 4/5 | 3/5 | 1/5 | 0/5 |

Possible gains later: BCH error correction (BCH-1 corrects up to 3 bit errors,
BCH-2 up to 2), and a 12.5 kHz filter when the beacon channel is known (+3 dB).

## Bench results

**2026-09-27, UV-K1, generator on 433.650 MHz, DIR mode, first on-air decode.**
Sync found, but the message is wrong in a systematic way:

| | Sent | Received |
|---|---|---|
| 15-hex ID | `1C7C2468ACFFBFF` | `0C3C002004FFBFF` |
| Country | 227 | 97 |
| Position | 49.27111 N 0.78222 E | 16.00000 N 0.25000 E (coarse) |
| BCH | ok / ok | ERR / ERR |

Over bits 26-64: every `1` preceded by a `0` is read as `0` (9/9), every `1`
preceded by a `1` is correct (8/8), no `0` is ever wrong. Timing and sync are
therefore right; the decision fails where biphase-L has no transition at the bit
boundary (`0` then `1`), which depends on the real signal shape at PA4.

None of the simulated chains reproduce this exact pattern (pulse or de-emphasized
signal, low-pass, AC coupling, inverted: INT always decodes). Simulated ADC
clipping at 0 (PA4 bias is only 518/4095) does break INT decoding entirely, which
may explain why DIR was in use.

**Diagnosis with 406 Lab scope mode (v1.1-v1.5).** Sampled at 9.6 kHz, PA4 slid
towards 0 V as soon as fast sampling started, and about half of the message
samples were clipped at 0 whatever the AF gain or RAW setting (details in
`../lab406/README.md`). PA4 has no DC reference of its own: the audio reaches it
through a coupling capacitor, and fast ADC sampling drags the node down.

**Fix.** The MCU DAC on PA4, output buffer off, set to mid-scale (2048), holds
the pin at about VDD/2 as a weak bias. Measured with it (AF gain 14, RAW):
carrier 2048-2049, message 1896-2194, no clipping. EPIRB 406 v1.2 turns this bias
on at launch and restores the DAC, its clock and the pin on exit. The host test
generator now uses the measured bias (2048) and level (~150 LSB peak); 17/17
still pass.

**v1.2 on the bench (INT mode):** ID `1C7C2468ACFFBFF`, country 227, "Std test",
BCH-1 OK: the first block (bits 25-106) is received intact and BCH-1 is confirmed
on a real frame. BCH-2 fails, so the fine position offsets and the homing flag
are not applied (`49.25000N 0.75000E coarse`, no `121.5`). Either BCH-2 is
computed differently from the generator, or the last bits of the burst are
received with errors. v1.3 shows the raw end of the frame (`END` + bits 105-144
as hex) to tell which: an error-free reception of the reference frame reads
`END F58521EDA3`.

**v1.3 on the bench.** The generator sends exactly the reference frame
(`FFFE2F8E3E12345631401FB07DF58521EDA3`), so BCH-2 is computed correctly. The
received end changed from burst to burst (`F68521E704`, `D5786733DD`): errors
growing towards the end of the burst, not a fixed pattern.

**Root cause: DC tracker rounding.** The mean was updated with `e >> 10`; an
arithmetic right shift floors, so every small negative deviation moved the mean
down by one step and small positive ones never moved it up. The mean crept
downwards during the message, the leaky integrator accumulated the offset, and
the last bits went wrong. Negligible with the large swing of the first model,
fatal at the ~150 LSB swing measured on PA4. Reproduced on the host at the
measured level (BCH-1 ok / BCH-2 fail, 3 of 4 seeds at 30 dB CNR), fixed with
round-to-nearest: 4/4 from 30 down to 12 dB, and from -3% to +2% bit-rate error.
A second-order (bit-rate tracking) loop was tried and dropped: no gain once the
rounding was fixed.

The test suite now has 12 more cases at the measured level (CNR 12 dB, ±2%
clock, 4 noise seeds each): the old decoder fails 6 of them with the radio's
symptom, the fixed one passes all 29.

**Recordings from the radio (406 Rec, 2026-09-27).** Three bursts recorded
on the K1 and decoded on the host give exactly the radio's result (sync found,
polarity inverted, ID correct, BCH FAIL/FAIL). Activity per 10 ms shows carrier
until 150 ms after the trigger, modulation until **410 ms**, then noise: the
carrier is gone. 260 ms of modulation instead of 360 ms, so only ~104 of the 144
bits are on air, at the same point on every burst; the bit rate itself is exact
(the preamble pattern repeats every 24 samples at 9.6 kHz). A synthetic burst cut
after 106 bits decodes as BCH ok/FAIL (the v1.2 result) and after 104 bits as
FAIL/FAIL with the correct ID (the v1.4 result). **The bench generator (rpitx
playing the IQ file) drops the last ~100 ms of the burst**; the decoder is not at
fault for the end of the frame. Fix on the generator side: pad the IQ file with
at least 150 ms of carrier after the last bit. The recordings are kept outside
the repo in `~/dev/uv-k1-406-captures`.

**Correction.** The generator is not at fault: an RTL-SDR on the Mac decodes the
full burst on air. 406 Rec v1.1 (RSSI every 10 ms) showed RSSI falling to the
noise floor (about -93 dBm) 410-420 ms after the trigger on every burst: the
signal left the K1's channel, most likely a frequency move of the rpitx carrier
near the end of the burst that a wideband RTL-SDR decoder follows and a 25 kHz
channel does not. (The RSSI read during fast sampling showed a constant -21 dBm,
not the -64 to -75 dBm of the trigger; only its change is meaningful. v1.1 is a
diagnostic build: the RSSI reads disturb the sample timing.)

**Full decode on the radio (2026-09-27).** EPIRB 406 v1.4 then decoded the whole
reference frame: ID `1C7C2468ACFFBFF`, country 227, "Std test", position
49.27111 N 0.78222 E, `END F58521EDA3` (all 144 bits intact, BCH-2 ok).

The earlier fixes stand on their own: the PA4 bias (v1.2) removed the clipping
that caused the "every 1 after a 0" errors, and the DC tracker rounding (v1.4) is
a real bias reproduced on the host at the measured level.

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

## Version

`APP_VER` in `build.sh` is bumped for every build that goes on a radio. It is
compiled in (`-DAPP_VERSION`) and shown in the status-bar title (e.g. `v1.1`), since
the apps menu does not display versions. Current: **v1.4**.
