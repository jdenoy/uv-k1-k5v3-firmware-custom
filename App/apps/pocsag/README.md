# POCSAG: on-radio pager message decoder (work in progress)

Overlay app that decodes POCSAG pager messages (ITU-R M.584, 512 / 1200 /
2400 bps) directly on the UV-K1 / UV-K5 v3, shows the RIC, function and text,
and beeps for every decoded message.

Status:

| Step | State |
|---|---|
| Decoder core (`pocsag.c`), host-tested | **Done**: 48/48 tests, including a real K1 capture |
| Radio app (`pocsag_app.c`) | v1.2 (3,868 B) built, to be tested on the K1 |
| Bench test on the K1 | rpitx `pocsag -f 439987500 -r 1200`; a real capture decodes clean on the host |

### What the bench found (2026-09-29 to 10-01)

v1.0 and v1.1 showed garbage on the K1. Recording the audio with POCSAG Rec
(`../pocrec`) and decoding it on the Mac found four problems, all fixed in
v1.2:

1. **The K1 audio path at PA4 is close to a differentiator**: a high-pass around
   1-1.5 kHz. Inside a run of equal bits the level falls back to the centre in
   about 0.3 ms, so each bit edge comes out as a pulse. The droop compensation
   (key 2) now offers 1000 / 1200 / 1500 Hz and defaults to 1000 Hz: on the real
   capture 1000 to 1500 Hz decode clean, 800 Hz does not; on synthetic 1 kHz
   coupling at CNR 12 dB, 1000 Hz decodes 8/8 seeds, 1200 Hz 7/8.
2. **Bit clock dragged by ripple**: the audio also carries strong 1-3 kHz
   content, whose zero crossings inside a run of equal bits pulled the DPLL a
   whole bit off. The clock now only moves on real transitions (the two bits
   around the boundary differ), using the crossing nearest the boundary, and the
   correction is applied at mid-slot.
3. **RSSI trigger firing on noise**: idle RSSI sat at -81 dBm, 11 dB above the
   -92 dBm floor measured at launch, so the app kept sampling noise. v1.2 has no
   trigger: it decodes continuously and the sync word is the detector (10
   minutes of band-limited noise gave no false sync on the host).
4. **Keyboard scan in the capture loop** (fixed in v1.1): 0.4 ms+ per scan
   smeared about one bit per codeword.

Receiving is legal; what you do with the content is not always. Pager traffic
that is not addressed to you or meant for the public (fire brigades, hospitals,
companies) is private correspondence in France (secret des correspondances):
do not record, publish or act on it. Use the app on amateur paging (DAPNET,
439.9875 MHz, 1200 bps), on your own pagers, or on a test transmitter.

## Using the app

1. Set the VFO to the paging frequency, **FM, wide bandwidth** (POCSAG uses
   +/-4.5 kHz deviation), e.g. 439.9875 MHz for DAPNET.
2. Launch **POCSAG**.
3. Lower the volume: the RX audio must stay on (it is what reaches PA4), so the
   speaker plays each transmission.

The app samples continuously (no squelch, no RSSI trigger). About every 100 ms,
while no transmission is being decoded, it reads the keys and updates the
screen; during a transmission it does not, so keys answer once the page is
over (at most 10 s on a continuous transmission). The beeps (one per decoded
message, 3 at most) play when the transmission ends.

| Screen | Content |
|---|---|
| Line 0 | RIC, function (`F0`-`F3`), text type (`A` alphanumeric, `N` numeric, `T` tone only), message number / messages kept |
| Rows below | The text, 32 characters per row, 5 rows |
| Bottom row | Bit rate, AC-coupling corner (`AC1k`), text mode (`?` auto, `A`, `N`), RSSI, messages decoded (`#`), `fix` (a bit was corrected) or `BAD` (a codeword could not be corrected), `mute` |

Keys (UV-K5 and UV-K1):

| Key | Action |
|---|---|
| UP / DOWN | Browse the last 4 messages (newest first; direction follows the firmware's navigation setting) |
| 1 | Bit rate 512 / 1200 / 2400 |
| 2 | AC-coupling corner the decoder compensates: off / 60 / 250 / 1000 / 1200 / 1500 Hz (`AC1k` default; the K1 needs 1000-1500) |
| 3 | Text: auto / alphanumeric / numeric |
| 4 | Beep on / off |
| MENU | Clear the messages and the counter |
| EXIT | Quit (read between transmissions) |

Bit rate, corner, text mode and beep are saved (committed when the app exits).
v1.2 changed the corner table, so settings saved by v1.0/v1.1 are ignored.
Auto text mode shows a message as alphanumeric unless more than a quarter of its
characters are not printable, then as numeric.

## Signal path and decoder

Same path as EPIRB 406 (see `../epirb406/README.md` and `../lab406/README.md`):
RAW receive (REG_2B bits 10/9/8, REG_73 bit 4: no HPF300, LPF3K, de-emphasis or
AFC), AF output to PA4, held at mid-scale by the MCU DAC (buffer off), ADC
channel 4 at 9.6 kHz timed from SysTick.

POCSAG is NRZ 2-FSK, so the discriminator output is the bit stream itself. The
decoder (`pocsag.c`, freestanding, no division):

1. **Droop compensation.** The audio reaches PA4 through a high-pass (about
   1-1.5 kHz on the K1, see above), which makes a run of equal bits fall back
   to the centre. Quantized feedback rebuilds what the high-pass removed, a
   low-pass of the decided levels, and adds it back. Its corner is key 2.
2. **Bit clock.** Each bit is integrated over its slot. A DPLL keeps the slot
   boundaries on the transitions: the zero crossing nearest each boundary is
   kept, and the clock moves by 1/4 of its error only when the bits on both
   sides differ; the correction is applied at mid-slot so it never crosses a
   boundary.
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

Space: the app uses 3,868 of the 4,096 bytes (v1.2). The text buffer (141 B)
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
- K1-like coupling (1 and 1.5 kHz high-pass) with the 1000 Hz corner, three
  bit rates;
- a real capture from the K1 (`test/k1/`, POCSAG Rec v1.0, rpitx `1234:test`
  at 1200 bps): RIC 1234, F3, `test`, no bit corrected, at corners 1000, 1200
  and 1500 Hz;
- the generator itself cross-checked with multimon-ng (same messages decoded).

## Bench test (next)

1. Flash v1.2, set the VFO to 439.9875 MHz (FM wide), launch POCSAG, send
   `echo "1234:test" | sudo ./pocsag -f 439987500 -r 1200`: expect `1234 F3 A`,
   `test`, and a beep.
2. If not, try the corners 1200 and 1500 Hz (key 2), and record with POCSAG Rec
   (`../pocrec`, key 5 while the page plays) for analysis.
3. Then 512 and 2400 bps, a weak signal, and a long run of pages.

## Version

`APP_VER` in `build.sh` is bumped for every build that goes on a radio. It is
compiled in (`-DAPP_VERSION`) and shown in the status-bar title (e.g. `v1.0`).
Current: **v1.2**.
