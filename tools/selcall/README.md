# SelCall: 5-tone selective calling (ZVEI-1/2, CCIR-1/2), on demand, per channel

Resident firmware feature (`ENABLE_FEAT_SELCALL`, on in the Labs and Custom
presets): each channel can hold a selcall type and two 5-digit codes, sent on
demand from a programmable key, typically to open a repeater's logic (e.g. the
"test" and "production" codes of Alpine repeaters). Nothing is sent
automatically.

## Use

1. Assign **SELCALL 1** and/or **SELCALL 2** to a programmable key (menu F1Shrt,
   F1Long, F2Shrt, F2Long or M Long).
2. On the channel, set the menus (category Channels):
   - **SelCal**: OFF / ZVEI-1 / ZVEI-2 / CCIR-1 / CCIR-2
   - **SC CD1**: code 1, 5 digits typed on the keypad (leading zeros kept)
   - **SC CD2**: code 2
   To clear a code, step it to OFF with UP/DOWN (OFF comes after 99999 and
   before 00000).
3. Press the key: the radio keys up with the channel's normal TX settings
   (frequency, power, CTCSS/DCS), waits 300 ms, sends the 5 tones and unkeys.
   Double beep and no TX if the channel has no selcall type or no such code.

During a selcall burst the DTMF PTT-ID and the roger beep are not sent (the
burst only opens the repeater logic); the CTCSS/DCS tail is. The DTMF side-tone
setting (D ST) also controls the selcall side tone.

## Tones

No gap between tones. A digit equal to the previous tone is replaced by the
repeat tone.

| Digit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | Repeat | Tone |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ZVEI-1 (Hz) | 2400 | 1060 | 1160 | 1270 | 1400 | 1530 | 1670 | 1830 | 2000 | 2200 | 2600 | 70 ms |
| ZVEI-2 (Hz) | 2400 | 1060 | 1160 | 1270 | 1400 | 1530 | 1670 | 1830 | 2000 | 2200 | 970 | 70 ms |
| CCIR-1 (Hz) | 1981 | 1124 | 1197 | 1275 | 1358 | 1446 | 1540 | 1640 | 1747 | 1860 | 2110 | 100 ms |
| CCIR-2 (Hz) | 1981 | 1124 | 1197 | 1275 | 1358 | 1446 | 1540 | 1640 | 1747 | 1860 | 2110 | 70 ms |

Example: `11223` in ZVEI-1 is sent as 1060, 2600, 1160, 2600, 1270 Hz; in CCIR
as 1124, 2110, 1197, 2110, 1275 Hz.
Sources: sigidwiki "ZVEI Selcall" and "CCIR Selcall", Wikipedia "CCIR (selcall)".

Cross-checked 2026-09-29 against the decoder tables of multimon-ng (commit
`0722194`, `demod_zvei1.c`, `demod_zvei2.c`, `demod_ccir.c`): digits 0-9 and
the repeat tone (index E) are identical for ZVEI-1, ZVEI-2 and CCIR. multimon-ng
prints a symbol at each tone change, whatever its duration, so CCIR-1 (100 ms)
and CCIR-2 (70 ms) both decode with `-a CCIR`, and the repeat tone is shown as
`E`: `11223` decodes as `1E2E3`.

## Storage

One 8-byte record per channel in each config bank, at physical address
`0x00B000 + index * 8` (3 sectors, `0x00B000-0x00DFFF`):

- index 0-1023: memory channels; 1024 + band * 2 + vfo: the 14 VFOs
- `[0]` type (0 off, 1 ZVEI-1, 2 ZVEI-2, 3 CCIR-1, 4 CCIR-2), `[1..3]` code 1
  (24-bit LE), `[4..6]` code 2, `[7]` reserved; erased flash (`0xFF`) = off, no
  code. Types 0-2 are those of the first ZVEI-only builds (ZV1, ZV2): records
  written by them stay valid.

Physically this area is free in every bank (the bank uses `0x0000-0x886E`,
`0x9000-0x90E8`, `0xA000-0xA170`). It is exposed to the serial link through the
EEPROM-compatible map (`driver/eeprom_compat.c`): legacy addresses
**`0xD000-0xF06F`** map to `0xB000-0xD06F` of the active bank. This is how the
CHIRP driver below reads and writes it.

- On the radio: saving a VFO to a memory copies the VFO's record, deleting a
  channel clears it, a factory reset erases all records.
