# 406 Rec: record a 406 beacon burst from the radio

RX-only overlay app that records one burst exactly as EPIRB 406 sees it and sends
it to the Mac over the programming cable, so the decoder can be debugged on real
samples instead of a model.

## What it records

Same receive setup as EPIRB 406: RAW RX (HPF300, LPF3K, de-emphasis, AFC off),
RX audio on, PA4 held at mid-scale by the MCU DAC (unbuffered, code 2048), RSSI
trigger 10 dB above the floor measured at launch, ADC channel 4 at 9.6 kHz timed
from SysTick.

- Start: 80 ms after the trigger (the start of the carrier carries nothing).
- Length: 4,260 samples = 444 ms, so 80-524 ms after the trigger: end of the
  carrier, preamble, sync and the whole 144-bit frame. That is what fits in the
  4 KiB overlay next to the code.
- 4 bits per sample, non-linear code `comp1` around c, the mean of the first 32
  samples. Code n counts the thresholds at or below x - c
  (`-250 -165 -105 -63 -34 -15 -4 4 15 34 63 105 165 250 360`) and stands for
  `-300 -200 -130 -80 -45 -22 -8 0 8 22 45 80 130 200 300 420` LSB. Fine near zero,
  coarse at the peaks. Codes 0 and 15 are counted as clipped.

Why non-linear: on the host, a linear 4-bit code (step ~20 LSB, and truncating
instead of rounding) made synthetic bursts undecodable. With `comp1`, recordings
encoded exactly like the app decode 4/4 at 30, 15 and 12 dB CNR.

## Serial output

USART1 as set up by the firmware (PA9/PA10, programming cable), 38400 8N1:

```
REC406 <ver> fs 9600 n 4260 t0 80 code comp1 c <c> rssi <dBm> clip <count>
<43 lines of up to 100 hex digits, one code per sample>
END
```

## Use

1. Close the app uploader (it holds the serial port).
2. On the Mac:
   `App/apps/rec406/rec406_capture.py --port /dev/cu.usbserial-XXXX --out ~/rec406`
3. On the K1: VFO on the burst frequency, FM wide; launch **406 Rec** with no
   burst in progress; lower the volume.
4. Each burst is recorded, sent (about 1.2 s), saved as `.txt` and `.u16`, and
   decoded on the Mac with `host_dec406`. Key 1 resends the last recording.

`rec406_capture.py --convert FILE.txt` rebuilds the `.u16` from a saved capture.

Note: v1.0 sends no line break after the last, partial data line (4260 =
42 x 100 + 60), so `END` arrives glued to it; the capture script handles it. The
port is opened non-blocking: a blocking open on macOS received nothing. `--debug`
prints every received line.

Keys (UV-K5 and UV-K1): 1 resend · EXIT quit. The backlight stays on.

## Version

`APP_VER` in `build.sh` is bumped for every build that goes on a radio and shown
in the status-bar title. Current: **v1.0**.
