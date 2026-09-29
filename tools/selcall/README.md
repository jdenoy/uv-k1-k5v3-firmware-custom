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

## Build identification

The Labs preset sets `BUILD_TAG`, appended to the version shown on the welcome
screen: `ZV1` first ZVEI build, `ZV2` CHIRP window, `SC1` SelCall (ZVEI + CCIR).
It is display only: `VERSION_STRING_2` is compared with the stored version at
boot, and changing it resets the key/menu locks, `SET_KEY`, display inversion and
the boot-message lines.

Flash cost (Labs): +1,680 bytes (113,616 of 120,832, build SC1).