- Records are per config bank, like the channels (multiboot).
- With the original CHIRP driver the area is untouched (it uploads up to
  `0xA170`); it neither shows nor erases the selcall settings.

## Side-key actions

`ACTION_OPT_SELCALL_1 = 24` and `ACTION_OPT_SELCALL_2 = 25` are appended to the
persisted action enum (never renumber). They were named ZVEI 1 / ZVEI 2 in the
ZV1/ZV2 builds: same values, keys already assigned keep working.

## CHIRP driver

`tools/selcall/chirp/f4hwn.chirp.v6.0.0-SelCall.py` is Armel's
`f4hwn.chirp.v6.0.0.py` (release v6.0.0) with selcall support. Load it in CHIRP
with *File > Load Module*. It appears as **UV-K1 & UV-K5 V3 (F4HWN) SELCALL**,
next to the original: images saved with it carry that model name.

- Each channel's **Extra** tab (memories and VFOs) shows **SelCall type**
  (OFF / ZVEI-1 / ZVEI-2 / CCIR-1 / CCIR-2), **SelCall code 1** and **SelCall
  code 2** (5 digits, or empty for none; anything else is refused).
- The key-action lists offer **SELCALL 1** and **SELCALL 2** (values 24 and 25).
- Download reads up to `0xF080` (the selcall window included); upload writes the
  usual area, the calibration if enabled, then the selcall window.
- Deleting a channel in CHIRP clears its record, as on the radio. Copy and paste
  of a channel carries its selcall settings with the other extras.
- Images saved with the original driver (smaller) still open: they are padded
  with `0xFF`, i.e. selcall off. The original driver also opens images saved with
  this one.
- With a firmware without the selcall window (other presets, or build ZV1), the
  window reads as `0xFF` (off everywhere) and writes are ignored: nothing breaks,
  the settings are just not stored.

Tested against the CHIRP source (kk7ds/chirp, 2026-09-27) with the real `bitwise`
parser and settings classes: 19 scenarios (old image padding, extras on empty
and used channels, raw record layout, round trip, clearing, last memory and VFO
indexes, a CCIR-2 record, delete, validation, key actions, upload areas with
calibration off, download size) plus cross-compatibility with the original
driver. The earlier ZVEI-only version was confirmed working in CHIRP with a
radio (download, settings, channels).
Scripts: `tools/selcall/chirp/test_chirp_selcall.py` and
`test_compat_original.py` (setup and usage in their headers).

## Tests

Host test of the tone sequence builder (`App/app/selcall_seq.c`, no hardware
dependency):

```
cc -I App -o /tmp/test_selcall tools/selcall/test_selcall_seq.c App/app/selcall_seq.c && /tmp/test_selcall
```

23 checks: plain codes, repeat tone for each standard, all-same digits, leading
zeros, refusal of type OFF, invalid type, no code, code > 99999, and the tone
duration of each type.

On air: receive the burst with an RTL-SDR and decode it with
`multimon-ng -a ZVEI1 -a ZVEI2 -a CCIR ...`; check the CTCSS with a second radio
set to the same tone (its squelch must open during the burst).

**On-air result (2026-09-29, UV-K1, build SC1):** bursts sent from the radio and
received with an RTL-SDR decode correctly in multimon-ng for ZVEI-1, ZVEI-2 and
CCIR. multimon-ng does not tell CCIR-1 from CCIR-2 (same tones, only the
duration differs: 100 ms vs 70 ms); the durations were then measured with
`measure_tones.py` (see below): CCIR-1 100 ms, CCIR-2 70 ms.
The channel's CTCSS is transmitted during the burst (checked with a second radio
set to the same tone): selcall tones and CTCSS OK on TX.

## Measuring tone durations and repeat tones

multimon-ng decodes the digits but does not measure time: it cannot tell
CCIR-1 (100 ms) from CCIR-2 (70 ms). `tools/selcall/measure_tones.py` (numpy)
reads the demodulated audio of a burst and lists each tone with its frequency,
its meaning in each standard and the spacing to the next tone, which is the tone
duration (no gap between selcall tones), then classifies the burst:

```
rtl_fm -f 433.650M -M fm -s 22050 - | tee burst.raw | multimon-ng -t raw -a CCIR -a ZVEI1 -a ZVEI2 -
tools/selcall/measure_tones.py burst.raw --rate 22050     # or a WAV file
```

