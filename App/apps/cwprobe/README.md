# CW Probe (bench app, throwaway)

Step 1 of the CW transceiver study (`../cwtrx/STUDY.md`). The PTT keys the
transmitter as a straight key. Every option the study left open is on a key,
and the timings it needs are measured on screen. Not meant for upstream: once
the answers are in, the real app (`cwtrx`) is built from `cwdecode`.

Build: `./compile-app.sh cwprobe` -> `build/Apps/CWProbe.app` (v0.3).

## Safety

- Use a dummy load or the lowest power and a simplex test frequency
  (433.650 MHz or a 2 m simplex you use for tests). Every PTT press transmits.
- The VFO must be in FM, otherwise the screen shows `TX DENIED`.
- A key held 10 s cuts TX (`TOT`); release PTT to re-arm.

## Screen

```
CARR S1 HARD            mode, sidetone method, keying shape
HANG 400 TONE 700       hang time (ms), tone pitch (Hz)
UP 70ms END 30ms        TX set-up time, TX-to-RX time
SETTLE 120ms            RSSI back within 3 dB of the RX baseline after TX
MARK 60ms R40 xxxx      last key-down; REG_40 as read (after an S4 to S7 over)
BOUNCE 2 3ms            PTT bounces on the last edge, span of the bounce
RX -121dBm              or TX; TOT / TX DENIED flags
433.65000               TX frequency
```

Times come from the 10 ms system tick: read them as +/- 10 ms.

## Keys

| Key | Action |
|-----|--------|
| PTT | straight key; TX starts on the first press, ends after the hang time |
| 1 | mode CARR (carrier keying, A1A) / TONE (carrier up, tone keyed, MCW) |
| 2 | sidetone method S0 to S7 (below) |
| 3 | keying shape HARD / RAMP (CARR only: PA bias ramped over 5 ms) |
| 4 | hang time 200 / 400 / 800 / 1500 ms |
| 5 | tone 500 to 900 Hz |
| MENU | clear the measures |
| EXIT | quit |

Settings can only change in RX. They are not saved (defaults: CARR, S1,
HARD, 400 ms, 700 Hz).

## Sidetone methods

| | Local tone | Tone keyed by | Expected on air in CARR |
|---|---|---|---|
| S0 | none | (TX mute, TONE mode only) | pure carrier |
| S1 | AF_BEEP | TX mute, REG_50 | carrier **and** tone modulation (MCW on a keyed carrier) |
| S2 | AF_BEEP | REG_70 enable, TX mute held on | pure carrier, if the speaker still hears the tone |
| S3 | AF_BEEP | REG_70 enable, gain 0, TX mute off | pure carrier, if gain 0 kills the deviation only |
| S4 | AF_BEEP | REG_70 enable, TX mute off, REG_40 deviation <11:0> = 0 | pure carrier, if the monitor sits before the deviation scaler |
| S5 | AF_BEEP | as S4, REG_40 deviation enable <12> cleared instead | same question, other bit |
| S6 | AF_BEEP | REG_40 <12> cleared **and** <11:0> = 0 | the S5 residual gone? |
| S7 | AF_BEEP | as S6, tone gain 20 instead of 66 | lower residual, sidetone still loud enough? |

S4 to S7 restore REG_40 before returning to RX (the firmware never writes
that register itself, so a leftover would silence every later FM TX). After
an S4 to S7 over, check one normal FM transmission outside the app still has
audio, to be safe.

In TONE mode the on-air tone is always keyed by TX mute; S1 to S7 only turn
the local AF_BEEP monitor on.

### Bench results

v0.1, CARR mode, RTL-SDR + gqrx as receiver (Johan, 2026-10-09):

| | Local | On air |
|---|---|---|
| S0 | no tone | no tone |
| S1 | tone | tone |
| S2 | no tone | no tone |
| S3 | tone | tone |

So the TX mute (REG_50) also mutes the local monitor, and REG_70 gain 0
does not remove the deviation: the local beep is a monitor of the TX audio
path. v0.2 adds S4/S5 to cut the deviation at the RF side instead.

v0.2, CARR mode (Johan, 2026-10-09):

| | Local | On air |
|---|---|---|
| S4 | tone | tone |
| S5 | tone | very faint |

The deviation enable bit (REG_40 <12>) is the lever: the monitor sits before
it. v0.3 adds S6 (both REG_40 changes) and S7 (S6 with a low tone gain) to
remove the residual.

v0.3, CARR mode, receiver listened without hearing the K5 speaker (Johan,
2026-10-09): S5, S6 and S7 send nothing on air. The faint S5 tone of the v0.2
run was the K5 speaker heard acoustically, not RF.

**Retained method: S5.** Clear REG_40 bit 12 (TX deviation enable) for the
whole over, key the tone with the REG_70 enable bit, TX mute (REG_50) off,
AF_BEEP to the speaker; restore REG_40 before returning to RX. One register
bit, pure A1A on air, sidetone that follows the keying.

Also confirmed: S5 and S7 sidetone loud enough locally (the tone gain can be
a user setting), and normal FM TX outside the app is intact afterwards
(REG_40 restore works).

Turnaround (v0.3, Johan, 2026-10-09): `UP` 80 ms, `END` 0 ms (under one
10 ms tick), `SETTLE` 580 ms. Of the 80 ms, 50 ms is the fixed delay inside
`tx_tone()` (`BK4819_TransmitTone`); the real app can skip that call and
write REG_71/REG_70 itself, leaving the ~15 ms of `tx_set_params()`.

## Bench protocol

Receiver: RTL-SDR with gqrx on the Mac, tuned to the K5 frequency.
`CW-U` mode (narrow filter, about 700 Hz pitch) to hear and see the carrier,
`Narrow FM` to hear any tone modulation. Keep the K5 speaker out of earshot
of the gqrx audio (headphones), or a local sidetone passes for RF.

1. **Sidetone.** Done (S5 retained, see the results above).
2. **Key clicks.** CARR, S5, HARD then RAMP: a steady series of dots. In
   gqrx, FFT and waterfall zoomed to about 20 kHz around the carrier, peak
   hold on: clicks show as a broad skirt flashing on each edge. Compare the
   skirt width and level HARD vs RAMP (a screenshot of each is ideal), and
   whether RAMP sounds softer in CW-U.
3. **Turnaround.** Done (`UP` 80 ms, `END` under 10 ms, `SETTLE` 580 ms).
4. **PTT bounce.** Press and release the PTT a dozen times, slowly and fast.
   Note the worst `BOUNCE` count and span: it sets the debounce of the real
   app (currently 3 ms of stable reads).
5. **Timing.** Send `PARIS` a few times at your normal speed and decode the
   gqrx audio with multimon-ng (gqrx: UDP audio button, default port 7355,
   48 kHz mono):

   ```
   nc -lu 7355 | sox -t raw -e signed -b 16 -r 48000 -c 1 - \
     -t raw -e signed -b 16 -r 22050 - | multimon-ng -t raw -a MORSE_CW -
   ```

   A first character that comes out wrong on every over means the set-up
   time eats the first element.
6. **Leakage.** In CARR between elements, gqrx signal level close to the K5
   (dummy load): how many dB remain with the PA gated off, compared with key
   down?

Bring back: HARD vs RAMP screenshots or impression, worst `BOUNCE`, the
multimon-ng output for `PARIS`, and the key-up/key-down level difference.
