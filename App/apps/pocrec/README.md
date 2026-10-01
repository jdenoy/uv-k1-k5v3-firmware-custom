# POCSAG Rec: record a POCSAG transmission from the radio

RX-only overlay app that records part of a POCSAG transmission exactly as the
POCSAG app sees it and sends it to the Mac over the programming cable, so the
decoder can be checked on real samples instead of a model. Derived from 406 Rec.

## What it records

Same receive setup as POCSAG and EPIRB 406: RAW RX (HPF300, LPF3K, de-emphasis,
AFC off), RX audio on, PA4 held at mid-scale by the MCU DAC (unbuffered, code
2048), ADC channel 4 at 9.6 kHz timed from SysTick.

- Start: `t0` ms after key 5, set with UP/DOWN (0-2000 ms, 100 ms steps,
  default 0). Send the page, then press 5 while it plays (rpitx sends it 4
  times, about 2 s in all). v1.0 started on an RSSI trigger 10 dB above the
  floor measured at launch; on the K1 the idle RSSI sat 11 dB above that floor,
  so it recorded noise over and over.
- Length: 3,700 samples = 385 ms (about 14 codewords at 1200 bps): what fits in
  the 4 KiB overlay next to the code.
- 4 bits per sample, linear code `lin48`: code n stands for
  `c + (n - 8) * 48 + 24` LSB, c = mean of the first 32 samples. Codes 0 and 15
  count as clipped; the raw minimum and maximum are sent as well. Linear rather
  than 406 Rec's `comp1` (fine near zero, coarse at the peaks): POCSAG sits on two
  levels, and their shape (droop, noise) is what we want to see.

## Serial output

USART1 as set up by the firmware (PA9/PA10, programming cable), 38400 8N1:

```
POCREC <ver> fs 9600 n 3700 t0 <ms> code lin48 c <c> min <min> max <max> rssi <dBm> clip <count>
<37 lines of 100 hex digits, one code per sample>
END
```

## Use

1. Close the app uploader (it holds the serial port).
2. On the Mac:
   `App/apps/pocrec/pocrec_capture.py --port /dev/cu.usbserial-XXXX --out ~/pocrec`
   (`--rate 512|2400` for other bit rates).
3. On the K1: VFO on the paging frequency, FM wide; launch **POCSAG Rec**;
   lower the volume. Send the page and press 5 while the speaker plays it.
4. Each recording is sent (`SEND` on screen: over the cable, the radio does not
   transmit), saved (`.txt`, `.u16`, `.png` plot) and
   analysed on the Mac: signal levels, the POCSAG host decoder at every
   AC-coupling corner (0, 60, 250, 1000, 1200, 1500 Hz), and multimon-ng on the same
   samples resampled to 22050 Hz (both polarities).

`pocrec_capture.py --analyse FILE.txt` redoes the analysis on a saved capture.
Checked offline on a synthetic `1234:test` recording quantized like the app:
decoded by the host decoder (corners 0 and 30 Hz) and by multimon-ng.

Keys (UV-K5 and UV-K1): 5 record · UP/DOWN start delay · 1 resend · EXIT quit.
The backlight stays on.

The first useful capture (v1.0, `../pocsag/test/k1/`) showed that the K1 audio
path is close to a differentiator (about 1 kHz high-pass); see the POCSAG
README.

## Version

`APP_VER` in `build.sh` is bumped for every build that goes on a radio and shown
in the status-bar title. Current: **v1.1**.