Use a code with a doubled digit (e.g. `11223`) to see the repeat tone: 2600 Hz
for ZVEI-1, 970 Hz for ZVEI-2, 2110 Hz for CCIR. ZVEI-1 and ZVEI-2 have the same
duration (70 ms); only the repeat tone differs. With multimon-ng alone, a ZVEI-2
`11223` shows `1E2E3` with `-a ZVEI2` but `1C2C3` with `-a ZVEI1` (970 Hz is
ZVEI-1's C tone).

Checked on synthetic bursts (tones plus noise at 22.05 kHz): CCIR-1 12345 ->
100 ms, CCIR-2 11223 -> 70 ms with 2110 Hz repeats, ZVEI-1 / ZVEI-2 11223 -> 70 ms
with 2600 / 970 Hz repeats, each classified correctly. The detected length of a
tone reads 5-7 ms short (transition blur of the 20 ms window); the spacing does
not.

The first version found no tone in a real recording: rtl_fm outputs the FM
discriminator unfiltered, so most of the power was reception noise above 3 kHz
(plus the CTCSS), and the tones carried only 20-35 % of it. The audio is now
limited to 300-3000 Hz before analysis. Real burst from the UV-K1 (ZVEI code
87654): 5 tones at 69-72 ms spacing, 2000/1830/1670/1530/1400 Hz, first tone
about 300 ms after the carrier appears (the preload).

**On-air duration and repeat-tone test (2026-09-29, UV-K1, build SC1), code
`11223` with each type: all OK.** CCIR-1 measured at 100 ms per tone, CCIR-2 at
70 ms, both with 2110 Hz repeats; ZVEI-1 and ZVEI-2 at 70 ms with 2600 Hz and
970 Hz repeats respectively.

## Build identification

The Labs preset sets `BUILD_TAG`, appended to the version shown on the welcome
screen: `ZV1` first ZVEI build, `ZV2` CHIRP window, `SC1` SelCall (ZVEI + CCIR),
`SC2` SelCall on Armel's `feature_update_v6` (v6.1.0, branch `feature/selcall-v6`).
It is display only: `VERSION_STRING_2` is compared with the stored version at
boot, and changing it resets the key/menu locks, `SET_KEY`, display inversion and
the boot-message lines.

Flash cost (Labs): +1,680 bytes (113,616 of 120,832, build SC1).

## Port to feature_update_v6 (v6.1.0, build SC2)

Branch `feature/selcall-v6` = Armel's `feature_update_v6` + SelCall (net diff of
`feature/selcall`, one commit). Upstream had reworked the menus: the SelCall
cases sit next to `MENU_PTT_ID`, and `SELCALL 1` / `SELCALL 2` are entries of the
`SIDEFUNCTION_NAMES` X-macro (IDs 24/25 unchanged, so saved key settings stay
valid; availability through `ACTION_IsAvailable` / `action_opt_table`). The
selcall area (0xB000-0xDFFF per bank) and the EEPROM window (0xD000-0xF06F) are
still unused upstream (settings now end at 0xA178), and the new raw
dump/restore and cable AirCopy copy the whole bank, records included.

Then 128 bytes trimmed from the SelCall code only, same behaviour and record
format: flash cost +1,612 -> **+1,484 bytes** (Labs 116,220 of 120,832, RAM +32 B).

- Records handled raw (8 bytes): `SELCALL_Clear` writes an erased record,
  `SELCALL_Copy` copies the 8 bytes, a missing record (NOAA) reads as erased flash,
  so decoding has one path.
- The three menus are consecutive and map to fields (type, code 1, code 2):
  `SELCALL_GetField` / `SELCALL_SetField` replace the load / modify / save done
  in two places of `app/menu.c`; the type is shown with upstream's
  `choiceTable` pattern.
- `SELCALL_Pending` / `SELCALL_Cancel` are inline over `gSelCallPending`.
- The standards table has no entry for OFF and no digit-table pointer (the type
  gives ZVEI or CCIR).
- Tried and dropped (larger): a shared non-inlined `RecordIndex`.

To do: the CHIRP driver here derives from the v6.0.0 driver; v6.1.0 extends the
settings (mixed scan lists, 0xA170-0xA178), so it has to be rebased on Armel's
v6.1.0 driver before use with this firmware.
