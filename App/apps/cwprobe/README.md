# CW Probe (bench app, throwaway)

Step 1 of the CW transceiver study (`../cwtrx/STUDY.md`). The PTT keys the
transmitter as a straight key. Every option the study left open is on a key,
and the timings it needs are measured on screen. Not meant for upstream: once
the answers are in, the real app (`cwtrx`) is built from `cwdecode`.

Build: `./compile-app.sh cwprobe` -> `build/Apps/CWProbe.app` (2,332 B, v0.2).

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
MARK 60ms R40 xxxx      last key-down; REG_40 as read (after an S4/S5 over)
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
| 2 | sidetone method S0 to S5 (below) |
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

S4 and S5 restore REG_40 before returning to RX (the firmware never writes
that register itself, so a leftover would silence every later FM TX). After
an S4/S5 over, check one normal FM transmission outside the app still has
audio, to be safe.

### Bench results

v0.1, CARR mode (Johan, 2026-10-09):

| | Local | On air |
|---|---|---|
| S0 | no tone | no tone |
| S1 | tone | tone |
| S2 | no tone | no tone |
| S3 | tone | tone |

So the TX mute (REG_50) also mutes the local monitor, and REG_70 gain 0
does not remove the deviation: the local beep is a monitor of the TX audio
path. v0.2 adds S4/S5 to cut the deviation at the RF side instead.

In TONE mode the on-air tone is always keyed by TX mute; S1 to S3 only turn
the local AF_BEEP monitor on.

## Bench protocol

IC-705 on the same frequency: CW mode (to hear and see the carrier) and FM
mode (to hear any tone modulation). Waterfall at the narrowest span.

1. **Sidetone (the main unknown).** CARR + HARD, for S1, S2, S3: send a few
   `V`s. For each, note (a) is a sidetone heard in the K5 speaker, does it
   follow the keying, (b) on the IC-705 in FM: is a 700 Hz tone heard on the
   carrier. Winning method = sidetone heard and no tone in FM.
   Then TONE + S1: sidetone heard, follows the keying?
2. **Key clicks.** CARR, S0, HARD then RAMP: hold a steady series of dots and
   look at the IC-705 waterfall and the audio around the carrier (clicks are
   the broadband ticks on each edge). LimeSDR capture if you want numbers:
   occupied bandwidth HARD vs RAMP. Note whether RAMP sounds softer.
3. **Turnaround.** Note `UP`, `END`, `SETTLE` over several overs. With the
   IC-705 sending a steady carrier a few seconds after the K5 releases, check
   that `SETTLE` stays below the hang time you would like to use.
4. **PTT bounce.** Press and release the PTT a dozen times, slowly and fast.
   Note the worst `BOUNCE` count and span: it sets the debounce of the real
   app (currently 3 ms of stable reads).
5. **Timing.** Send `PARIS` a few times at your normal speed with the IC-705
   CW decoder on (or the K5's own `MARK` for single dots). The IC-705 decode
   tells whether the first element of an over comes out shortened.
6. **Leakage.** In CARR between elements, IC-705 S-meter close to the K5
   (dummy load): how much carrier remains with the PA gated off?

Bring back: the S1/S2/S3 table filled in, HARD vs RAMP impression or
capture, typical `UP`/`END`/`SETTLE`, worst `BOUNCE`, and whether the IC-705
decodes the K5 cleanly.
