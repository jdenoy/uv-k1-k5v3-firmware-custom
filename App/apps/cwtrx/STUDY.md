# CW transceiver overlay app: feasibility study

Branch: `feature/cw-transceiver`. Bench radio: UV-K1 (V3 board). Status: study only, no code yet.

Goal: one overlay app that sends Morse by keying the transmitter with the PTT
button (PTT used as a straight key) and decodes received CW on the same
frequency, with semi break-in (automatic return to RX after a short hang time).

## Verdict

Feasible with the current overlay ABI (level 2), no resident firmware change
required for a first version. Two points need a bench test before writing code:
the sidetone in pure carrier mode (solved on the bench, see Sidetone) and
the key clicks of hard PA gating. The
4 KiB budget is the main engineering constraint, not the radio.

## What already exists

Both halves are already in the tree, written by Armel:

| App | Role | Size (code + bss) | Reusable part |
|-----|------|-------------------|---------------|
| `cwdecode` | RX: decodes A1A from the BK4829 RSSI envelope, adaptive WPM, threshold, AGC | 2,996 B (+716 B assets) | the whole decoder: `acceptEdge(down, t)` takes key-down/key-up edges with a timestamp, from any source |
| `beacon` | TX: automatic CW fox, TONE (MCW, F2A) or CARR (carrier keying, A1A) | 2,692 B (+173 B assets) | the keying sequence `tx_set_params`, `tx_tone`, `tx_mute`, `tx_carrier`, `tx_end` |

Measured with `./compile-app.sh cwdecode beacon` on 2026-10-09.

The natural plan is to fork `cwdecode` and add a TX state machine, not to fork
`beacon`: the decoder is the larger and more delicate part, and `beacon` only
contributes about ten lines of keying.

## How each piece maps to the ABI

### PTT as a key

`api->get_key()` calls `KEYBOARD_GetKey()`, which returns `KEY_PTT` from the
raw PTT GPIO when no keypad key is down (`App/driver/keyboard.c`). Inside an
overlay app the resident PTT handler does not run, so the app owns the PTT
completely. The overlay wrapper even passes `APP_KEY_PTT` through while the
screen saver is shown (`app_get_key` in `App/apps/app_overlay.c`).

- The PTT line has no debounce (the keypad matrix has one, the PTT GPIO does
  not). The app needs its own: two equal samples 5 ms apart is enough for a
  mechanical switch and costs 5 ms of latency.
- `cwdecode` polls keys every 50 ms (`KEY_MS`). That is far too slow for
  keying: at 20 WPM a dot lasts 60 ms. The PTT must be polled in the 10 ms
  sample loop (or faster during TX, see timing below). The other keys can stay
  at 50 ms.
- A keypad key pressed during a dash would mask the PTT (keypad wins in
  `KEYBOARD_GetKey`). Acceptable: lock the keypad while transmitting.

### Transmit

Sequence proven by `beacon` in CARR mode:

1. first PTT press: `tx_state()` (0 = allowed), then `tx_set_params()` once.
   That is `RADIO_SetTxParameters`: RX off, TX frequency, PA on, about 15 ms
   of fixed delays (`SYSTEM_DelayMs(10)` + `(5)`).
2. each element: `tx_carrier(true)` / `tx_carrier(false)`, which only toggles
   BK4819 GPIO1 (PA enable). This is a register write over the BK4819 serial
   bus, well under 1 ms. Fast enough for any manual speed.
3. after the hang time with the key up: `tx_end()` = PA off +
   `RADIO_SetupRegisters(true)`, back to RX.

Two modes, as in `beacon`, selectable by a key and saved:

- **CARR (A1A, true CW):** PA gated. Only a receiver with a BFO (SSB/CW rig)
  hears a tone; an FM receiver hears noise quieting. Another UV-K1/K5 running this
  app or `cwdecode` decodes it from RSSI.
- **TONE (MCW, F2A):** carrier stays up during the over, an audio tone (700 Hz
  by default) is keyed with `tx_mute`. Audible on any FM handheld. This is the
  practical mode for local nets and for training with other FM users.

`tx_state()` refuses TX when the TX VFO is not in FM. The app should therefore
require FM on the VFO and handle USB monitoring itself (below).

### Timing and latency

`ticks_ms()` has 10 ms resolution. At 20 WPM the dot is 60 ms, so a 10 ms
quantum is a 17 % error on a dot: acceptable for a hand key, marginal above
about 25 WPM. Better: run the TX loop on `delay_ms(2)` and count iterations
locally, which gives 2 ms resolution without any new API.

The first element of an over is delayed by the ~15 ms of `tx_set_params()`.
If the key-up is applied in real time, that first dot is shortened by 15 ms.
Fix: apply every edge through a fixed delay line (for example 20 ms). Every
mark and space then keeps its exact length, the whole over is simply shifted by
20 ms. The cost is a small ring buffer of edge times (a few bytes).

**Bench result (CW Probe 0.3):** `UP` 80 ms, of which 50 ms is the delay
inside `tx_tone()`; skipping it (direct REG_71/REG_70 writes) leaves about
15 to 30 ms, which the 20 ms delay line absorbs. `END` under 10 ms. RSSI
`SETTLE` after TX: 580 ms, so the RX decoder must blank about 600 ms after
`tx_end()`, and the default hang time should be 800 ms (600 ms minimum).

Software time-out: the resident TOT does not run inside the app, so the app
must cut TX itself (key held longer than, say, 10 s, or total over longer than
the configured TOT).

### Sidetone

Without a local tone, keying blind is unusable. Options, in order of
preference:

