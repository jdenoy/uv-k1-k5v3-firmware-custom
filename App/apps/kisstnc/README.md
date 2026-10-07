# KISS TNC: APRSdroid over USB (feasibility study)

Goal: send and receive APRS frames on the UV-K1 / UV-K5 v3 from
[APRSdroid](https://github.com/ge0rg/aprsdroid), the phone doing the APRS
application (map, messages, beacons) and the radio acting as the TNC: Bell 202
AFSK 1200 bauds on air, KISS over the USB-C cable to the phone (USB OTG).

Status: **v0.2, RX and TX built and host-tested, not yet tried on the air**.
Step 0 (USB link, v0.1) works on the radio with APRSdroid.

## v0.2: the TNC

- **RX**: the APRS RX demodulator, unchanged. Every AX.25 frame with a good FCS
  goes to the host as a KISS data frame (FCS stripped, the copy from a second
  slicer dropped), sent in the next housekeeping slot (50 ms, between frames),
  never inside the 9.6 kHz sampling loop. Unlike APRS RX, no APRS-only UI check:
  a TNC passes every AX.25 frame.
- **TX**: KISS bytes are read from USB every 10 ms inside the sampling loop. A
  data frame waits for a clear channel (no slicer inside a preamble or a frame),
  then p-persistence (KISS P, default 63: one chance in four per 50 ms slot). It
  is sent with the APRS TX modulator: TXDELAY flags (40 = 267 ms by default, or
  KISS TXDELAY x 1.5), the frame bit-stuffed with its FCS, 3 tail flags, then
  back to RX. Tone level 66 (APRS TX's default), no twist.
- **Screen**: `USB` / `NO USB` / `TX` capsule, frames received and sent, the
  last station heard with its RSSI, frames lost, the VFO frequency. Keys: 1
  speaker (saved), 2 test frame to the host (link check without RF), EXIT.
- **Size**: 3,636 B of the 4,096 B overlay (88 %) + 234 B of assets. No
  division linked.

Tests (`test/test_tnc.py`, 15 checks, all pass): the app is built for the
computer against a mock API and mock MCU registers (`test/host_tnc.c`,
`test/host_hw.h`) and checked with APRS RX's channel model:

- RX: six frames (position, digipeated, message, 200 B, status, bad FCS) through
  the radio audio path (RAW and STD) at 0, 1500 and 3000 Hz of noise: the C app
  gives exactly the frames the Python reference decoder finds, never the bad one;
- TX: the bit stream rebuilt from the tone writes and their timing: 40 flags,
  the frame plus FCS exactly, 1200 / 2200 Hz only, KISS TXDELAY 30 -> 45 flags;
- loop: the C modulator's tones, through the channel model, decoded by the C
  demodulator, give back the host's frame (STD 0 and 1500 Hz noise, RAW).

Four deliberate bugs (bit stuffing, FCS stripping, CRC, space tone) are each
caught by the test.

### Trying it on the radio

1. Firmware `f4hwn.labs.bin` of this branch (`v6.1.0 KT1`), `KISSTNC.app`
   v0.2, VFO on 144.800 MHz FM, launch **KISS TNC**.
2. Receive with the computer first: `python3 test/kiss_host.py --listen 600`
   prints every frame heard. Then APRSdroid (TNC (KISS), USB serial): the
   stations appear on its map.
3. Transmit: APRSdroid "send position", or
   `python3 test/kiss_host.py --call F4WAT-7 --count 1 --info '>KISS TNC test'`.
   Check the frame with APRS RX on a second radio, multimon-ng, or aprs.fi.
   This keys the radio: a free APRS frequency, or a dummy load first.

### Known limits of v0.2

- One frame from the host at a time: a frame arriving while another waits for
  the channel is dropped (APRSdroid sends one frame per beacon).
- The channel check is the demodulator's DCD only: a voice carrier is not seen.
- The USB receive ring holds 256 B: read every 10 ms, plenty for APRSdroid, but
  a host sending more than ~250 B at once can overrun it.
- Tone level and twist are fixed (APRS TX defaults); a setting may follow if
  the deviation needs adjusting.
- v0.3 adds key 3, **RX only** (capsule `RX ONLY`, not saved): frames from the
  host are counted as lost, never transmitted. First field report (2026-10-07):
  APRSdroid drops the link ("KissReader out of data", a USB transfer error) as
  soon as tracking starts, when it sends its first beacon. Nothing in the TX
  path touches USB or interrupts, so RF into the USB cable at key-up is the
  suspect; RX only, then low power and a ferrite on the cable, tell it apart.

## Step 0: USB link test (v0.1)

What is in this branch:

- **Resident** (Labs, `BUILD_TAG` `KT1`, welcome screen `v6.1.0 KT1`): API level
  3 with `serial_read` / `serial_write` and capability `APP_CAP_SERIAL`
  (`App/apps/app_api.h`, `app_overlay.c`, `App/app/uart.c`, `App/driver/vcp.c`).
  Cost: +260 B flash (115,176 B, 5,656 B free), 0 B RAM.
- **App** `KISS TNC` v0.1 (`kisstnc_app.c`, 1,616 B): reads KISS from USB,
  decodes the frames, echoes each data frame back (key 1 toggles the echo), and
  key 2 sends a test frame `CALL>APZK1:>UV-K1 KISS TNC test` from the
  boot-message callsign. Screen: `USB OK` / `NO HOST` (DTR), bytes in, frames
  in / out, KISS errors, last source callsign and length, last 8 bytes in hex.
- **Tests**: a host test of the echo (replaced in v0.2 by `test/test_tnc.py`)
  and `test/kiss_host.py` (stand-in for APRSdroid on a Mac or Linux PC).

On the radio:

1. Flash `f4hwn.labs.bin` of this branch (the app needs API level 3: older
   firmware refuses it), check `v6.1.0 KT1` on the welcome screen.
2. Install `KISSTNC.app` (build/Apps/) as the other apps, launch **KISS TNC**.
3. Computer first: `python3 test/kiss_host.py --call F4WAT --count 3`. Expected:
   `USB OK` on the radio, three `TX` lines, the same three as `RX` lines (echo),
   and a `RX F4WAT>APZK1:>UV-K1 KISS TNC test` line after pressing 2.
4. Then APRSdroid: Preferences > Connection: Connection protocol **TNC
   (KISS)**, Connection type **USB serial** (baud rate: any, ignored), empty
   KISS init string. Plug the OTG cable, start APRSdroid tracking, accept the
   USB permission. Expected: the log shows "Opened CDCSerialDevice", each
   beacon APRSdroid sends comes back in its log as received (echo), key 2 adds
   the test frame.

## Ways to connect APRSdroid to the radio

| Option | Radio side | Phone side | Verdict |
|---|---|---|---|
| A. APRSdroid "AFSK (audio)" | Stock firmware, VOX on, audio cable to the K1 jack | APRSdroid's software modem, phone audio in/out | Works today, no code. Needs a cable with level matching, VOX keying (slow, clips the preamble), phone audio routing |
| B. **APRSdroid "TNC (KISS)" over USB** | **New overlay app: KISS TNC** (demod + mod on the radio) | USB OTG cable, APRSdroid USB backend | **Recommended**: digital link, proper PTT, the radio's own demodulator |
| C. KISS over Bluetooth | The radio has no Bluetooth (a serial BT module on the UART pins would be a hardware mod) | APRSdroid Bluetooth backend | Out of scope |
| D. TNC2 text mode over USB | Same as B with a text protocol | APRSdroid "TNC2" | No gain over KISS, more parsing on the radio |

## What was checked

### APRSdroid (ge0rg/aprsdroid, master 65fb5fa, 2025-12-22)

- USB backend `src/backend/UsbTnc.scala` uses the felhr UsbSerial library
  (`libs/usbserial-6.1.0-cdc.jar`). `requestPermissions()` takes the first
  device for which `UsbSerialDevice.isSupported(dev)` is true; generic **CDC ACM**
  devices are supported (`CDCSerialDevice`), whatever their VID/PID.
- `CDCSerialDevice` opens with `SET_CONTROL_LINE_STATE = 0x0003` (DTR and RTS
  on). This matters: the firmware only sends to the host while DTR is set.
- The baud rate setting (default 115200) is ignored by a CDC device.
- `res/xml/device_filter.xml` lists FTDI / CP210x / CH34x / PL2303 IDs only, so
  plugging the radio will **not** auto-start APRSdroid; start the connection by
  hand in APRSdroid (Connection: TNC (KISS), Link: USB serial).
- KISS (`src/tncproto/KissProto.scala`):
  - writes `C0 00 <AX.25 frame> C0` (data command 0, no FCS, the TNC adds it);
  - reads FEND-delimited frames, unescapes `DB DC` / `DB DD`, drops a leading
    `00` command byte, parses with `Parser.parseAX25`;
  - an optional init string (`kiss.init`) is sent as text lines: leave it
    empty;
  - the callsign must be 6 characters or fewer (F4WAT is fine).

### Firmware (feature_update_v6, v6.1.0)

- The radio already enumerates as a **USB CDC ACM** device (CherryUSB,
  `App/usb/usbd_cdc_if.c`, VID `0x36b7`, PID `0xFFFF`, "PUYA CDC DEMO"), the port
  CHIRP and K5Viewer use (`ENABLE_USB`).
- Reception: the USB interrupt (priority 3) fills `VCP_RxBuf[256]`, a ring
  with write index `VCP_RxBufPointer` (`App/driver/vcp.c`). It keeps running
  while an overlay app runs.
- The resident command parser reads that ring with its own static index
  (`VCP_ReadIndex`, `App/app/uart.c`) from the main loop, which does not run
  while an app runs.
- Transmission: `cdc_acm_data_send_with_dtr(buf, len)` sends only while DTR is
  set and busy-waits for the endpoint (with a timeout that marks the link down).
- The app API (`App/apps/app_api.h`, ABI 1, level 2) has **no serial service**:
  an overlay app cannot reach the USB port today.

### Existing APRS apps (sizes on v6.1.0, overlay budget 4,096 B)

| App | Code | Kept for a TNC | Dropped |
|---|---|---|---|
| APRS RX | 4,056 B | `listen` 920 (sampling, band-pass, correlators, AGC, 3 slicers + DPLL), `hdlcBit` 484 (NRZI, HDLC, FCS), `emit` 172, ADC/DAC setup ~150: **~1.75 KB** | Display, Mic-E and position decoding, symbols, scrolling: ~1.8 KB |
| APRS TX | 4,040 B | Bit stuffing + NRZI + FCS (from `build`), `sendBit`, `setTone`, `clkCyc`, the PTT sequence (`tx_state`, `tx_set_params`, `tx_tone`, `tx_mute`, `tx_end`): **~0.8 KB** | Position editor, frame builder from fields, display: ~3.2 KB |

Estimate for the KISS TNC app: 1.75 + 0.8 KB cores, ~0.25 KB KISS framing and
serial glue, ~0.3 KB minimal screen: **~3.1 KB**, about 1 KB of margin. The two
frame buffers (up to ~330 B each) live on `app_main`'s stack, as FoxHunt's
history, not in the overlay.

## Proposed design

### 1. Resident: a serial service for apps

Appended to `app_api_t` (next API level, or an optional capability bit such as
`APP_CAP_SERIAL` so builds without `ENABLE_USB` still load other apps):

```c
/* Bytes received on the USB serial port since the last call, up to len.
 * Shares the resident parser's read index, so bytes an app consumed are not
 * parsed as commands after it returns. */
uint16_t (*serial_read)(uint8_t *buf, uint16_t len);
/* Send len bytes; false when no host has the port open (DTR clear). */
bool     (*serial_write)(const uint8_t *buf, uint16_t len);
```

Both are thin wrappers (`VCP_RxBuf` ring copy with `VCP_ReadIndex`,
`cdc_acm_data_send_with_dtr`). On app exit the loader drops unread bytes, so a
half KISS frame never reaches the CHIRP command parser. K5Viewer reads the same
ring from each `blit_full` / `blit_line` of an app: every serial call re-arms
its lock (`gUART_LockK5Viewer`, the countdown CHIRP uses), so binary KISS data is
never taken for viewer commands (`55 AA 00 00` keep-alive, `AA 55 03 xx` key
injection), and its parser restarts past the app's bytes on exit. Measured
cost: +244 B of flash for the service, +16 B for the build tag.

The fields sit after the Labs system-information block, whose presence is
conditional: every preset that runs overlay apps (Labs) has it, so an app using
the port builds with `-DENABLE_FEAT_F4HWN_OVERLAY_INFO`, as SSTV does.

This changes the ABI shared with upstream: to be proposed to Armel (F4HWN)
before relying on it outside this fork.

### 2. Overlay app "KISS TNC" (`App/apps/kisstnc`)

- **RX**: the APRS RX demodulator unchanged (keep it in step with
  `aprsrx/test/model_rx.py`); each good frame (FCS checked, FCS stripped) goes
  out as `C0 00 <escaped frame> C0`. No UI-frame filter: a TNC passes every
  valid AX.25 frame.
- **TX**: KISS bytes are read between samples into a frame buffer (escapes
  undone, command byte checked: 0 = data, 1-6 = TXDELAY / P / SLOTTIME / TXTAIL
  / FULLDUPLEX / SETHW accepted and stored, others ignored). A complete frame
  is queued, then sent when the channel is clear: no slicer inside a preamble or
  a frame (the DCD the RX app already computes), p-persistence and slot time as
  KISS sets them. The sender adds the FCS, bit-stuffs, NRZI-encodes and keys the
  BK4829 tone generator as APRS TX does, TXDELAY flags before the frame.
- **Screen**: USB link state (DTR), frames received / sent, last RSSI, a TX
  indicator. Keys: EXIT quit, 1 speaker on/off (as APRS RX).
- Frequency, power and bandwidth come from the VFO, as in the other apps.

## Risks and open points

1. **Sampling jitter**: the USB interrupt can fire inside APRS RX's 9.6 kHz
   sampling loop (timed from SysTick). Short ISR, but to be measured with the
   model and on air; worst case, read/write USB only when no slicer is busy.
2. **Blocking send**: `cdc_acm_data_send_with_dtr` busy-waits. A frame of
   ~100 B is two 64 B packets, a few ms at full speed: send only between
   frames, never inside the sampling loop.
3. **Power**: with an OTG cable the phone supplies VBUS and the radio charges
   from the phone. A Y cable with external power, or accepting the drain.
4. **Overlay budget**: ~3.1 KB estimated, to be confirmed at the first build.
5. **Upstream ABI**: the serial service has to be agreed with upstream, or kept
   as a fork-only capability.
6. **Half duplex**: no RX while transmitting; frames arriving from the phone
   during a reception wait for the channel to clear.

## Plan

| Step | Content | Check |
|---|---|---|
| 0 | Resident `serial_read` / `serial_write` + a tiny **loopback app** (shows the bytes received, echoes them back). **Works on the radio with APRSdroid** | `test/kiss_host.py` on the Mac, then APRSdroid connects (log shows "Opened CDCSerialDevice") |
| 1 | **RX**: APRS RX demodulator + KISS out. **Built, host-tested (v0.2)** | APRSdroid shows stations heard on 144.800 MHz on its map; host test: model frames vs KISS bytes |
| 2 | **TX**: KISS in + APRS TX modulator, CSMA. **Built, host-tested (v0.2)** | APRSdroid beacon decoded by APRS RX on a second radio, multimon-ng, and seen on aprs.fi through a digipeater/iGate |
| 3 | KISS parameters, screen, size pass | Overlay size, on-air soak test |
