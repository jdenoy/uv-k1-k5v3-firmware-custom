# ToneCall

ToneCall sends a sequential tone call (selective call, "selcall") on the
current channel: five tones or more sent back to back, in ZVEI-1, ZVEI-2,
CCIR-1 or CCIR-2. The call is chosen among up to 8 presets that the user
defines with the companion app **ToneCall Edit**. No code is built in: the
user stays responsible for what is sent, on frequencies where they are
allowed to transmit.

## Where it is used

The main use is **Canal E**, the Alpine emergency channel (161.300 MHz,
simplex, CTCSS 123 Hz on TX: SDIS 74 / GRA in Haute-Savoie, Rega in
Switzerland, the Italian side). `21414` raises an alarm at the dispatch centre
(acknowledged by 3 long tones), `21301` is a coverage test (2 long tones). Rega
accepts ZVEI-1 or ZVEI-2. The SDIS 74 rule: the code is sent **once**, at the
start of the call, repeated only if nobody answers, and **never with the PTT
during the conversation**. ToneCall follows it: the call is a separate action,
then the conversation goes on with the PTT on the channel.

5-tone sequences are not used to open amateur repeaters (1750 Hz or CTCSS
are); German/Austrian fire brigades still alert pagers and sirens with ZVEI
(receive only for us); CCIR is mostly professional PMR.

## Using it

1. Tune the channel the call goes on, with its CTCSS/DCS (123 Hz for Canal E).
2. Open ToneCall (`F` + `7`, pick its slot).
3. `UP`/`DOWN` selects a preset; `MENU` or `PTT` sends it.

| Screen | Key | Action |
|---|---|---|
| List | `UP` / `DOWN` | Select a preset |
| List | `MENU` / `PTT` | Send the selected preset |
| List | `EXIT` | Quit |
| Listen | `MENU` / `PTT` | Send the same preset again |
| Listen | `UP` / `DOWN` | Back to the list |
| Listen | `EXIT` | Quit, back to the channel |

The list shows the TX frequency on top, with the channel's TX tone on the right
(`CT123.0`, or `D023N` / `D023I` for DCS) when it has one, then the presets (number, name, code)
with the selected one highlighted and its type at the bottom. The last preset
used is selected at the next launch.

A call keys up with the channel's normal TX settings (frequency, power,
CTCSS/DCS), leaves 300 ms of carrier, sends the tones with no gap and unkeys.
The app then stays in RX on the same channel with the speaker following the
squelch, so the station's acknowledgement is heard ("RX" while the squelch is
open). `TX OFF` means the radio refused to transmit (TX lock, band, battery).

## Editing presets: ToneCall Edit

| Screen | Key | Action |
|---|---|---|
| Index | `UP` / `DOWN` | Select a preset |
| Index | `MENU` | Edit it |
| Index | `F` | Clear it |
| Index | `EXIT` | Save (if anything changed) and quit |
| Editor | `UP` / `DOWN` | Field: NAME, TYPE, CODE |
| Editor | `MENU` | Keep the preset, back to the index |
| Editor | `EXIT` | Discard the changes to this preset |

- **NAME** (up to 8 characters): number keys type letters by multi-tap
  (`2` ABC ... `9` WXYZ, `0` space, `1` `.-/`), `*` switches ABC/123, `F`
  deletes. An empty name shows `-` in the lists.
- **TYPE**: `*` cycles, `1`-`4` pick ZVEI-1, ZVEI-2, CCIR-1, CCIR-2.
- **CODE** (1 to 8 digits, leading zeros kept): number keys add a digit, `F`
  deletes. An empty code makes the preset empty.

The changes reach ToneCall when ToneCall Edit exits.

## Why two apps

ToneCall's presets are about 100 bytes. An overlay app's own config area holds
only `APP_CONFIG_LOCAL_MAX` (16) bytes, enough for the last preset used but not
for 8 named presets. The firmware's answer for larger settings is the
**configuration proxy** (firmware v6.2.0, `app_config_proxy.h`): an editor app
declares, in its own assets, a block of up to 117 bytes in another app's
assets; its `cfg_load`/`cfg_save` then read and stage that block, and the
loader writes it, with the target's asset CRC, after the editor exits. An app
cannot proxy its own assets (the loader skips the running slot), so the editor
has to be a separate app. This is the split APRS TX / APRS Edit already uses.

The alternatives were weighed and rejected:

- **One app, no names**, everything in the 16 local bytes: 6 presets of 5
  digits at most, shown as "ZVEI-2 21414". Too few, and unreadable in an
  emergency.
- **A larger config area**: a change to the slot layout and resident code.

The two-app split costs no resident code, keeps the sender small and quick to
use, and editing presets is rare compared with sending them.

Reinstalling ToneCall restores its bundled block: every preset empty.

## Files

- `tonecall_app.c`: the sender.
- `tonecall_seq.c/.h`: tone sequence builder, no hardware dependency (also
  linked into ToneCall Edit for the type names).
- `../tonecall_presets.h`: the preset block layout shared by both apps.
- `gen_assets.py`: ToneCall's assets (the empty preset block).
- `../tonecalledit/`: the editor, its proxy descriptor and its T9 key map.

## Tests

```sh
# tone sequences (host)
cd App/apps/tonecall && cc -o /tmp/test_tonecall test/test_tonecall_seq.c tonecall_seq.c && /tmp/test_tonecall
# proxy descriptor vs ToneCall's preset block
python3 App/apps/tonecalledit/test/check_assets.py
# build both apps
./compile-app.sh tonecall tonecalledit
```

On air, `rtl_fm` + `multimon-ng` decode the calls:

```sh
rtl_fm -f <freq>M -M fm -s 22050 - | multimon-ng -t raw -c -a ZVEI1 -a ZVEI2 -a CCIR -
```

multimon-ng prints the repeat tone as `E` (11223 shows as 1E2E3) and cannot
tell CCIR-1 from CCIR-2; `test/measure_tones.py` measures the tone durations on
a recording.

Requires firmware v6.2.0 or later (configuration proxy). Install ToneCall
before launching ToneCall Edit, which otherwise shows `TONECALL NOT FOUND`.
