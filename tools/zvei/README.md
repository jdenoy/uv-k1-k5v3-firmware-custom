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

This area is outside the EEPROM-compatible map (`driver/eeprom_compat.c`):
the bank uses `0x0000-0x886E`, `0x9000-0x90E8`, `0xA000-0xA170`, and **CHIRP
never reads or writes it** (the driver uploads up to `0xA170`). Consequences:

- A CHIRP upload does not erase or change the ZVEI settings.
- CHIRP does not show them, and a channel moved or recreated in CHIRP keeps the
  ZVEI record of its slot number, not of its content.
- On the radio: saving a VFO to a memory copies the VFO's ZVEI record, deleting
  a channel clears it, a factory reset erases all records.
- Records are per config bank, like the channels (multiboot).

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

The Labs preset sets `BUILD_TAG` (e.g. `ZV1`), appended to the version shown on
the welcome screen (`v6.0.0 ZV1`). It is display only: `VERSION_STRING_2` is
compared with the stored version at boot, and changing it resets the key/menu
locks, `SET_KEY`, display inversion and the boot-message lines.

Flash cost (Labs): +1,472 bytes (113,408 of 120,832).
