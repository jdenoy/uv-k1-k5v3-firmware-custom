# POCSAG: on-radio pager message decoder (work in progress)

Overlay app that decodes POCSAG pager messages (ITU-R M.584, 512 / 1200 /
2400 bps) directly on the UV-K1 / UV-K5 v3, shows the RIC, function and text,
and beeps for every decoded message.

Status:

| Step | State |
|---|---|
| Decoder core (`pocsag.c`), host-tested on synthetic audio | **Done**: 39/39 tests pass |
| Radio app (`pocsag_app.c`: trigger, sampling, display, beep) | v1.0 run on the K1: garbage text. v1.1 (3,920 B) fixes the capture loop, to be tested |
| Bench test on the K1 | In progress: rpitx `pocsag -f 439987500 -r 1200`, recordings with POCSAG Rec (`../pocrec`) |

v1.0 on the radio (2026-09-29): only the `off` corner gave anything, and the
text was garbage. Two findings:

- The capture loop read the keyboard every 256 samples. The debounced scan takes
  0.4 ms or more (5 columns x 8 reads x 10 us), 4+ sample periods; the late
  samples were then read back to back, which smears about one bit per codeword
  (host emulation, `POC_HOLES=8`: 13 of 16 codewords needed a correction on a
  clean signal, so any noise made them uncorrectable). v1.1 no longer reads keys
  during a transmission (EXIT works between transmissions).
- A DC-coupled signal decodes with corners off and 30 Hz only (60 Hz and up
  fail), which matches what the radio showed: the K1 path looks close to
  DC-coupled. v1.1 defaults to `off`. A setting saved by v1.0 is kept: press 2
  until `AC off` if needed.

Receiving is legal; what you do with the content is not always. Pager traffic
that is not addressed to you or meant for the public (fire brigades, hospitals,
companies) is private correspondence in France (secret des correspondances):
do not record, publish or act on it. Use the app on amateur paging (DAPNET,
439.9875 MHz, 1200 bps), on your own pagers, or on a test transmitter.

## Using the app

1. Set the VFO to the paging frequency, **FM, wide bandwidth** (POCSAG uses
   +/-4.5 kHz deviation), e.g. 439.9875 MHz for DAPNET.
2. Launch **POCSAG** with no transmission in progress: the noise floor is
   measured at launch (key 5 measures it again).
3. Lower the volume: the RX audio must stay on (it is what reaches PA4), so the
   speaker plays each transmission.

A transmission is detected on RSSI (10 dB above the floor) and sampled until the
carrier drops (RSSI back below floor + 5 dB for about 100 ms), for at most 10 s
at a time on a continuous carrier. Keys are not read during a transmission. The screen is updated and the beeps played
(one per decoded message, 3 at most) when the sampling stops.

| Screen | Content |
|---|---|
| Line 0 | RIC, function (`F0`-`F3`), text type (`A` alphanumeric, `N` numeric, `T` tone only), message number / messages kept |
| Rows below | The text, 32 characters per row, 5 rows |
| Bottom row | Bit rate, AC-coupling corner (`AC60`), text mode (`?` auto, `A`, `N`), RSSI / floor, messages decoded (`#`), `fix` (a bit was corrected) or `BAD` (a codeword could not be corrected), `mute` |

Keys (UV-K5 and UV-K1):