1. **TONE mode:** `BK4819_TransmitTone(play_speaker, hz)` already routes the
   tone to the speaker when `play_speaker` is true (`AF_BEEP` +
   `AUDIO_AudioPathOn`). The API wrapper `tx_tone` passes `false`, but the app
   can call `set_af(3 /* BEEP */)` and `audio_path(true)` after `tx_tone()`.
   To verify on the bench: whether `tx_mute` (REG_50) also mutes the local
   monitor, in which case the sidetone follows the keying for free.
2. **CARR mode:** the tone generator must sound in the speaker without
   modulating the carrier. Candidate: keep the tone running, keep `tx_mute`
   on (no modulation), and check whether the AF_BEEP path still hears it.
   If not, set the TX tone deviation (REG_70 tuning gain) to 0 and keep
   `tx_mute` off. This is the main unknown of the study.
3. **Fallback:** green LED (`api->led`) plus the on-screen decode of what is
   being sent. Usable for practice, poor for real QSOs.

**Bench result (CW Probe 0.1 to 0.3, 2026-10-09): solved.** The local
AF_BEEP monitor is taken after the TX mute (REG_50) but before the TX
deviation enable (REG_40 bit 12). Clearing that bit for the whole over and
keying the tone with the REG_70 enable bit gives a sidetone that follows the
keying and nothing on air (RTL-SDR + gqrx in FM). REG_40 must be restored before
`tx_end()`: the firmware never writes it, so a leftover would mute every
later FM transmission. Details in `../cwprobe/README.md`.

### Receive

Reuse `cwdecode` unchanged: RSSI envelope, quiet-channel calibration,
adaptive threshold and WPM. During TX the RSSI path is meaningless (own
carrier), so sampling stops and the RX decoder state is frozen; after
`tx_end()` the AGC needs a few tens of ms before samples are trusted again
(blanking window).

Optional audible RX: the BK4819 has a USB demodulator (`BK4819_AF_BASEBAND2`
= 5, the mode behind the firmware's USB modulation). With `set_af(5)` and the
RX tuned 700 Hz below the carrier, an A1A signal becomes an audible beat note.
The RX offset needs the frequency registers (REG_38/39) through `bk_write`,
then `tx_set_params()` retunes TX correctly by itself. Nice to have, not
needed for the decoder, and it costs bytes: phase 2.

### Echo decode of the sent text

Feeding the debounced PTT edges into the same `acceptEdge()` shows what the
operator is sending, in the same scrolling history (different marker, for
example `>` at line start). Practically free in code and very useful for
practice: the decoder becomes a keying coach. The WPM estimator should keep
separate state for RX and TX, or reset when the direction changes, since the
two stations rarely key at the same speed.

## Budget estimate (4,096 B code + data + bss)

| Part | Estimate |
|------|----------|
| `cwdecode` as is | 2,996 B |
| PTT debounce, T/R state machine, hang time, delay line, TOT | 350 to 500 B |
| sidetone setup, CARR/TONE mode, config bytes | 100 to 150 B |
| TX status display (TX icon, mode, hang time) | 100 to 200 B, mostly assets |
| **Total** | **3,550 to 3,850 B** |

It fits, with 250 to 550 B of margin. If it does not, cheapest cuts: the
compact-font toggle and the AGC key of `cwdecode` (keep AGC on), or moving
labels to assets. USB monitoring would probably not fit in the same app.

Fallback if the merge does not fit: a separate `CW Key` app (TX + echo
decode only, around 2.5 KiB) next to `CW Decode`. Less convenient, since a
QSO then needs switching apps.

## Proposed controls

| Key | Action |
|-----|--------|
| PTT | straight key |
| `4` | CARR / TONE |
| `5` | hang time 200 / 400 / 800 / 1500 ms (`F` reverses) |
| `6` | sidetone pitch 500 to 900 Hz by 100 Hz |
| others | unchanged from `cwdecode` |

Keypad locked while in TX.

## Risks and points to measure on the bench

1. **Key clicks:** `tx_carrier()` switches the PA enable line with no ramp.
   Hard keying of a 5 W PA spreads clicks over several kHz. To measure with
   the RTL-SDR in gqrx (or the LimeSDR). Mitigation if needed: ramp the PA bias
   (REG_36) in 3 to 4 steps over about 5 ms around each edge.
2. **Leakage with PA off:** `beacon` notes that the BK4819 still radiates a
   little with the PA gated. A nearby receiver hears a weak carrier between
   elements; it does not matter at a distance, but it lowers the on/off
   contrast for a close RSSI decoder.
3. **Sidetone in CARR mode:** see above, the main unknown.
4. **T/R turnaround:** `RADIO_SetupRegisters` then AGC settling. Defines the
   minimum useful hang time (probably 150 to 200 ms).
5. **PTT switch bounce and wear:** the UV-K1 PTT is a tactile switch, not a key;
   sustained keying will wear it. Long term: an external key on the
   accessory jack. To check: whether the jack's PTT reaches the same GPIO as
   the side button (it does on speaker-mics, to confirm on the V3 board).
6. **Repeater shift / split:** TX and RX VFO must be the same frequency
   (simplex). Refuse TX, or warn, when they differ.

## Suggested order of work

1. Bench probe app (throwaway): key the PA with PTT in both modes, sidetone
   variants, measure clicks and turnaround. Answers risks 1 to 4.
2. Fork `cwdecode` into `cwtrx`, add the TX state machine and echo decode.
3. Size with `./compile-app.sh cwtrx`, trim if needed.
4. Over-the-air test: UV-K1 to RTL-SDR/gqrx (CW-U, multimon-ng decode), then
   LimeSDR keyed carrier to UV-K1 for the RX side (only one radio on hand).
5. Phase 2 if room: USB beat-note monitoring.

Naming: directory `cwtrx`, label `CW TRX`, `APP_VER` 0.1 in `build.sh`.
