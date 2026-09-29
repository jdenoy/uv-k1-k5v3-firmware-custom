# ZVEI selective calling (5-tone), on demand, per channel

Resident firmware feature (`ENABLE_FEAT_ZVEI`, on in the Labs and Custom
presets): each channel can hold a ZVEI type and two 5-digit codes, sent on
demand from a programmable key, typically to open a repeater's logic (e.g. the
"test" and "production" codes of Alpine repeaters). Nothing is sent
automatically.

## Use

1. Assign **ZVEI 1** and/or **ZVEI 2** to a programmable key (menu F1Shrt,
   F1Long, F2Shrt, F2Long or M Long).
2. On the channel, set the menus (category Channels):
   - **ZVEI**: OFF / ZVEI-1 / ZVEI-2
   - **ZV CD1**: code 1, 5 digits typed on the keypad (leading zeros kept)
   - **ZV CD2**: code 2
   To clear a code, step it to OFF with UP/DOWN (OFF comes after 99999 and
   before 00000).
3. Press the key: the radio keys up with the channel's normal TX settings
   (frequency, power, CTCSS/DCS), waits 300 ms, sends the code (5 x 70 ms) and
   unkeys. Double beep and no TX if the channel has no ZVEI type or no such code.

During a ZVEI burst the DTMF PTT-ID and the roger beep are not sent; the
CTCSS/DCS tail is. The DTMF side-tone setting (D ST) also controls the ZVEI
side tone.

## Tones

70 ms per tone, no gap. A digit equal to the previous tone is replaced by the
repeat tone.

| Digit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | Repeat |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ZVEI-1 (Hz) | 2400 | 1060 | 1160 | 1270 | 1400 | 1530 | 1670 | 1830 | 2000 | 2200 | 2600 |
| ZVEI-2 (Hz) | 2400 | 1060 | 1160 | 1270 | 1400 | 1530 | 1670 | 1830 | 2000 | 2200 | 970 |

Example: `11223` in ZVEI-1 is sent as 1060, 2600, 1160, 2600, 1270 Hz.
Source: sigidwiki "ZVEI Selcall" (ZVEI-1 and ZVEI-2 only differ by the repeat
tone).

## Storage

One 8-byte record per channel in each config bank, at physical address
`0x00B000 + index * 8` (3 sectors, `0x00B000-0x00DFFF`):

- index 0-1023: memory channels; 1024 + band * 2 + vfo: the 14 VFOs
- `[0]` type (0 off, 1 ZVEI-1, 2 ZVEI-2), `[1..3]` code 1 (24-bit LE),
  `[4..6]` code 2, `[7]` reserved; erased flash (`0xFF`) = off, no code

Physically this area is free in every bank (the bank uses `0x0000-0x886E`,
`0x9000-0x90E8`, `0xA000-0xA170`). Since build ZV2 it is also exposed to the
serial link through the EEPROM-compatible map (`driver/eeprom_compat.c`):
legacy addresses **`0xD000-0xF06F`** map to `0xB000-0xD06F` of the active bank.
This is how the CHIRP driver below reads and writes it.

- On the radio: saving a VFO to a memory copies the VFO's ZVEI record, deleting
  a channel clears it, a factory reset erases all records.
- Records are per config bank, like the channels (multiboot).
- With the original CHIRP driver the area is untouched (it uploads up to
  `0xA170`); it neither shows nor erases the ZVEI settings.

## CHIRP driver

`tools/zvei/chirp/f4hwn.chirp.v6.0.0-ZVEI1&2.py` is Armel's
`f4hwn.chirp.v6.0.0.py` (release v6.0.0) with ZVEI support. Load it in CHIRP with
*File > Load Module*. It appears in CHIRP as **UV-K1 & UV-K5 V3 (F4HWN)
ZVEI1&2**, next to the original: images saved with it carry that model name.

- Each channel's **Extra** tab (memories and VFOs) shows **ZVEI type** (OFF /
  ZVEI-1 / ZVEI-2), **ZVEI code 1** and **ZVEI code 2** (5 digits, or empty for
  none; anything else is refused).
- The key-action lists offer **ZVEI 1** and **ZVEI 2** (values 24 and 25).
- Download reads up to `0xF080` (the ZVEI window included); upload writes the
  usual area, the calibration if enabled, then the ZVEI window.
- Deleting a channel in CHIRP clears its ZVEI record, as on the radio. Copy and
  paste of a channel carries its ZVEI settings with the other extras.
- Images saved with the original driver (smaller) still open: they are padded
  with `0xFF`, i.e. ZVEI off. The original driver also opens images saved with
  this one.
- With a firmware without the ZVEI window (other presets, or ZVEI build ZV1),
  the window reads as `0xFF` (ZVEI off everywhere) and writes are ignored:
  nothing breaks, the settings are just not stored.

Tested against the CHIRP source (kk7ds/chirp, 2026-09-27) with the real `bitwise`
parser and settings classes: 17 scenarios (old image padding, extras on empty
and used channels, raw record layout, round trip, clearing, last memory and VFO
indexes, delete, validation of 3/6 digits and letters, key actions, upload areas
with calibration off, download size, and cross-compatibility with the original
driver). Not yet run inside the CHIRP application against a radio.
Scripts: `tools/zvei/chirp/test_chirp_zvei.py` and `test_compat_original.py`
(setup and usage in their headers).

## Side-key actions

`ACTION_OPT_ZVEI_1 = 24` and `ACTION_OPT_ZVEI_2 = 25` are appended to the
persisted action enum (never renumber). The CHIRP driver does not know these two
values yet: a key set to ZVEI on the radio may show as an unknown value in CHIRP.

## Tests

Host test of the tone sequence builder (`App/app/zvei_seq.c`, no hardware
dependency):

```
cc -I App -o /tmp/test_zvei tools/zvei/test_zvei_seq.c App/app/zvei_seq.c && /tmp/test_zvei
```

13 cases: plain code, repeat tone for ZVEI-1 and ZVEI-2, all-same digits,
leading zeros, and refusal of type OFF, invalid type, no code, code > 99999.

On air: receive the burst with an RTL-SDR and decode it with
`multimon-ng -a ZVEI1 -a ZVEI2 ...`; check the CTCSS with a second radio set to
the same tone (its squelch must open during the burst).

## Build identification

The Labs preset sets `BUILD_TAG` (`ZV1`: first ZVEI build; `ZV2`: ZVEI window
for CHIRP), appended to the version shown on
the welcome screen (`v6.0.0 ZV1`). It is display only: `VERSION_STRING_2` is
compared with the stored version at boot, and changing it resets the key/menu
locks, `SET_KEY`, display inversion and the boot-message lines.

Flash cost (Labs): +1,480 bytes (113,432 of 120,832, build ZV2).
