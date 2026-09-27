# 406 Lab: feasibility probe for an on-radio 406 MHz decoder

Overlay app (RX only) that answers one question: **can an app read the
demodulated receive signal?** It is the groundwork for the EPIRB 406 decoder app
(`App/apps/epirb406/`).

## Why a probe was needed

A first-generation Cospas-Sarsat beacon sends 400 bps biphase-L phase modulation
(±1.1 rad). Decoding it needs the demodulated waveform sampled at a few kHz. The
firmware gives no obvious route:

- No BK4829 register is documented as exposing the RX waveform
  (`App/ui/main.c`, audio scope comment; REG_64 is the mic/VOX level).
- The MCU ADC is only used on PB0 (battery, channel 8, `App/board.c`).
- The BK4829 FSK modem only handles 1200/2400 baud FFSK and NOAA SAME.

## What the probe does

1. Switches the receiver to **RAW** (as `BK4819_EnterRaw` does in `bk4829.c`):
   REG_2B bits 10/9/8 set (RX HPF300, LPF3K and de-emphasis off), REG_73 bit 4 set
   (AFC off). Every radio register is restored by the loader's
   `RADIO_SetupRegisters()` when the app exits.
2. Triggers on a burst when RSSI rises 10 dB above the floor measured at launch.
3. For ~430 ms polls one group of 8 BK4829 registers plus one ADC pin, timed from
   SysTick (48 MHz, 10 ms period), and keeps two windows per channel:
   - A = 15-130 ms after trigger (unmodulated carrier)
   - B = 200-420 ms (biphase message)
4. Activity = mean |v[n] - v[n-1]| per sample. A channel that tracks the
   demodulated signal is quiet in A and busy in B.
5. 16 groups cover registers 0x00-0x7F (0x5F, the FSK FIFO, is never read).

ADC pins probed (key 4): **PB1** (channel 9, set analog by `BOARD_ADC_Init` but
never read) and **PA4** (channel 4, the voice-prompt DAC output that feeds the
audio amplifier; voice is disabled in every preset). For PA4 the DAC is switched
off and the pin set analog only while it is read; ADC channel 8 is restored before
every battery sample.

## Results (UV-K1, 2026-09-27)

Test source: beacon-format generator (rpitx playing an IQ file) on 433.650 MHz,
long standard location test frame, 15-hex ID `1C7C2468ACFFBFF`. VFO FM, wide.

| Channel | Idle | A (carrier) | B (message) | Range in B | Verdict |
|---|---|---|---|---|---|
| PB1 (ch 9) | 0 | 0 | 0 | 0-0 | Tied to ground or a low node. Not usable. |
| **PA4 (ch 4)** | **518** | **1** | **125** | **165-894** | **Carries the RX audio.** |
| Registers 0x00-0x27 | | 0 | 0 | | Configuration registers, static |

Samples per window: n = 419 (about 1.9 kHz per channel with 8 registers + ADC
polled per round).

**Conclusion:** with the speaker on (AF output enabled, audio path on), the
demodulated RAW audio is present on **PA4** with about 0.6 V peak-to-peak around a
0.42 V bias, no clipping. An app can read it through ADC channel 4 with **no
hardware change and no firmware change**. Registers 0x28-0x7F were not scanned:
the PA4 path made it unnecessary.

Open point: the volume knob is analog (not in firmware). Whether it changes the
PA4 level depends on where PA4 joins the audio circuit; to be checked by turning
the knob down during a burst and watching the PA4 range.

## Screen

| Row | Content |
|---|---|
| 0 | `G3>4` results of group 3, next is 4 · `B` bursts · RSSI / floor |
| 1-4 | `RR a/b` per register: activity on carrier / message (whole LSB) |
| 5 | `PB1` or `PA4`: idle value, a/b, min-max in B, `n` samples per window |
| 7 | RAW, speaker, auto/manual group advance |

## Scope mode (key 5)

Added after the first on-air decode of the EPIRB 406 app gave a systematic error
pattern (see `../epirb406/README.md`). PA4 is sampled alone, paced exactly like
the decoder (9.6 kHz, one sample every 5000 cycles), for the same windows.
Rows 1-4 then show the AF DAC gain (REG_48 bits 3:0), min-max, activity and
sample count for the carrier and message windows, and how many message samples
clip at 0 or 4095. UP/DOWN step the AF DAC gain (0-15) to find a level that
does not clip. The TOP ranking of the register scan was removed to make room;
the scan itself is unchanged.

Keys (UV-K5 and UV-K1): UP/DOWN group (AF DAC gain in scope mode) · 1 auto-advance
· 2 speaker · 3 RAW on/off · 4 ADC probe PB1/PA4 · 5 scope mode · MENU clear ·
EXIT quit.

## Scope measurements (UV-K1, generator on 433.650 MHz)

| Setup | Carrier min-max | Message min-max | Message samples at 0 |
|---|---|---|---|
| RAW, AF gain 14 | 224-771 | 0-325 | 1082 / 2112 |
| RAW, AF gain 12 | 18-552 | 0-194 | 1765 / 2112 |
| RAW off, AF gain 15 | 225-952 | 0-398 | 1284 / 2112 |

At 9.6 kHz the level slides down as soon as fast sampling starts (the carrier
window drifts by ~500 LSB with almost no modulation) and the message sits on 0 V.
Polled at ~1.9 kHz (register mode) the same pin read 165-894 around 518, and
between bursts it reads ~1388. Neither the AF gain nor the chip's 300 Hz
high-pass (RAW off) removes it, so the cause is on the board, not in the chip.

Hypothesis: PA4 has no firm DC bias (audio reaches it through a coupling
capacitor), so each ADC conversion takes a little charge and at 9.6 kHz the ADC
drags the node towards 0 V. Test (v1.3, key 6 in scope mode): the MCU DAC on
PA4 with its output buffer off, at mid-scale (2048), acts as a weak bias
resistor to about VDD/2. The DAC clock (RCC APBENR1 bit 29) is enabled by the
app, and DAC, clock and pin mode are restored on exit.

## Version

`APP_VER` in `build.sh` is bumped for every build that goes on a radio. It is
compiled in (`-DAPP_VERSION`) and shown in the status-bar title (e.g. `v1.2`), since
the apps menu does not display versions. Current: **v1.3**.
