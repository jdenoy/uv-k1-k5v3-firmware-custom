# KISS TNC: APRSdroid over USB (feasibility study)

Goal: send and receive APRS frames on the UV-K1 / UV-K5 v3 from
[APRSdroid](https://github.com/ge0rg/aprsdroid), the phone doing the APRS
application (map, messages, beacons) and the radio acting as the TNC: Bell 202
AFSK 1200 bauds on air, KISS over the USB-C cable to the phone (USB OTG).

Status: **study, no code yet**. Verdict: **feasible**, with one resident
change (a serial service in the app API) and one new overlay app built from the
APRS RX demodulator and the APRS TX modulator. Details and plan below.

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
`cdc_acm_data_send_with_dtr`), estimated 60 to 100 B of resident flash. On app
exit the loader should also drop unread bytes, so a half KISS frame never
reaches the CHIRP command parser.

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
| 0 | Resident `serial_read` / `serial_write` + a tiny **loopback app** (shows the bytes received, echoes them back) | `screen` / `minicom` on the Mac, then APRSdroid connects (log shows "Opened CDCSerialDevice") |
| 1 | **RX**: APRS RX demodulator + KISS out | APRSdroid shows stations heard on 144.800 MHz on its map; host test: model frames vs KISS bytes |
| 2 | **TX**: KISS in + APRS TX modulator, CSMA | APRSdroid beacon decoded by APRS RX on a second radio, multimon-ng, and seen on aprs.fi through a digipeater/iGate |
| 3 | KISS parameters, screen, size pass | Overlay size, on-air soak test |