| Key | Action |
|---|---|
| UP / DOWN | Browse the last 4 messages (newest first; direction follows the firmware's navigation setting) |
| 1 | Bit rate 512 / 1200 / 2400 |
| 2 | AC-coupling corner the decoder compensates: off / 30 / 60 / 100 / 150 / 250 Hz |
| 3 | Text: auto / alphanumeric / numeric |
| 4 | Beep on / off |
| 5 | Measure the noise floor again |
| MENU | Clear the messages and the counter |
| EXIT | Quit (also during a transmission) |

Bit rate, corner, text mode and beep are saved (committed when the app exits).
Auto text mode shows a message as alphanumeric unless more than a quarter of its
characters are not printable, then as numeric.

## Signal path and decoder

Same path as EPIRB 406 (see `../epirb406/README.md` and `../lab406/README.md`):
RAW receive (REG_2B bits 10/9/8, REG_73 bit 4: no HPF300, LPF3K, de-emphasis or
AFC), AF output to PA4, held at mid-scale by the MCU DAC (buffer off), ADC
channel 4 at 9.6 kHz timed from SysTick.

POCSAG is NRZ 2-FSK, so the discriminator output is the bit stream itself. The
decoder (`pocsag.c`, freestanding, no division):

1. **Droop compensation.** The audio reaches PA4 through AC coupling, which
   makes a run of equal bits sag towards the centre (at 512 bps with a 30 Hz
   corner, a 5-bit run loses most of its level). Quantized feedback rebuilds what
   the high-pass removed, a low-pass of the decided levels, and adds it back.
   Its corner is key 2 (default `off` since v1.1). On synthetic audio a 60 Hz
   setting decodes coupling from 30 to 150 Hz at every bit rate; 250 Hz coupling needs the 250
   setting; a DC-coupled chain needs `off`. The real K1 corner is not known yet.
2. **Bit clock.** Each bit is integrated over its slot; a DPLL on the zero
   crossings moves 1/8 of the phase error per crossing, rounded towards zero (a
   floored -1 once pushed the phase across the boundary and dropped a bit).
3. **Framing.** The sync codeword `0x7CD215D8` is searched in both polarities
   (2 bit errors accepted, 3 on the following batches), then 16 codewords per
   batch.
4. **Codewords.** BCH(31,21) syndrome plus even parity, one bit error corrected
   (the syndrome is matched against x^k mod g, no table). An uncorrectable
   address drops its message; an uncorrectable message codeword is kept and
   flags the message `BAD`.
5. **Messages.** RIC = 18 address bits and the 3-bit frame number; message
   bits kept raw (560 bits: 80 alphanumeric or 140 numeric characters, `trunc`
   beyond) and decoded as 7-bit ASCII or BCD (`0-9 * U space - ) (`) only when
   displayed. An idle codeword, the next address, a lost sync or the end of the
   carrier closes a message.

Space: the app uses 3,920 of the 4,096 bytes (v1.1). The text buffer (141 B)
lives on the stack in `draw()`, the waiting screen does not show the frequency,
and tone-only messages are marked `T` in the header instead of a text line.

## Host tests

`test/run_tests.sh` (python3 + numpy + scipy, a C compiler; multimon-ng if
installed) synthesizes what PA4 sees (`test/genpocsag.py`: FSK, carrier offset,
AWGN in 25 kHz, discriminator, audio low-pass, AC coupling, ADC at 9.6 kHz with
clock error), decodes it with `test/host_pocsag.c` and checks the messages:

- the three bit rates: clean, CNR 12 dB, inverted chain, 100 Hz AC coupling,
  +/-500 ppm bit clock;
- carrier offset +/-3 kHz, 3 kHz audio low-pass, 150 Hz coupling, half level;
- one bit error corrected, two errors flagged `BAD`, tone only, 3 transmissions,
  3 messages in one batch, truncation of a long message;
- no coupling with the corner off, 250 Hz coupling with the 250 setting at CNR
  12 dB (three bit rates), a combined worst case, CNR 11 dB over 4 noise seeds;
- the generator itself cross-checked with multimon-ng (same messages decoded).

## Bench test (to do)

1. Transmit test pages on 433.650 MHz with the rpitx generator (rpitx ships a
   `pocsag` tool) at 1200 bps, then 512 and 2400; check the message and the beep.
2. If messages come out `BAD` or not at all, try the corners (key 2): the one
   that decodes best tells us the real AC coupling of the K1 audio path. Then
   make it the default.
3. Check the RSSI trigger with a weak signal, and a long run of pages on a
   continuous carrier (10 s capture windows).

## Version

`APP_VER` in `build.sh` is bumped for every build that goes on a radio. It is
compiled in (`-DAPP_VERSION`) and shown in the status-bar title (e.g. `v1.0`).
Current: **v1.1**.
